#include "BookCreation.h"
#include "ActorLookup.h"
#include "BookManager.h"
#include "BookText.h"
#include "Database.h"
#include "DiaryDB.h"
#include "Localization.h"
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>
#include <unordered_map>

namespace SkyrimNetDiaries {

    // ── Serial DPF creation queue ────────────────────────────────────
    // DynamicPersistentForms.Create() is dispatched async to the Papyrus VM and
    // its callback runs on a VM worker thread.  DPF's FormID allocator is NOT
    // thread-safe: when many books are created at once (catch-up scan queues one
    // task per actor, each creating multiple volumes), concurrent callbacks get
    // handed the SAME FormID.  Two actors then share a book FormID, so opening
    // one shows the other's content (the cross-linked-diary bug).
    //
    // We serialize: only one Create() is ever in flight.  Each request is queued;
    // a request is dispatched only after the previous one's callback has fired
    // (DPF has finished allocating that FormID).  This removes the race entirely.
    struct PendingDiaryCreation {
        std::string actorUuid;
        std::string actorName;
        std::string bioTemplateName;
        std::string journalTemplate;
        double startTime = 0.0;
        double endTime = 0.0;
        double prevVolumeLastCreationTime = 0.0;
        int volumeNumber = 1;
        int prevVolumeCountAtBoundary = 0;
        RE::FormID targetActorFormID = 0;
        std::vector<DiaryEntry> entries;
        RE::TESObjectBOOK* templateBook = nullptr;
        int retryCount = 0;  // bumped when DPF hands back a colliding FormID
    };
    static std::mutex g_createQueueMutex;
    static std::deque<PendingDiaryCreation> g_createQueue;
    static bool g_createInFlight = false;

    // Synchronous FormID claim table.  When DPF.Create()'s callback returns a form,
    // we claim its FormID HERE (immediately, on the VM thread) — registration into
    // books_ is deferred to a game-thread task, so books_ can't be trusted for
    // collision detection in the callback.  If DPF hands back a FormID already
    // claimed by a DIFFERENT actor (its allocator recycled a duplicate deleted
    // record — see PumpDiaryCreateQueue comment), we reject that form and re-queue
    // the creation.  DPF's next AddForm consumes the duplicate slot and returns a
    // fresh ID, so retries both fix the content AND drain the poisoned pool.
    static std::mutex g_claimedFormIdMutex;
    static std::unordered_map<RE::FormID, std::string> g_claimedFormIds;  // FormID → owning UUID
    static constexpr int kMaxCreateRetries = 16;

    // DPF clones on VR have been seen with sourceFiles.array == 0x1, which crashes
    // TESForm::GetFile for anything that reads the description (item cards,
    // Description Framework, save serialization).  A value inside the first 64 KB
    // can never be a real pointer; nullptr is the state SE/AE clones already have
    // and GetFile null-checks.  Valid pointers are untouched, so SE/AE behaviour
    // does not change.  See docs/BOOK_FORMS.md.
    static bool ClearBogusSourceFiles(RE::TESForm* form) {
        const auto value = reinterpret_cast<std::uintptr_t>(form->sourceFiles.array);
        if (value == 0 || value >= 0x10000) return false;
        SKSE::log::debug("[DPF] Cleared invalid sourceFiles.array 0x{:X} on form 0x{:X}", value, form->GetFormID());
        form->sourceFiles.array = nullptr;
        return true;
    }

    static void PumpDiaryCreateQueue();  // defined after DPFCreateCallback

