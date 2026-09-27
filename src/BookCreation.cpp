/*
 * SkyrimNet Physical Diaries - a Skyrim SKSE plugin that turns SkyrimNet NPC
 * diary entries into books you can find and read in the world.
 * Copyright (C) 2026 Zevick
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#include "BookCreation.h"
#include "ActorLookup.h"
#include "BookManager.h"
#include "Database.h"
#include "DiaryDB.h"
#include "Localization.h"
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>

namespace SkyrimNetDiaries {

    namespace {

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
        //
        // Every request carries the generation it was queued in.  A load or MCM Reset
        // (CancelPendingCreations) drops the queue and bumps the generation, so a
        // callback from before it is discarded instead of registering an old volume
        // into the new session.
        struct PendingDiaryCreation {
            std::string actorUuid;
            std::string actorName;
            std::string bioTemplateName;
            std::string journalTemplate;
            double startTime = 0.0;
            double prevVolumeLastCreationTime = 0.0;
            int volumeNumber = 1;
            int prevVolumeCountAtBoundary = 0;
            RE::FormID targetActorFormID = 0;
            std::vector<DiaryEntry> entries;          // this volume's entries, oldest first
            RE::TESObjectBOOK* templateBook = nullptr;  // ESP form, stable for the session
            std::uint32_t generation = 0;
            int retryCount = 0;                        // bumped when DPF hands back a colliding FormID
        };

        // All guarded by g_createQueueMutex.
        std::mutex g_createQueueMutex;
        std::deque<PendingDiaryCreation> g_createQueue;
        bool g_createInFlight = false;
        std::uint32_t g_createGeneration = 0;
        // Volumes queued or in flight per actor UUID, until registered in books_.
        // books_ doesn't show them yet, so without this a second trigger for the
        // same actor would create the same volumes again.
        std::unordered_map<std::string, int> g_pendingByUuid;

        // Synchronous FormID claim table.  When DPF.Create()'s callback returns a form,
        // we claim its FormID HERE (immediately, on the VM thread) — registration into
        // books_ is deferred to a game-thread task, so books_ can't be trusted for
        // collision detection in the callback.  If DPF hands back a FormID already
        // claimed by a DIFFERENT actor (its allocator recycled a duplicate deleted
        // record), we reject that form and re-queue the creation.  DPF's next AddForm
        // consumes the duplicate slot and returns a fresh ID, so retries both fix the
        // content AND drain the poisoned pool.
        std::mutex g_claimedFormIdMutex;
        std::unordered_map<RE::FormID, std::string> g_claimedFormIds;  // FormID → owning UUID
        constexpr int kMaxCreateRetries = 16;

        // DPF clones on VR have been seen with sourceFiles.array == 0x1, which crashes
        // TESForm::GetFile for anything that reads the description (item cards,
        // Description Framework, save serialization).  A value inside the first 64 KB
        // can never be a real pointer; nullptr is the state SE/AE clones already have
        // and GetFile null-checks.  Valid pointers are untouched, so SE/AE behaviour
        // does not change.  See docs/BOOK_FORMS.md.
        bool ClearBogusSourceFiles(RE::TESForm* form) {
            const auto value = reinterpret_cast<std::uintptr_t>(form->sourceFiles.array);
            if (value == 0 || value >= 0x10000) return false;
            SKSE::log::debug("[DPF] Cleared invalid sourceFiles.array 0x{:X} on form 0x{:X}", value, form->GetFormID());
            form->sourceFiles.array = nullptr;
            return true;
        }

        void PumpDiaryCreateQueue();

        // Call with g_createQueueMutex held.  Marks one of the actor's volumes as no
        // longer pending (registered, or given up on).
        void FinishPendingLocked(const PendingDiaryCreation& req) {
            if (req.generation != g_createGeneration) return;  // already cleared by a cancel
            auto it = g_pendingByUuid.find(req.actorUuid);
            if (it != g_pendingByUuid.end() && --it->second <= 0) {
                g_pendingByUuid.erase(it);
            }
        }

        void FinishPending(const PendingDiaryCreation& req) {
            std::lock_guard<std::mutex> lock(g_createQueueMutex);
            FinishPendingLocked(req);
        }

        bool IsCurrentGeneration(std::uint32_t generation) {
            std::lock_guard<std::mutex> lock(g_createQueueMutex);
            return generation == g_createGeneration;
        }

        // kCantTake stays on this long: taking the book and saving straight after
        // creation crashed before DPF had finished registering the form.
        constexpr auto kCantTakeDuration = std::chrono::seconds(5);

        void ClearCantTakeLater(RE::FormID bookId, std::string bookName) {
            std::thread([bookId, bookName = std::move(bookName)]() {
                std::this_thread::sleep_for(kCantTakeDuration);
                SKSE::GetTaskInterface()->AddTask([bookId, bookName]() {
                    if (auto* book = RE::TESForm::LookupByID<RE::TESObjectBOOK>(bookId)) {
                        book->data.flags.reset(RE::OBJ_BOOK::Flag::kCantTake);
                        SKSE::log::debug("Removed kCantTake flag from '{}' - book is now safe to take", bookName);
                    }
                });
            }).detach();
        }

        // Game thread: configure the new form from its template, register the volume,
        // write its text, and give the book to the NPC.
        void CompleteCreation(const PendingDiaryCreation& req, RE::FormID bookId) {
            if (!IsCurrentGeneration(req.generation)) {
                SKSE::log::info("[DPF] Discarding book 0x{:X} for {} vol {}: created before a load or Reset",
                                bookId, req.actorName, req.volumeNumber);
                return;
            }
            auto* newBook = RE::TESForm::LookupByID<RE::TESObjectBOOK>(bookId);
            if (!newBook || req.entries.empty()) {
                SKSE::log::error("[DPF] Book 0x{:X} for {} vol {} is gone or has no entries", bookId, req.actorName, req.volumeNumber);
                FinishPending(req);
                return;
            }

            const std::string bookName = Localization::GetSingleton()->FormatBookName(req.actorName, req.volumeNumber);
            ConfigureDiaryForm(newBook, req.templateBook, bookName);
            ClearBogusSourceFiles(newBook);
            newBook->data.flags.set(RE::OBJ_BOOK::Flag::kCantTake);
            SKSE::log::debug("[DPF] Template '{}' data.type=0x{:02X} (0x00=BookTome, 0xFF=NoteScroll)",
                             req.journalTemplate, static_cast<unsigned>(newBook->data.type.underlying()));

            auto* bookManager = BookManager::GetSingleton();
            DiaryBookData data;
            data.actorUuid = req.actorUuid;
            data.actorName = req.actorName;
            data.bookFormId = bookId;
            data.startTime = req.startTime;
            data.endTime = req.entries.back().entry_date;
            data.volumeNumber = req.volumeNumber;
            data.journalTemplate = req.journalTemplate;
            data.bioTemplateName = req.bioTemplateName;
            data.prevVolumeLastCreationTime = req.prevVolumeLastCreationTime;
            data.prevVolumeCountAtBoundary = req.prevVolumeCountAtBoundary;
            data.actorFormId = req.targetActorFormID;
            auto& registered = bookManager->RegisterBook(std::move(data));
            bookManager->SetVolumeText(registered, req.entries);
            FinishPending(req);

            // FindActorForBook: player special-case, UUID-keyed cache, then
            // UUID → live FormID, then stored FormID with a UUID back-check.
            RE::Actor* targetActor = FindActorForBook(req.targetActorFormID, req.actorName, req.bioTemplateName, req.actorUuid);
            if (!targetActor) {
                SKSE::log::error("Failed to find target actor 0x{:X} ({}) for '{}' - book created but not added to inventory",
                                 req.targetActorFormID, req.actorName, bookName);
                return;
            }
            targetActor->AddObjectToContainer(newBook, nullptr, 1, nullptr);
            SKSE::log::info("✓ Added '{}' to {}'s inventory (actor 0x{:X}, kCantTake flag active)",
                            bookName, req.actorName, req.targetActorFormID);
            ClearCantTakeLater(bookId, bookName);
        }

        // Receives the form DPF.Create() returned, on a VM thread.
        class DPFCreateCallback : public RE::BSScript::IStackCallbackFunctor {
        public:
            explicit DPFCreateCallback(PendingDiaryCreation a_req) : req_(std::move(a_req)) {}

            void operator()(RE::BSScript::Variable a_result) override {
                if (!IsCurrentGeneration(req_.generation)) {
                    // A load or Reset cancelled this request and may already have a new
                    // Create() in flight, so leave the in-flight slot alone.
                    SKSE::log::info("[DPF] Ignoring Create() result for {} vol {}: requested before a load or Reset",
                                    req_.actorName, req_.volumeNumber);
                    return;
                }

                RE::TESForm* form = nullptr;
                if (!a_result.IsNoneObject() && a_result.IsObject()) {
                    form = a_result.Unpack<RE::TESForm*>();
                }
                const bool valid = form && form->GetFormType() == RE::FormType::Book;
                const RE::FormID newFormId = valid ? form->GetFormID() : 0;

                // FormID collision check.  If DPF handed back a FormID that any volume
                // already owns (a recycled duplicate record), registering it would make
                // two volumes share one form, even for the same actor: retry instead.
                bool collision = false;
                std::string owner;
                if (valid) {
                    std::lock_guard<std::mutex> lock(g_claimedFormIdMutex);
                    auto it = g_claimedFormIds.find(newFormId);
                    if (it != g_claimedFormIds.end()) {
                        collision = true;
                        owner = it->second;
                    } else {
                        g_claimedFormIds[newFormId] = req_.actorUuid;
                    }
                }
                const bool retry = collision && req_.retryCount < kMaxCreateRetries;

                // Release the in-flight slot, and queue any retry at the front, in one
                // step so the next pump can't run in between and leave the retry waiting.
                {
                    std::lock_guard<std::mutex> lock(g_createQueueMutex);
                    if (req_.generation != g_createGeneration) return;
                    g_createInFlight = false;
                    if (retry) {
                        auto again = req_;
                        ++again.retryCount;
                        g_createQueue.push_front(std::move(again));
                    } else if (!valid || collision) {
                        FinishPendingLocked(req_);
                    }
                }
                SKSE::GetTaskInterface()->AddTask([]() { PumpDiaryCreateQueue(); });

                if (!valid) {
                    SKSE::log::error("DPF.Create() returned no book for {} vol {}", req_.actorName, req_.volumeNumber);
                    return;
                }
                if (collision) {
                    if (retry) {
                        SKSE::log::warn("[DPF] FormID 0x{:X} collision: already owned by UUID {}, requested by {} — re-queuing (retry {})",
                                        newFormId, owner, req_.actorName, req_.retryCount + 1);
                    } else {
                        SKSE::log::error("[DPF] FormID 0x{:X} collision for {} unresolved after {} retries — giving up (diary not created this pass)",
                                         newFormId, req_.actorName, req_.retryCount);
                    }
                    return;  // the colliding form is abandoned (never added anywhere)
                }

                SKSE::log::info("DPF created book FormID 0x{:X} for {}", newFormId, req_.actorName);
                SKSE::GetTaskInterface()->AddTask([req = req_, newFormId]() {
                    try {
                        CompleteCreation(req, newFormId);
                    } catch (const std::exception& e) {
                        SKSE::log::error("[DPF] CompleteCreation exception for {}: {}", req.actorName, e.what());
                        FinishPending(req);
                    } catch (...) {
                        SKSE::log::error("[DPF] CompleteCreation: unknown exception for {}", req.actorName);
                        FinishPending(req);
                    }
                });
            }

            bool CanSave() const override { return false; }
            void SetObject(const RE::BSTSmartPointer<RE::BSScript::Object>&) override {}

        private:
            PendingDiaryCreation req_;
        };

        // Dispatches the next queued creation, if any, when no Create() is in flight.
        // Called when a request is enqueued and again after each Create() completes.
        void PumpDiaryCreateQueue() {
            PendingDiaryCreation req;
            {
                std::lock_guard<std::mutex> lock(g_createQueueMutex);
                if (g_createInFlight || g_createQueue.empty()) return;
                req = std::move(g_createQueue.front());
                g_createQueue.pop_front();
                g_createInFlight = true;
            }

            auto* vm = RE::BSScript::Internal::VirtualMachine::GetSingleton();
            bool dispatched = false;
            if (vm) {
                auto callback = RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor>(new DPFCreateCallback(req));
                RE::TESForm* templatePtr = req.templateBook;
                // The VM copies the arguments while dispatching; we own and free them.
                std::unique_ptr<RE::BSScript::IFunctionArguments> args(RE::MakeFunctionArguments(std::move(templatePtr)));
                dispatched = vm->DispatchStaticCall("DynamicPersistentForms", "Create", args.get(), callback);
            }
            if (dispatched) return;

            SKSE::log::error("DPF.Create() dispatch FAILED for {} vol {} — Dynamic Persistent Forms not loaded?",
                             req.actorName, req.volumeNumber);
            {
                std::lock_guard<std::mutex> lock(g_createQueueMutex);
                if (req.generation == g_createGeneration) {
                    g_createInFlight = false;  // no callback will come
                    FinishPendingLocked(req);
                }
            }
            // Move on to the next request instead of leaving the queue stalled.
            SKSE::GetTaskInterface()->AddTask([]() { PumpDiaryCreateQueue(); });
        }

    } // namespace

    void ClaimBookFormId(RE::FormID formId, const std::string& actorUuid) {
        std::lock_guard<std::mutex> lock(g_claimedFormIdMutex);
        g_claimedFormIds[formId] = actorUuid;
    }

    void ClearBookFormIdClaims() {
        std::lock_guard<std::mutex> lock(g_claimedFormIdMutex);
        g_claimedFormIds.clear();
    }

    bool ConfigureDiaryForm(RE::TESObjectBOOK* book, const RE::TESObjectBOOK* templateBook,
                            const std::string& name) {
        // From the template: book type (must be a tome, 0x00; a note scroll, 0xFF,
        // ignores [pagebreak]), model and item card.  Flags are not copied.  Never
        // touch data.teaches: clearing it crashed DPF's serializer on save.
        if (templateBook) {
            book->data.type = templateBook->data.type;
            book->inventoryModel = templateBook->inventoryModel;
            book->itemCardDescription = templateBook->itemCardDescription;
        }
        book->weight = 0.5f;
        book->value = 0;
        book->data.flags = static_cast<RE::OBJ_BOOK::Flag>(0);

        const char* current = book->GetFullName();
        const bool renamed = !current || name != current;
        if (renamed) book->SetFullName(name.c_str());
        return renamed;
    }

    bool HasPendingCreations(const std::string& actorUuid) {
        std::lock_guard<std::mutex> lock(g_createQueueMutex);
        return g_pendingByUuid.contains(actorUuid);
    }

    void CancelPendingCreations() {
        std::size_t queued = 0;
        bool inFlight = false;
        {
            std::lock_guard<std::mutex> lock(g_createQueueMutex);
            queued = g_createQueue.size();
            inFlight = g_createInFlight;
            g_createQueue.clear();
            g_createInFlight = false;
            g_pendingByUuid.clear();
            ++g_createGeneration;
        }
        if (queued > 0 || inFlight) {
            SKSE::log::info("[DPF] Cancelled {} queued book creation(s){}", queued,
                            inFlight ? " and one in flight" : "");
        }
    }

    void BookManager::CreateDiaryBook(const std::string& actorUuid, const std::string& actorName,
                                      double startTime, int volumeNumber, RE::FormID targetActorFormID,
                                      const std::vector<DiaryEntry>& entries, const std::string& bioTemplateName,
                                      double prevVolumeLastCreationTime, int prevVolumeCountAtBoundary) {
        if (entries.empty()) return;

        const std::string templateToUse = SelectJournalTemplate(actorUuid, actorName, targetActorFormID);

        // Look the template up by EditorID.  This works with powerofthree's Tweaks or
        // Native EditorID Fix.  Don't scan books comparing GetFormEditorID(): that
        // vfunc returns "" for books unless Native EditorID Fix is installed.
        auto* templateBook = RE::TESForm::LookupByEditorID<RE::TESObjectBOOK>(templateToUse);
        if (!templateBook) {
            SKSE::log::error("Template book not found with Editor ID: '{}'", templateToUse);
            return;
        }

        PendingDiaryCreation req;
        req.actorUuid                  = actorUuid;
        req.actorName                  = actorName;
        req.bioTemplateName            = bioTemplateName;
        req.journalTemplate            = templateToUse;
        req.startTime                  = startTime;
        req.prevVolumeLastCreationTime = prevVolumeLastCreationTime;
        req.volumeNumber               = volumeNumber;
        req.prevVolumeCountAtBoundary  = prevVolumeCountAtBoundary;
        req.targetActorFormID          = targetActorFormID;
        req.entries                    = entries;
        req.templateBook               = templateBook;

        std::size_t pending = 0;
        {
            std::lock_guard<std::mutex> lock(g_createQueueMutex);
            req.generation = g_createGeneration;
            ++g_pendingByUuid[actorUuid];
            g_createQueue.push_back(std::move(req));
            pending = g_createQueue.size();
        }
        SKSE::log::debug("Queued DPF.Create() for {} (volume {}) — {} now queued", actorName, volumeNumber, pending);

        PumpDiaryCreateQueue();
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