    // Custom callback functor to capture DPF.Create() return value
    class DPFCreateCallback : public RE::BSScript::IStackCallbackFunctor {
    public:
        DPFCreateCallback(std::string uuid, std::string actorName, double startTime, double endTime, int volumeNum, RE::FormID targetFormID,
                       std::vector<SkyrimNetDiaries::DiaryEntry> cachedEntries = {}, std::string bioTemplate = "", std::string journalTemplate = "",
                       double prevVolLastCreationTime = 0.0, int prevVolCountAtBoundary = 0,
                       RE::TESObjectBOOK* templateBook = nullptr, int retryCount = 0)
            : actorUuid(uuid), actorName(actorName), startTime(startTime), endTime(endTime), volumeNumber(volumeNum), targetActorFormID(targetFormID),
              diaryEntries(std::move(cachedEntries)), bioTemplateName(std::move(bioTemplate)), journalTemplateName(std::move(journalTemplate)),
              prevVolumeLastCreationTime(prevVolLastCreationTime), prevVolumeCountAtBoundary(prevVolCountAtBoundary),
              templateBook(templateBook), retryCount(retryCount) {}
        
        virtual void operator()(RE::BSScript::Variable a_result) override {
            // This DPF.Create() has completed — DPF has finished allocating this
            // FormID, so it's now safe to dispatch the next queued creation.  Done
            // FIRST so the early-return error paths below can't stall the queue.
            {
                std::lock_guard<std::mutex> lock(g_createQueueMutex);
                g_createInFlight = false;
            }
            SKSE::GetTaskInterface()->AddTask([]() { PumpDiaryCreateQueue(); });

            SKSE::log::debug("DPF.Create() callback invoked for {}", actorName);

            if (a_result.IsNoneObject() || !a_result.IsObject()) {
                SKSE::log::error("DPF.Create() returned None/non-object for {}", actorName);
                return;
            }
            
            auto* form = a_result.Unpack<RE::TESForm*>();
            if (!form || form->GetFormType() != RE::FormType::Book) {
                SKSE::log::error("DPF.Create() returned invalid form for {}", actorName);
                return;
            }
            
            auto* newBook = static_cast<RE::TESObjectBOOK*>(form);
            const RE::FormID newFormId = newBook->GetFormID();
            SKSE::log::info("DPF created book FormID 0x{:X} for {}", newFormId, actorName);

            // ── FormID collision detection ──────────────────────────────
            // Claim this FormID synchronously.  If it's already claimed by a
            // DIFFERENT actor, DPF's allocator recycled a duplicate deleted slot
            // and handed us a FormID that's already in use — registering it would
            // cross-link two actors' diaries.  Reject and re-queue; DPF's next
            // AddForm consumes the duplicate slot and returns a fresh ID.
            {
                std::lock_guard<std::mutex> lock(g_claimedFormIdMutex);
                auto it = g_claimedFormIds.find(newFormId);
                if (it != g_claimedFormIds.end() && it->second != actorUuid) {
                    // Collision.
                    if (retryCount < kMaxCreateRetries && templateBook) {
                        SKSE::log::warn("[DPF] FormID 0x{:X} collision: already owned by UUID {}, "
                                        "requested by {} — re-queuing (retry {})",
                                        newFormId, it->second, actorName, retryCount + 1);
                        PendingDiaryCreation retry;
                        retry.actorUuid                  = actorUuid;
                        retry.actorName                  = actorName;
                        retry.bioTemplateName            = bioTemplateName;
                        retry.journalTemplate            = journalTemplateName;
                        retry.startTime                  = startTime;
                        retry.endTime                    = endTime;
                        retry.prevVolumeLastCreationTime = prevVolumeLastCreationTime;
                        retry.volumeNumber               = volumeNumber;
                        retry.prevVolumeCountAtBoundary  = prevVolumeCountAtBoundary;
                        retry.targetActorFormID          = targetActorFormID;
                        retry.entries                    = diaryEntries;
                        retry.templateBook               = templateBook;
                        retry.retryCount                 = retryCount + 1;
                        {
                            std::lock_guard<std::mutex> qlock(g_createQueueMutex);
                            g_createQueue.push_back(std::move(retry));
                        }
                    } else {
                        SKSE::log::error("[DPF] FormID 0x{:X} collision for {} unresolved after {} retries "
                                         "— giving up (diary not created this pass)",
                                         newFormId, actorName, retryCount);
                    }
                    // The slot was already released at the top of operator(); the
                    // pumped queue will pick up the re-queued request.  Abandon this
                    // colliding form (never added to any inventory or books_).
                    return;
                }
                // Unclaimed (or already ours) — take it.
                g_claimedFormIds[newFormId] = actorUuid;
            }

            // Capture values by copy for the lambda
            std::string uuid = actorUuid;
            std::string name = actorName;
            double start = startTime;
            double end = endTime;
            int volume = volumeNumber;
            RE::FormID targetFormID = targetActorFormID;
            std::string bioTemplate = bioTemplateName;
            std::string journalTemplate = journalTemplateName;
            auto cachedEntries = diaryEntries;  // Copy diary entries for lambda capture
            double prevVolLastCT = prevVolumeLastCreationTime;
            int prevVolCountAtBoundary = prevVolumeCountAtBoundary;
            
            // Configure the book on the game thread
            SKSE::GetTaskInterface()->AddTask([newBook, uuid, name, start, end, volume, targetFormID, cachedEntries, bioTemplate, journalTemplate, prevVolLastCT, prevVolCountAtBoundary]() {
                auto bookManager = BookManager::GetSingleton();
                
                // Validate the book form before proceeding
                if (!newBook || newBook->GetFormType() != RE::FormType::Book) {
                    SKSE::log::error("Invalid book form in callback for {}", name);
                    return;
                }
                
                // Get template to copy properties
                auto templateBook = RE::TESForm::LookupByEditorID<RE::TESObjectBOOK>(journalTemplate.c_str());
                
                if (templateBook) {
                    // Copy basic properties
                    newBook->data.type = templateBook->data.type;
                    newBook->inventoryModel = templateBook->inventoryModel;
                    newBook->itemCardDescription = templateBook->itemCardDescription;
                    newBook->weight = 0.5f;
                    newBook->value = 0;

                    ClearBogusSourceFiles(newBook);

                    // Log the book type so we can verify it's a tome (0) not a scroll (0xFF).
                    // kNoteScroll books ignore [pagebreak] and render all text on one page,
                    // which would cause text to overflow and appear as overlapping content.
                    SKSE::log::info("[DPF] Template '{}' data.type=0x{:02X} (0x00=BookTome, 0xFF=NoteScroll)",
                        journalTemplate, static_cast<unsigned>(newBook->data.type.underlying()));
                    
                    // CRITICAL: Clear flags by setting to 0, then selectively set safe ones
                    // Do NOT copy flags from template
                    newBook->data.flags = static_cast<RE::OBJ_BOOK::Flag>(0);
                    newBook->data.flags.set(RE::OBJ_BOOK::Flag::kCantTake);
                    
                    // Template already has clean teaches data - don't touch it
                    // Clearing/nullifying causes DPF serializer to crash on save
                    
                    SKSE::log::debug("Book initialization complete with flags: 0x{:X}", newBook->data.flags.underlying());
                } else {
                    SKSE::log::error("Failed to find template book - using minimal initialization");
                    // Without template, zero out teaches structure but let DPF handle the rest
                    std::memset(&newBook->data.teaches, 0, sizeof(newBook->data.teaches));
                    newBook->data.flags.set(RE::OBJ_BOOK::Flag::kCantTake);
                    
                    SKSE::log::debug("Cleared teaches data without template");
                }
                
                // Set book name with volume number
                std::string bookName = Localization::GetSingleton()->FormatBookName(name, volume);
                newBook->SetFullName(bookName.c_str());
                
                SKSE::log::debug("Configured book: '{}'", bookName);
                
                // Format diary entries (use cached if available, otherwise fetch from database)
                std::vector<SkyrimNetDiaries::DiaryEntry> allEntries;
                
                if (!cachedEntries.empty()) {
                    // Use pre-cached entries (avoids database reopening)
                    SKSE::log::debug("Using {} pre-cached diary entries for {} (no DB access needed)", cachedEntries.size(), name);
                    allEntries = cachedEntries;
                } else {
                    // Fallback: fetch from API if entries weren't cached (rare)
                    try {
                        uint32_t actorFormId = SkyrimNetDiaries::Database::GetFormIDForUUID(uuid);
                        if (actorFormId != 0) {
                            allEntries = SkyrimNetDiaries::Database::GetDiaryEntries(actorFormId, 5000, start, 0.0, prevVolLastCT, prevVolCountAtBoundary);
                        }
                        SKSE::log::debug("Retrieved {} diary entries from API for {} (UUID: {}, startTime: {})", 
                                      allEntries.size(), name, uuid, start);
                    } catch (const std::exception& e) {
                        SKSE::log::error("API error fetching entries for {}: {}", name, e.what());
                    }
                }
                
                // Process entries (works for both cached and API-fetched)
                if (!allEntries.empty()) {
                    // Limit to configured max entries per book
                            const int MAX_ENTRIES_PER_BOOK = SkyrimNetDiaries::Config::GetSingleton()->GetEntriesPerVolume();
                            std::vector<SkyrimNetDiaries::DiaryEntry> entriesToFormat;
                            
                            // Take only the first max entries for this volume
                            int entriesToTake = std::min(static_cast<int>(allEntries.size()), MAX_ENTRIES_PER_BOOK);
                            entriesToFormat.assign(allEntries.begin(), allEntries.begin() + entriesToTake);
                            
                            std::string bookText = FormatDiaryEntries(entriesToFormat, name, start, end, MAX_ENTRIES_PER_BOOK);
                            
                            
                            SKSE::log::debug("Set dynamic text for '{}': {} diary entries (of {} total)", 
                                          bookName, entriesToTake, allEntries.size());
                            
                            // Calculate the actual endTime based on the last entry included in this volume
                            double volumeEndTime = end;  // Default to the passed endTime
                            if (!entriesToFormat.empty()) {
                                // Use the timestamp of the last entry included as the endTime for this volume
                                volumeEndTime = entriesToFormat.back().entry_date;
                                SKSE::log::debug("Volume {} endTime set to last entry timestamp: {}", volume, volumeEndTime);
                            }
                            
                            // Register book with the calculated endTime using FormID
                            bookManager->RegisterBook(uuid, name, newBook->GetFormID(), start, volumeEndTime, volume, journalTemplate, bioTemplate, prevVolLastCT, prevVolCountAtBoundary, targetFormID);

                            // Persist text to DB and warm in-memory cache.
                            DiaryDB::GetSingleton()->UpdateBookText(uuid, volume, bookText, entriesToTake);
                            auto* registeredVol = bookManager->GetBookForFormID(newBook->GetFormID());
                            if (registeredVol) {
                                registeredVol->cachedBookText = bookText;
                                registeredVol->lastKnownEntryCount = entriesToTake;
                            }
                            
                            // If there are more than max entries, log that additional volumes should be created
                            if (allEntries.size() > MAX_ENTRIES_PER_BOOK) {
                                SKSE::log::info("Note: Actor has {} entries total. Volume {} contains first {}. Additional entries will be in next volume when diary is stolen/returned.",
                                              allEntries.size(), volume, MAX_ENTRIES_PER_BOOK);
                            }
                            
                            // Add to NPC inventory immediately
                            // NOTE: DPF callbacks are async and may not be on the main thread
                            // We need to defer the inventory add to the main game thread
                            SKSE::log::debug("Attempting to add book to actor 0x{:X} ({})...", targetFormID, name);
                            
                            SKSE::GetTaskInterface()->AddTask([uuid, targetFormID, newBook, bookName, name = std::string(name), bioTemplate]() {
                                // FindActorForBook: player special-case, UUID-keyed cache, then
                                // UUID → live FormID, then stored FormID with a UUID back-check.
                                RE::Actor* targetActor = FindActorForBook(targetFormID, name, bioTemplate, uuid);

                                if (targetActor) {
                                    // Actor found - add book to inventory
                                    targetActor->AddObjectToContainer(newBook, nullptr, 1, nullptr);
                                    SKSE::log::info("✓ Added '{}' to {}'s inventory (actor 0x{:X}, kCantTake flag active)", bookName, name, targetFormID);
                                    
                                    // CRITICAL: Remove kCantTake flag after 5 seconds
                                    // This gives DPF time to fully register the form before it can be taken/saved
                                    // Prevents crash if player takes book and triggers autosave immediately
                                    std::thread([newBook, bookName]() {
                                        std::this_thread::sleep_for(std::chrono::seconds(5));
                                        SKSE::GetTaskInterface()->AddTask([newBook, bookName]() {
                                            if (newBook && newBook->GetFormType() == RE::FormType::Book) {
                                                newBook->data.flags.reset(RE::OBJ_BOOK::Flag::kCantTake);
                                                SKSE::log::debug("Removed kCantTake flag from '{}' - book is now safe to take", bookName);
                                            }
                                        });
                                    }).detach();
                                } else {
                                    SKSE::log::error("Failed to find target actor 0x{:X} ({}) for '{}' - actor lookup returned nullptr", 
                                                   targetFormID, name, bookName);
                                    SKSE::log::error("ESL FormID? {} - Book created but not added to inventory", 
                                                   (targetFormID & 0xFF000000) == 0xFE000000 ? "YES" : "NO");
                                }
                            });
                            
                            return; // Success - exit here
                } else {
                    SKSE::log::error("No diary entries available for '{}'", bookName);
                }
            });
        }
        
        virtual bool CanSave() const override { return false; }
        virtual void SetObject(const RE::BSTSmartPointer<RE::BSScript::Object>& a_object) override {}
        
    private:
        std::string actorUuid;
        std::string actorName;
        double startTime;
        double endTime;
        int volumeNumber;
        RE::FormID targetActorFormID;
        std::vector<SkyrimNetDiaries::DiaryEntry> diaryEntries;  // Cached diary entries to avoid DB reopening
        std::string bioTemplateName;  // For ESL actor lookup
        std::string journalTemplateName;  // Which journal template to use for this actor
        double prevVolumeLastCreationTime = 0.0;  // creation_time of last entry in previous volume (boundary de-dup)
        int prevVolumeCountAtBoundary = 0;           // how many prev-vol entries share the boundary date/CT
        RE::TESObjectBOOK* templateBook = nullptr;   // kept so a colliding create can be re-queued
        int retryCount = 0;                          // FormID-collision retry counter
    };

    void ClaimBookFormId(RE::FormID formId, const std::string& actorUuid) {
        std::lock_guard<std::mutex> lock(g_claimedFormIdMutex);
        g_claimedFormIds[formId] = actorUuid;
    }

    void ClearBookFormIdClaims() {
        std::lock_guard<std::mutex> lock(g_claimedFormIdMutex);
        g_claimedFormIds.clear();
    }

    RE::TESObjectBOOK* BookManager::CreateDiaryBook(const std::string& actorUuid, const std::string& actorName,
                                                     double startTime, double endTime, int volumeNumber, RE::FormID targetActorFormID,
                                                     const std::vector<DiaryEntry>& entries, const std::string& bioTemplateName,
                                                     double prevVolumeLastCreationTime, int prevVolumeCountAtBoundary) {
        // Select appropriate journal template for this actor
        std::string templateToUse = SelectJournalTemplate(actorUuid, actorName);
        
        if (templateToUse.empty()) {
            SKSE::log::error("BookManager not initialized - no template book set");
            return nullptr;
        }

        // Look the template up by EditorID.  This works with powerofthree's Tweaks or
        // Native EditorID Fix.  Don't scan books comparing GetFormEditorID(): that
        // vfunc returns "" for books unless Native EditorID Fix is installed.
        auto* templateBook = RE::TESForm::LookupByEditorID<RE::TESObjectBOOK>(templateToUse);

        if (!templateBook) {
            SKSE::log::error("Template book not found with Editor ID: {}", templateToUse);
            return nullptr;
        }

        // Enqueue the creation rather than dispatching immediately.  DPF.Create()
        // FormID allocation is not thread-safe, and many of these calls are issued
        // back-to-back (catch-up scan), so we serialize them through a global queue
        // and only ever have one Create() in flight.  See the queue comment near
        // the top of this file.
        PendingDiaryCreation req;
        req.actorUuid                  = actorUuid;
        req.actorName                  = actorName;
        req.bioTemplateName            = bioTemplateName;
        req.journalTemplate            = templateToUse;
        req.startTime                  = startTime;
        req.endTime                    = endTime;
        req.prevVolumeLastCreationTime = prevVolumeLastCreationTime;
        req.volumeNumber               = volumeNumber;
        req.prevVolumeCountAtBoundary  = prevVolumeCountAtBoundary;
        req.targetActorFormID          = targetActorFormID;
        req.entries                    = entries;
        req.templateBook               = templateBook;

        {
            std::lock_guard<std::mutex> lock(g_createQueueMutex);
            g_createQueue.push_back(std::move(req));
        }
        SKSE::log::debug("Queued DPF.Create() for {} (volume {}) — {} now pending",
                         actorName, volumeNumber, g_createQueue.size());

        PumpDiaryCreateQueue();

        // Creation is async (serialized) — the callback handles the rest.
        return nullptr;
    }

    // ── Serial DPF creation pump ─────────────────────────────────────
    // Dispatches the next queued creation, if any, when no Create() is in flight.
    // Called when a request is enqueued and again from each DPFCreateCallback once
    // the previous Create() has completed.  Guarantees one-at-a-time dispatch so
    // DPF's FormID allocator never races.
    void PumpDiaryCreateQueue() {
        PendingDiaryCreation req;
        {
            std::lock_guard<std::mutex> lock(g_createQueueMutex);
            if (g_createInFlight || g_createQueue.empty()) return;
            req = std::move(g_createQueue.front());
            g_createQueue.pop_front();
            g_createInFlight = true;
        }

        auto vm = RE::BSScript::Internal::VirtualMachine::GetSingleton();
        if (!vm || !req.templateBook) {
            SKSE::log::error("PumpDiaryCreateQueue: VM/template unavailable for {} — dropping and continuing",
                             req.actorName);
            {
                std::lock_guard<std::mutex> lock(g_createQueueMutex);
                g_createInFlight = false;
            }
            // Try the next one (this slot is dead, but others may be fine).
            SKSE::GetTaskInterface()->AddTask([]() { PumpDiaryCreateQueue(); });
            return;
        }

        auto callback = RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor>(
            new DPFCreateCallback(req.actorUuid, req.actorName, req.startTime, req.endTime,
                                  req.volumeNumber, req.targetActorFormID, req.entries,
                                  req.bioTemplateName, req.journalTemplate,
                                  req.prevVolumeLastCreationTime, req.prevVolumeCountAtBoundary,
                                  req.templateBook, req.retryCount));

        RE::TESForm* templatePtr = req.templateBook;
        auto createArgs = RE::MakeFunctionArguments(std::move(templatePtr));
        bool dispatched = vm->DispatchStaticCall("DynamicPersistentForms", "Create", createArgs, callback);

        if (!dispatched) {
            SKSE::log::error("DPF.Create() dispatch FAILED for {} — Dynamic Persistent Forms not loaded?",
                             req.actorName);
            std::lock_guard<std::mutex> lock(g_createQueueMutex);
            g_createInFlight = false;  // callback won't fire; unblock the queue
        }
    }

    void BookManager::SanitizeLoadedBookForms() {
        // Every book, not only DiaryDB-tracked volumes: DPF also restores diaries whose
        // rows are gone (orphaned by Reset or a rebuild, or when the DB failed to open).
        const auto& [map, lock] = RE::TESForm::GetAllForms();
        if (!map) return;
        int cleared = 0;
        {
            const RE::BSReadLockGuard guard{ lock };
            for (auto& [id, form] : *map) {
                if (form && form->GetFormType() == RE::FormType::Book && ClearBogusSourceFiles(form)) {
                    ++cleared;
                }
            }
        }
        if (cleared > 0) {
            SKSE::log::info("[DPF] Cleared invalid sourceFiles on {} book form(s)", cleared);
        }
    }

} // namespace SkyrimNetDiaries
