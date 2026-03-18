#include "BookManager.h"
#include "Database.h"
#include "DPF_API.h"
#include "DiaryDB.h"
#include <fstream>
#include <cstring>
#include <algorithm>
#include <cctype>
#include <mutex>
#include <unordered_map>

// External declarations from main.cpp
extern std::string FormatDiaryEntries(const std::vector<SkyrimNetDiaries::DiaryEntry>&, const std::string&, double, double, int maxEntries);

namespace SkyrimNetDiaries {

    // Actor lookup cache - persists for entire game session (FormIDs stable until game restart)
    static std::mutex g_actorCacheMutex;
    static std::unordered_map<RE::FormID, RE::Actor*> g_actorCache;

    // Custom callback functor to capture DPF.Create() return value
    class DPFCreateCallback : public RE::BSScript::IStackCallbackFunctor {
    public:
        DPFCreateCallback(std::string uuid, std::string actorName, double startTime, double endTime, int volumeNum, RE::FormID targetFormID,
                       std::vector<SkyrimNetDiaries::DiaryEntry> cachedEntries = {}, std::string bioTemplate = "", std::string journalTemplate = "",
                       double prevVolLastCreationTime = 0.0, int prevVolCountAtBoundary = 0) 
            : actorUuid(uuid), actorName(actorName), startTime(startTime), endTime(endTime), volumeNumber(volumeNum), targetActorFormID(targetFormID),
              diaryEntries(std::move(cachedEntries)), bioTemplateName(std::move(bioTemplate)), journalTemplateName(std::move(journalTemplate)),
              prevVolumeLastCreationTime(prevVolLastCreationTime), prevVolumeCountAtBoundary(prevVolCountAtBoundary) {}
        
        virtual void operator()(RE::BSScript::Variable a_result) override {
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
            SKSE::log::info("DPF created book FormID 0x{:X} for {}", newBook->GetFormID(), actorName);
            
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
                auto dataHandler = RE::TESDataHandler::GetSingleton();
                auto templateBook = RE::TESForm::LookupByEditorID<RE::TESObjectBOOK>(journalTemplate.c_str());
                
                if (templateBook) {
                    // Copy basic properties
                    newBook->data.type = templateBook->data.type;
                    newBook->inventoryModel = templateBook->inventoryModel;
                    newBook->itemCardDescription = templateBook->itemCardDescription;
                    newBook->weight = 0.5f;
                    newBook->value = 0;

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
                std::string bookName = name + "'s Diary";
                if (volume > 1) {
                    bookName += ", v" + std::to_string(volume);
                }
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
                            
                            SKSE::GetTaskInterface()->AddTask([targetFormID, newBook, bookName, name = std::string(name), bioTemplate]() {
                                RE::Actor* targetActor = nullptr;
                                
                                // Handle player diaries (UUID = "player_special")
                                if (bioTemplate == "player_special" || targetFormID == 0x14) {
                                    targetActor = RE::PlayerCharacter::GetSingleton();
                                    SKSE::log::debug("Player diary detected - assigning to player (FormID 0x14)");
                                } else {
                                
                                // Check cache first to avoid repeated expensive searches
                                bool foundInCache = false;
                                {
                                    std::lock_guard<std::mutex> lock(g_actorCacheMutex);
                                    auto it = g_actorCache.find(targetFormID);
                                    if (it != g_actorCache.end()) {
                                        targetActor = it->second;
                                        foundInCache = true;
                                        if (targetActor) {
                                            SKSE::log::debug("✓ Found actor 0x{:X} ({}) in cache - skipping search", targetFormID, name);
                                        }
                                    }
                                }
                                
                                // Only do expensive search if not in cache
                                if (!foundInCache) {
                                    SKSE::log::debug("Looking up actor for book '{}', targetFormID=0x{:X}, bioTemplate='{}'", bookName, targetFormID, bioTemplate);
                                
                                // For actors with bio_template_name, use name + last 3 digits matching
                                // bio_template_name format: "actorname_XYZ" where XYZ are last 3 hex digits of REFERENCE FormID
                                if (!bioTemplate.empty() && bioTemplate.find('_') != std::string::npos) {
                                    SKSE::log::debug("Using bio_template_name matching for: '{}'", bioTemplate);

                                    // Extract last 3 digits from bio_template_name (e.g., "fetri_el_874" -> "874")
                                    size_t lastUnderscore = bioTemplate.find_last_of('_');
                                    std::string expectedLast3;
                                    if (lastUnderscore != std::string::npos && lastUnderscore + 1 < bioTemplate.length()) {
                                        expectedLast3 = bioTemplate.substr(lastUnderscore + 1);
                                        // Convert to lowercase for case-insensitive matching
                                        std::transform(expectedLast3.begin(), expectedLast3.end(), expectedLast3.begin(), ::tolower);
                                        SKSE::log::debug("Searching for actor '{}' with REFERENCE FormID ending in '{}'", name, expectedLast3);
                                    }
                                    
                                    if (!expectedLast3.empty()) {
                                        // Search actor REFERENCES using ProcessLists (high/middle/low)
                                        auto processLists = RE::ProcessLists::GetSingleton();
                                        if (processLists) {
                                            int actorsChecked = 0;
                                            int nameMatches = 0;
                                            
                                            // Helper lambda to search an actor list
                                            auto searchActorList = [&](RE::BSTArray<RE::ActorHandle>& actorHandles) {
                                                for (auto& handle : actorHandles) {
                                                    // Use LookupByHandle (RELOCATION_ID 12204/12332) instead of
                                                    // handle.get() which uses BSPointerHandle::get() (ID 12785/12922)
                                                    // and crashes in VR if the address library entry is missing.
                                                    RE::NiPointer<RE::Actor> actorPtr;
                                                    if (!RE::Actor::LookupByHandle(handle.native_handle(), actorPtr)) {
                                                        continue;
                                                    }
                                                    auto* actorRef = actorPtr.get(); // NiPointer::get() - safe raw ptr
                                                    if (actorRef) {
                                                        auto actorBase = actorRef->GetActorBase();
                                                        if (actorBase) {
                                                            std::string engineName = actorBase->GetFullName();
                                                            uint32_t refFormID = actorRef->GetFormID();
                                                            
                                                            actorsChecked++;
                                                            
                                                            // Name check: exact match OR engine name is a suffix of
                                                            // the SkyrimNet display name.  The suffix check handles
                                                            // rank-prefixed names where SkyrimNet prepends a title
                                                            // (e.g. SkyrimNet "Jarl Elisif the Fair" vs engine
                                                            // "Elisif the Fair").
                                                            bool nameMatch = (engineName == name) ||
                                                                (!engineName.empty() &&
                                                                 name.size() > engineName.size() &&
                                                                 name.compare(name.size() - engineName.size(),
                                                                              engineName.size(), engineName) == 0);
                                                            if (nameMatch) {
                                                                nameMatches++;
                                                                // Get last 3 hex digits of REFERENCE FormID
                                                                char refLast3[4];
                                                                snprintf(refLast3, sizeof(refLast3), "%03x", refFormID & 0xFFF);
                                                                
                                                                SKSE::log::debug("  Found '{}' at 0x{:X} (last3: {})", engineName, refFormID, refLast3);
                                                                
                                                                // Match last 3 digits of reference FormID
                                                                if (expectedLast3 == refLast3) {
                                                                    targetActor = actorRef;
                                                                    SKSE::log::debug("✓ Matched! Found actor via bio_template_name: '{}' (0x{:X})", engineName, refFormID);
                                                                    return true; // Found!
                                                                }
                                                            }
                                                        }
                                                    }
                                                }
                                                return false;
                                            };
                                            
                                            // Search high, middle, and low process actors
                                            if (!searchActorList(processLists->highActorHandles)) {
                                                if (!searchActorList(processLists->middleHighActorHandles)) {
                                                    if (!searchActorList(processLists->middleLowActorHandles)) {
                                                        searchActorList(processLists->lowActorHandles);
                                                    }
                                                }
                                            }
                                            
                                            if (!targetActor) {
                                                SKSE::log::error("Actor lookup failed: checked {} actors, found {} with name '{}'", actorsChecked, nameMatches, name);
                                                SKSE::log::error("Looking for: name='{}', last3='{}', bioTemplate='{}'", name, expectedLast3, bioTemplate);
                                            }
                                        }
                                    } else {
                                        SKSE::log::warn("Could not extract last 3 digits from bio_template_name: '{}'", bioTemplate);
                                    }
                                } else {
                                    // No bio_template_name - use FormID lookup (vanilla actors)
                                    SKSE::log::debug("No bio_template_name, using FormID lookup for 0x{:X}", targetFormID);
                                    targetActor = RE::TESForm::LookupByID<RE::Actor>(targetFormID);
                                }
                                
                                    // Cache the result (even if nullptr) to avoid repeating search
                                    {
                                        std::lock_guard<std::mutex> lock(g_actorCacheMutex);
                                        g_actorCache[targetFormID] = targetActor;
                                        if (targetActor) {
                                            SKSE::log::debug("✓ Cached actor 0x{:X} ({}) for subsequent volumes", targetFormID, name);
                                        } else {
                                            SKSE::log::debug("Cached nullptr for actor 0x{:X} to avoid repeated searches", targetFormID);
                                        }
                                    }
                                } // End of !foundInCache block
                                } // End of player check
                                
                                actor_resolved:
                                if (targetActor) {
                                    // Actor found - add book to inventory
                                    // If found in ProcessLists, they're loaded enough to manipulate inventory
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
    };

    BookManager* BookManager::GetSingleton() {
        static BookManager singleton;
        return &singleton;
    }

    void BookManager::Initialize(const std::string& baseTemplate, 
                                 const std::string& journal01,
                                 const std::string& journal02,
                                 const std::string& journal03,
                                 const std::string& journal04,
                                 const std::string& nightingaleJournal) {
        templateBookEditorId_ = baseTemplate;
        
        // Store journal variants (include base template for more variety)
        journalTemplates_.push_back(baseTemplate);
        if (!journal01.empty()) journalTemplates_.push_back(journal01);
        if (!journal02.empty()) journalTemplates_.push_back(journal02);
        if (!journal03.empty()) journalTemplates_.push_back(journal03);
        if (!journal04.empty()) journalTemplates_.push_back(journal04);
        
        nightingaleTemplate_ = nightingaleJournal;
        
        // Log initialization
        if (!journalTemplates_.empty() && !nightingaleTemplate_.empty()) {
            SKSE::log::info("BookManager initialized with base template: {}, {} journal variants, Nightingale: {}",
                           baseTemplate, journalTemplates_.size(), nightingaleTemplate_);
        } else if (!journalTemplates_.empty()) {
            SKSE::log::info("BookManager initialized with base template: {}, {} journal variants",
                           baseTemplate, journalTemplates_.size());
        } else if (!nightingaleTemplate_.empty()) {
            SKSE::log::info("BookManager initialized with base template: {}, Nightingale: {}",
                           baseTemplate, nightingaleTemplate_);
        } else {
            SKSE::log::info("BookManager initialized with base template: {}", baseTemplate);
        }
    }

    std::string BookManager::SelectJournalTemplate(const std::string& actorUuid, const std::string& actorName) {
        // Check if we already selected a template for this actor
        auto it = actorTemplates_.find(actorUuid);
        if (it != actorTemplates_.end()) {
            SKSE::log::debug("Using cached journal template for {}: {}", actorName, it->second);
            return it->second;
        }
        
        std::string selectedTemplate;
        
        // Check for Nightingale NPCs (special journal)
        if (!nightingaleTemplate_.empty()) {
            if (actorName == "Karliah" || actorName == "Gallus" || actorName == "Mercer Frey") {
                selectedTemplate = nightingaleTemplate_;
                SKSE::log::debug("Selected Nightingale journal for {}", actorName);
                actorTemplates_[actorUuid] = selectedTemplate;
                return selectedTemplate;
            }
        }
        
        // If no variants configured, use base template
        if (journalTemplates_.empty()) {
            selectedTemplate = templateBookEditorId_;
            SKSE::log::debug("No journal variants - using base template for {}", actorName);
            actorTemplates_[actorUuid] = selectedTemplate;
            return selectedTemplate;
        }
        
        // Randomly select from available variants
        auto* rng = RE::BGSDefaultObjectManager::GetSingleton();
        if (rng) {
            // Use actor UUID as seed for deterministic "randomness" (consistent across reloads)
            std::hash<std::string> hasher;
            size_t seed = hasher(actorUuid);
            size_t index = seed % journalTemplates_.size();
            selectedTemplate = journalTemplates_[index];
        } else {
            // Fallback: just use first variant
            selectedTemplate = journalTemplates_[0];
        }
        
        SKSE::log::debug("Selected journal template for {}: {}", actorName, selectedTemplate);
        actorTemplates_[actorUuid] = selectedTemplate;
        DiaryDB::GetSingleton()->UpsertActorTemplate(actorUuid, selectedTemplate);
        return selectedTemplate;
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

        // Get template book by Editor ID
        auto dataHandler = RE::TESDataHandler::GetSingleton();
        if (!dataHandler) {
            SKSE::log::error("Failed to get TESDataHandler");
            return nullptr;
        }

        SKSE::log::debug("Looking for template book with Editor ID: {}", templateToUse);
        
        // Find book by Editor ID - need to iterate since there's no direct lookup
        RE::TESObjectBOOK* templateBook = nullptr;
        auto& books = dataHandler->GetFormArray<RE::TESObjectBOOK>();
        for (auto book : books) {
            if (book && book->GetFormEditorID()) {
                std::string editorId = book->GetFormEditorID();
                if (editorId == templateToUse) {
                    templateBook = book;
                    SKSE::log::debug("Found template book: {} (FormID: 0x{:X})", 
                        book->GetName(), 
                        book->GetFormID());
                    break;
                }
            }
        }
        
        if (!templateBook) {
            SKSE::log::error("Template book not found with Editor ID: {}", templateToUse);
            return nullptr;
        }

        // Use DynamicPersistentForms.Create() - this is async so we dispatch and handle result in callback
        auto vm = RE::BSScript::Internal::VirtualMachine::GetSingleton();
        if (!vm) {
            SKSE::log::error("VM not available - cannot create books!");
            return nullptr;
        }

        SKSE::log::debug("Dispatching async DPF.Create() for {} (volume {}) with {} cached entries...", actorName, volumeNumber, entries.size());
        
        // Create callback that will handle the created book asynchronously
        auto callback = RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor>(
            new DPFCreateCallback(actorUuid, actorName, startTime, endTime, volumeNumber, targetActorFormID, entries, bioTemplateName, templateToUse, prevVolumeLastCreationTime, prevVolumeCountAtBoundary)
        );
        
        RE::TESForm* templatePtr = templateBook;
        auto createArgs = RE::MakeFunctionArguments(std::move(templatePtr));
        
        bool dispatched = vm->DispatchStaticCall("DynamicPersistentForms", "Create", createArgs, callback);
        
        if (!dispatched) {
            SKSE::log::error("DPF.Create() dispatch FAILED - Dynamic Persistent Forms mod is NOT loaded!");
            return nullptr;
        }
        
        SKSE::log::debug("DPF.Create() dispatched - book will be created asynchronously");
        
        // Return nullptr since creation is async - the callback will handle everything
        return nullptr;
    }

    bool BookManager::UpdateBookText(RE::TESObjectBOOK* book, const std::string& actorUuid,
                                     double startTime, double endTime) {
        // TODO: Get database path from somewhere global
        // For now, just set placeholder text
        std::string bookText = "<font face='$HandwrittenFont' size='18'>";
        bookText += "Diary of " + actorUuid + "\n\n";
        bookText += "Entries from time " + std::to_string(startTime);
        bookText += " to " + (endTime == 0.0 ? "present" : std::to_string(endTime));
        bookText += "\n\n[Diary entries will be loaded from database]";
        bookText += "</font>";

        // For now, we'll need to use the item card description (CNAM field)
        // The actual book text rendering happens in the BookMenu but we can't directly set it
        // We'll store the text in the description for now as a placeholder
        // TODO: Investigate hooking into the BookMenu directly to inject text
        SKSE::log::warn("UpdateBookText called - book text rendering not fully implemented yet");
        SKSE::log::debug("Would set text: {}", bookText);
        
        return true;
    }

    void BookManager::SetBookText(RE::TESObjectBOOK* book, const std::string& text) {
        if (!book) {
            SKSE::log::error("SetBookText: null book");
            return;
        }

        // Directly write the book description text
        // This is the text that BookMenu displays
        // We use a memory write since BGSLocalizedStringDL is just an ID
        book->descriptionText.id = 0;  // Clear localization ID
        
        // The actual text is stored in a string table, but we can temporarily
        // override it by setting a custom string. Unfortunately CommonLibSSE
        // doesn't expose direct text setting for localized strings.
        // We need to use the itemCardDescription instead
        book->itemCardDescription.descriptionText.id = 0;
        
        SKSE::log::debug("Set book text ({} characters) for book 0x{:X}", text.length(), book->GetFormID());
        SKSE::log::warn("Note: Book text injection via memory is limited - text may not display");
    }

    DiaryBookData* BookManager::GetBookForActor(const std::string& actorUuid) {
        auto it = books_.find(actorUuid);
        if (it == books_.end() || it->second.empty()) {
            return nullptr;
        }
        // Return the latest (last) volume
        return &it->second.back();
    }

    std::vector<DiaryBookData>* BookManager::GetAllVolumesForActor(const std::string& actorUuid) {
        auto it = books_.find(actorUuid);
        if (it == books_.end()) {
            return nullptr;
        }
        return &it->second;
    }

    DiaryBookData* BookManager::GetBookForFormID(RE::FormID formId) {
        // Search all actors' volumes for matching FormID
        for (auto& [uuid, volumes] : books_) {
            for (auto& book : volumes) {
                if (book.bookFormId == formId) {
                    return &book;
                }
            }
        }
        return nullptr;
    }

    std::optional<DiaryBookData> BookManager::GetBookByFormID(RE::FormID formId) {
        // Search all actors' volumes for matching FormID and return a copy
        for (const auto& [uuid, volumes] : books_) {
            for (const auto& book : volumes) {
                if (book.bookFormId == formId) {
                    return book;  // Return copy
                }
            }
        }
        return std::nullopt;
    }

    std::vector<std::string> BookManager::GetAllTrackedActorUUIDs() const {
        std::vector<std::string> uuids;
        uuids.reserve(books_.size());
        for (const auto& [uuid, volumes] : books_) {
            if (!volumes.empty()) {
                uuids.push_back(uuid);
            }
        }
        return uuids;
    }

    void BookManager::RegisterBook(const std::string& actorUuid, const std::string& actorName,
                                   RE::FormID bookFormId, double startTime, double endTime, int volumeNumber,
                                   const std::string& journalTemplate,
                                   const std::string& bioTemplateName,
                                   double prevVolumeLastCreationTime,
                                   int prevVolumeCountAtBoundary,
                                   RE::FormID actorFormId) {
        DiaryBookData data;
        data.actorUuid = actorUuid;
        data.actorName = actorName;
        data.bookFormId = bookFormId;
        data.startTime = startTime;
        data.endTime = endTime;
        data.volumeNumber = volumeNumber;
        data.journalTemplate = journalTemplate;
        data.bioTemplateName = bioTemplateName;
        data.prevVolumeLastCreationTime = prevVolumeLastCreationTime;
        data.prevVolumeCountAtBoundary = prevVolumeCountAtBoundary;
        data.actorFormId = actorFormId;
        // Pre-warm the runtime cache so RefreshVolumeOnOpen never needs the UUID roundtrip.
        if (actorFormId != 0) {
            data.cachedActorFormId = actorFormId;
        }

        books_[actorUuid].push_back(data);
        SKSE::log::info("Registered book for actor {}: FormID 0x{:X}, Volume {} (template: {}, subfolder: {})",
                       actorUuid, bookFormId, volumeNumber, journalTemplate, bioTemplateName);

        // Persist row so it survives a save revert (bookText written by caller via UpdateBookText).
        DiaryDB::VolumeRow dbRow;
        dbRow.actorUuid                  = data.actorUuid;
        dbRow.actorName                  = data.actorName;
        dbRow.actorFormId                = static_cast<uint32_t>(actorFormId);
        dbRow.bookFormId                 = static_cast<uint32_t>(data.bookFormId);
        dbRow.volumeNumber               = data.volumeNumber;
        dbRow.startTime                  = data.startTime;
        dbRow.endTime                    = data.endTime;
        dbRow.journalTemplate            = data.journalTemplate;
        dbRow.bioTemplateName            = data.bioTemplateName;
        dbRow.lastKnownEntryCount        = data.lastKnownEntryCount;
        dbRow.prevVolumeLastCreationTime = data.prevVolumeLastCreationTime;
        dbRow.prevVolumeCountAtBoundary  = data.prevVolumeCountAtBoundary;
        // bookText left empty – caller sets it via UpdateBookText immediately after.
        DiaryDB::GetSingleton()->UpsertVolume(dbRow);
    }

    void BookManager::UpdateBookEndTime(const std::string& actorUuid, int volumeNumber, double endTime) {
        auto it = books_.find(actorUuid);
        if (it != books_.end()) {
            for (auto& book : it->second) {
                if (book.volumeNumber == volumeNumber) {
                    book.endTime = endTime;
                    SKSE::log::debug("Updated book endTime for {} volume {} (FormID 0x{:X}) to {}", 
                                   actorUuid, volumeNumber, book.bookFormId, endTime);
                    DiaryDB::GetSingleton()->UpdateEndTime(actorUuid, volumeNumber, endTime);
                    return;
                }
            }
        }
    }
    
    void BookManager::UpdateVolumeEntryCount(const std::string& actorUuid, int volumeNumber, int entryCount) {
        auto it = books_.find(actorUuid);
        if (it != books_.end()) {
            for (auto& book : it->second) {
                if (book.volumeNumber == volumeNumber) {
                    SKSE::log::debug("Updated {} volume {} entry count: {} -> {}", book.actorName, volumeNumber, book.lastKnownEntryCount, entryCount);
                    book.lastKnownEntryCount = entryCount;
                    return;
                }
            }
        }
    }

    void BookManager::UnregisterBook(const std::string& actorUuid) {
        auto it = books_.find(actorUuid);
        if (it != books_.end()) {
            SKSE::log::info("Unregistered {} volumes for {}", it->second.size(), actorUuid);
            books_.erase(it);
        }
        DiaryDB::GetSingleton()->DeleteActor(actorUuid);
    }

    int BookManager::RemoveBookByFormID(RE::FormID formId) {
        // Search all actors' books for this FormID
        for (auto& [uuid, volumes] : books_) {
            for (auto it = volumes.begin(); it != volumes.end(); ++it) {
                if (it->bookFormId == formId) {
                    int volumeNum = it->volumeNumber;
                    std::string actorName = it->actorName;
                    
                    // Check if this is the latest volume before removing
                    int maxVolume = 0;
                    for (const auto& vol : volumes) {
                        if (vol.volumeNumber > maxVolume) {
                            maxVolume = vol.volumeNumber;
                        }
                    }
                    bool isLatestVolume = (volumeNum == maxVolume);
                    
                    volumes.erase(it);
                    SKSE::log::debug("Removed volume {} for {} (FormID 0x{:X}) - player returned book",
                                  volumeNum, actorName, formId);
                    DiaryDB::GetSingleton()->DeleteVolume(uuid, volumeNum);

                    // If this was the only volume, remove the UUID entry entirely
                    if (volumes.empty()) {
                        books_.erase(uuid);
                        SKSE::log::debug("Removed last volume for {} - cleared all tracking", actorName);
                        return 2; // Was the latest (and only) volume
                    }
                    
                    if (isLatestVolume) {
                        SKSE::log::debug("Volume {} was the latest - NPC can get updates", volumeNum);
                        return 2; // Was the latest volume, allow updates
                    } else {
                        SKSE::log::debug("Volume {} is old - newer volumes exist, this volume stays frozen", volumeNum);
                        return 1; // Older volume, don't allow updates
                    }
                }
            }
        }
        return 0; // Not found
    }

    void BookManager::RegenerateAllDiaryTexts() {
        SKSE::log::info("[BookManager] Regenerating text for {} actors' diaries from API", books_.size());

        try {
            int totalRegenerated = 0;
            for (auto& [uuid, volumes] : books_) {
                uint32_t actorFormId = SkyrimNetDiaries::Database::GetFormIDForUUID(uuid);
                if (actorFormId == 0) continue;

                for (auto& bookData : volumes) {
                    // Keep cached FormID warm so the next open can skip the UUID lookup.
                    bookData.cachedActorFormId = actorFormId;

                    const int MAX_ENTRIES_PER_VOLUME = SkyrimNetDiaries::Config::GetSingleton()->GetEntriesPerVolume();

                    double queryStart = (bookData.volumeNumber == 1) ? 0.0 : bookData.startTime;
                    double queryEnd = bookData.endTime; // 0.0 = no upper bound for latest volume

                    std::string bookTitle = bookData.actorName + "'s Diary";
                    if (bookData.volumeNumber > 1) {
                        bookTitle += ", v" + std::to_string(bookData.volumeNumber);
                    }

                    SKSE::log::debug("[Regen] '{}' vol={} stored startTime={:.2f} endTime={:.2f} -> queryStart={:.2f} queryEnd={:.2f} limit={}",
                                    bookTitle, bookData.volumeNumber,
                                    bookData.startTime, bookData.endTime,
                                    queryStart, queryEnd, MAX_ENTRIES_PER_VOLUME + 1);

                    auto volumeEntries = SkyrimNetDiaries::Database::GetDiaryEntries(
                        actorFormId, MAX_ENTRIES_PER_VOLUME + 1,
                        queryStart, queryEnd, bookData.prevVolumeLastCreationTime,
                        bookData.prevVolumeCountAtBoundary);

                    SKSE::log::debug("[Regen] '{}' API returned {} entries; first={:.2f} last={:.2f}",
                                    bookTitle, volumeEntries.size(),
                                    volumeEntries.empty() ? 0.0 : volumeEntries.front().entry_date,
                                    volumeEntries.empty() ? 0.0 : volumeEntries.back().entry_date);

                    // Same deterministic cap as the slow path — sealed volumes must never
                    // show more than MAX_ENTRIES_PER_VOLUME entries.
                    if (bookData.endTime > 0.0 &&
                        static_cast<int>(volumeEntries.size()) > MAX_ENTRIES_PER_VOLUME) {
                        volumeEntries.resize(MAX_ENTRIES_PER_VOLUME);
                    }

                    std::string bookText = FormatDiaryEntries(volumeEntries, bookData.actorName,
                                                              bookData.startTime, bookData.endTime,
                                                              MAX_ENTRIES_PER_VOLUME);

                    // Persist to DB and warm in-memory cache.
                    int liveCount = static_cast<int>(volumeEntries.size());
                    DiaryDB::GetSingleton()->UpdateBookText(
                        uuid, bookData.volumeNumber, bookText, liveCount);
                    bookData.cachedBookText      = bookText;
                    bookData.lastKnownEntryCount = liveCount;

                    totalRegenerated++;
                }
            }

            SKSE::log::info("[BookManager] Regenerated {} diary volumes from API", totalRegenerated);

        } catch (const std::exception& e) {
            SKSE::log::error("[BookManager] Exception regenerating diary texts: {}", e.what());
        }
    }

    void BookManager::Save(SKSE::SerializationInterface* a_intfc) {
        // Volume data is now stored in DiaryDB (SQLite) — persists across reverts.
        // Write a sentinel so the co-save record stays well-formed.
        std::uint32_t sentinel = 0;
        a_intfc->WriteRecordData(&sentinel, sizeof(sentinel)); // totalVolumes = 0
        a_intfc->WriteRecordData(&sentinel, sizeof(sentinel)); // actorTemplateCount = 0
        SKSE::log::debug("[BookManager] Save: sentinel written (real data lives in DiaryDB)");
    }

    void BookManager::Load(SKSE::SerializationInterface* /*a_intfc*/, std::uint32_t /*version*/) {
        // Volume data is loaded from DiaryDB in LoadFromDB() (called from kPostLoadGame).
        // The sentinel written by Save() is intentionally ignored.
        SKSE::log::debug("[BookManager] Load: skipping co-save (real data loaded from DiaryDB)");
    }

    void BookManager::Revert() {
        // Clear in-memory maps only; DiaryDB on disk is intentionally preserved
        // so that volume metadata survives the revert and loads correctly.
        books_.clear();
        actorTemplates_.clear();
        SKSE::log::debug("[BookManager] Revert: in-memory data cleared (DiaryDB preserved on disk)");
    }

    void BookManager::FlushToDB() {
        auto* db = DiaryDB::GetSingleton();
        if (!db->IsOpen()) return;

        int volumesFlushed = 0;
        for (const auto& [uuid, volumes] : books_) {
            for (const auto& data : volumes) {
                DiaryDB::VolumeRow row;
                row.actorUuid                  = data.actorUuid;
                row.actorName                  = data.actorName;
                row.bookFormId                 = static_cast<uint32_t>(data.bookFormId);
                row.volumeNumber               = data.volumeNumber;
                row.startTime                  = data.startTime;
                row.endTime                    = data.endTime;
                row.journalTemplate            = data.journalTemplate;
                row.bioTemplateName            = data.bioTemplateName;
                row.lastKnownEntryCount        = data.lastKnownEntryCount;
                row.prevVolumeLastCreationTime = data.prevVolumeLastCreationTime;
                row.prevVolumeCountAtBoundary  = data.prevVolumeCountAtBoundary;
                row.bookText                   = data.cachedBookText;
                row.persistedInSave            = data.persistedInSave;  // preserve — MarkAllVolumesPersisted sets on save
                db->UpsertVolume(row);
                ++volumesFlushed;
            }
        }
        for (const auto& [uuid, tmpl] : actorTemplates_) {
            db->UpsertActorTemplate(uuid, tmpl);
        }
        SKSE::log::debug("[BookManager] FlushToDB: wrote {} volumes, {} actor templates",
                        volumesFlushed, actorTemplates_.size());
    }

    void BookManager::ClearActorCache() {
        std::lock_guard<std::mutex> lock(g_actorCacheMutex);
        g_actorCache.clear();
        SKSE::log::debug("[BookManager] Actor cache cleared");
    }

    // ---------------------------------------------------------------------------
    // FindActorForBook: resolve the owning NPC for a diary volume.
    // Uses bio_template_name matching (for ESL/mod-added NPCs) or direct FormID
    // lookup (for vanilla NPCs), with g_actorCache to avoid repeated searches.
    // ---------------------------------------------------------------------------
    static RE::Actor* FindActorForBook(RE::FormID targetFormID,
                                       const std::string& actorName,
                                       const std::string& bioTemplate) {
        if (bioTemplate == "player_special" || targetFormID == 0x14)
            return RE::PlayerCharacter::GetSingleton();

        {
            std::lock_guard<std::mutex> lock(g_actorCacheMutex);
            auto it = g_actorCache.find(targetFormID);
            if (it != g_actorCache.end()) return it->second;
        }

        RE::Actor* result = nullptr;

        if (!bioTemplate.empty() && bioTemplate.find('_') != std::string::npos) {
            size_t lastUnderscore = bioTemplate.find_last_of('_');
            std::string expectedLast3;
            if (lastUnderscore != std::string::npos && lastUnderscore + 1 < bioTemplate.size()) {
                expectedLast3 = bioTemplate.substr(lastUnderscore + 1);
                std::transform(expectedLast3.begin(), expectedLast3.end(), expectedLast3.begin(), ::tolower);
            }
            if (!expectedLast3.empty()) {
                auto* pl = RE::ProcessLists::GetSingleton();
                if (pl) {
                    auto searchList = [&](RE::BSTArray<RE::ActorHandle>& handles) {
                        for (auto& handle : handles) {
                            RE::NiPointer<RE::Actor> ptr;
                            if (!RE::Actor::LookupByHandle(handle.native_handle(), ptr)) continue;
                            auto* ref = ptr.get();
                            if (!ref) continue;
                            auto* base = ref->GetActorBase();
                            if (!base) continue;
                            std::string engineName = base->GetFullName();
                            // Exact match OR engine name is a suffix of the SkyrimNet display name.
                            // Handles rank-prefixed names: "Jarl Elisif the Fair" (SkyrimNet) vs
                            // "Elisif the Fair" (engine GetFullName).
                            bool nameMatch = (engineName == actorName) ||
                                (!engineName.empty() &&
                                 actorName.size() > engineName.size() &&
                                 actorName.compare(actorName.size() - engineName.size(),
                                                   engineName.size(), engineName) == 0);
                            if (!nameMatch) continue;
                            char last3[4];
                            snprintf(last3, sizeof(last3), "%03x", ref->GetFormID() & 0xFFF);
                            if (expectedLast3 == last3) { result = ref; return true; }
                        }
                        return false;
                    };
                    if (!searchList(pl->highActorHandles))
                        if (!searchList(pl->middleHighActorHandles))
                            if (!searchList(pl->middleLowActorHandles))
                                searchList(pl->lowActorHandles);
                }
            }
        } else {
            result = RE::TESForm::LookupByID<RE::Actor>(targetFormID);
        }

        {
            std::lock_guard<std::mutex> lock(g_actorCacheMutex);
            g_actorCache[targetFormID] = result;
        }
        return result;
    }

    // ---------------------------------------------------------------------------
    // EnsureBookInInventory: if the NPC doesn't have the book, add it.
    // Called after LoadFromDB for volumes whose DPF form exists but may not be
    // in the NPC's inventory (e.g. after reload-without-save).
    // ---------------------------------------------------------------------------
    static void EnsureBookInInventory(RE::FormID bookFormId, RE::FormID targetFormID,
                                      const std::string& actorName, const std::string& bioTemplate,
                                      const std::string& bookName) {
        auto* book = RE::TESForm::LookupByID<RE::TESObjectBOOK>(bookFormId);
        if (!book) {
            SKSE::log::warn("[EnsureInventory] Book 0x{:X} no longer valid — skipping '{}'", bookFormId, bookName);
            return;
        }

        RE::Actor* actor = FindActorForBook(targetFormID, actorName, bioTemplate);
        if (!actor) {
            SKSE::log::warn("[EnsureInventory] Could not find actor '{}' for '{}'", actorName, bookName);
            return;
        }

        // Check inventory — avoid adding a duplicate.
        auto inv = actor->GetInventory();
        for (const auto& [item, invData] : inv) {
            if (item && item->GetFormID() == bookFormId && invData.first > 0) {
                SKSE::log::debug("[EnsureInventory] '{}' already in {}'s inventory", bookName, actorName);
                return;
            }
        }

        actor->AddObjectToContainer(book, nullptr, 1, nullptr);
        SKSE::log::info("[EnsureInventory] Re-added '{}' to {}'s inventory after reload", bookName, actorName);
    }

    void BookManager::QueueInventoryCheck() {
        int queued = 0;
        int skipped = 0;
        for (const auto& [uuid, volumes] : books_) {
            uint32_t actorFormId = SkyrimNetDiaries::Database::GetFormIDForUUID(uuid);
            for (const auto& vol : volumes) {
                if (vol.persistedInSave) {
                    // This volume was committed to a .ess save — the loaded inventory
                    // state is authoritative.  Do not re-add (would duplicate taken books).
                    ++skipped;
                    continue;
                }
                RE::FormID bookFid   = vol.bookFormId;
                RE::FormID actorFid  = static_cast<RE::FormID>(actorFormId);
                std::string aName    = vol.actorName;
                std::string bio      = vol.bioTemplateName;
                std::string bName    = vol.actorName + "'s Diary";
                if (vol.volumeNumber > 1) bName += ", v" + std::to_string(vol.volumeNumber);
                SKSE::GetTaskInterface()->AddTask([bookFid, actorFid, aName, bio, bName]() {
                    EnsureBookInInventory(bookFid, actorFid, aName, bio, bName);
                });
                ++queued;
            }
        }
        if (queued > 0 || skipped > 0)
            SKSE::log::debug("[BookManager] Inventory check: {} volume(s) queued, {} skipped (already persisted)", queued, skipped);
    }

    std::vector<std::string> BookManager::LoadFromDB() {
        auto* db = DiaryDB::GetSingleton();

        // Always clear in-memory state on each game load, regardless of whether the
        // DB is open.  If we skip the clear when the DB is closed (e.g. save-folder
        // detection failed on a previous load), stale books_ entries from the prior
        // session survive and cause the catch-up scan to think every actor already
        // has books — so nothing gets recreated after a reload-without-save.
        books_.clear();
        actorTemplates_.clear();

        if (!db->IsOpen()) {
            SKSE::log::warn("[BookManager] LoadFromDB: DiaryDB not open — skipping");
            return {};
        }

        auto rows = db->LoadAllVolumes();
        std::vector<std::string> invalidActors;

        for (auto& row : rows) {
            // Volumes that were never committed to a .ess save are ephemeral.
            // The player may have quit-without-saving after diary creation, then
            // loaded an older save whose game-time is EARLIER than the volume's
            // recorded endTime.  Any new diary entry generated at that reverted
            // game-time would have entry_date <= endTime and be silently filtered
            // out as "belongs to previous volume" — so the book is never rebuilt.
            //
            // Safety: only delete if the DPF form is also gone.  If the form still
            // exists the row may be a legitimate migrated row (persisted_in_save
            // added via ALTER TABLE DEFAULT 0 on an older install) that just hasn't
            // been re-saved yet.  In that case let QueueInventoryCheck / the catch-up
            // scan handle it gracefully rather than nuking it here.
            if (!row.persistedInSave) {
                auto* form = RE::TESForm::LookupByID(static_cast<RE::FormID>(row.bookFormId));
                bool formValid = form && form->GetFormType() == RE::FormType::Book;
                if (!formValid) {
                    SKSE::log::info("[LoadFromDB] {} vol {} was never saved and DPF form is gone — removing stale row and queuing recreation",
                                   row.actorName, row.volumeNumber);
                    db->DeleteVolume(row.actorUuid, row.volumeNumber);
                    invalidActors.push_back(row.actorUuid);
                    continue;
                }
                // Form still alive but not persisted — fall through and load normally.
                // QueueSealedVolumeRecovery / catch-up will detect the stale endTime
                // and rebuild if entries exist beyond it.
                SKSE::log::info("[LoadFromDB] {} vol {} not persisted but DPF form 0x{:X} still valid — loading and deferring to catch-up",
                               row.actorName, row.volumeNumber, row.bookFormId);
            }

            // Validate the DPF book form still exists (it is lost on save revert).
            auto* form = RE::TESForm::LookupByID(static_cast<RE::FormID>(row.bookFormId));
            if (!form || form->GetFormType() != RE::FormType::Book) {
                SKSE::log::warn("[LoadFromDB] FormID 0x{:X} for {} vol {} is invalid — removing and queuing recovery",
                               row.bookFormId, row.actorName, row.volumeNumber);
                db->DeleteVolume(row.actorUuid, row.volumeNumber);
                invalidActors.push_back(row.actorUuid);
                continue;
            }

            DiaryBookData data;
            data.actorUuid                   = row.actorUuid;
            data.actorName                   = row.actorName;
            data.bookFormId                  = static_cast<RE::FormID>(row.bookFormId);
            data.volumeNumber                = row.volumeNumber;
            data.startTime                   = row.startTime;
            data.endTime                     = row.endTime;
            data.journalTemplate             = row.journalTemplate;
            data.bioTemplateName             = row.bioTemplateName;
            data.lastKnownEntryCount         = row.lastKnownEntryCount;
            data.prevVolumeLastCreationTime  = row.prevVolumeLastCreationTime;
            data.prevVolumeCountAtBoundary   = row.prevVolumeCountAtBoundary;
            data.cachedBookText              = row.bookText;  // pre-warmed from DB
            data.persistedInSave             = row.persistedInSave;
            data.actorFormId                 = static_cast<RE::FormID>(row.actorFormId);
            // Pre-warm cachedActorFormId so RefreshVolumeOnOpen never needs UUID roundtrip.
            if (row.actorFormId != 0) {
                data.cachedActorFormId = static_cast<RE::FormID>(row.actorFormId);
            }

            books_[data.actorUuid].push_back(std::move(data));
        }

        // Ensure each actor's volumes are in order.
        for (auto& [uuid, volumes] : books_) {
            std::sort(volumes.begin(), volumes.end(),
                [](const DiaryBookData& a, const DiaryBookData& b) {
                    return a.volumeNumber < b.volumeNumber;
                });
        }

        actorTemplates_ = db->LoadActorTemplates();

        SKSE::log::info("[LoadFromDB] Loaded {} actors ({} invalid FormIDs queued for recovery)",
                        books_.size(), invalidActors.size());
        return invalidActors;
    }

    void BookManager::RefreshVolumeOnOpen(DiaryBookData* vol) {
        if (!vol) return;

        // Backfill bioTemplateName (may be missing on old co-save sessions).
        if (vol->bioTemplateName.empty()) {
            vol->bioTemplateName = Database::GetTemplateNameByUUID(vol->actorUuid);
        }

        // Lazy FormID lookup — prefer the authoritative stored actorFormId; fall back
        // to UUID roundtrip only for volumes that pre-date this field (actorFormId == 0).
        if (vol->cachedActorFormId == 0) {
            if (vol->actorFormId != 0) {
                vol->cachedActorFormId = vol->actorFormId;
            } else {
                vol->cachedActorFormId = Database::GetFormIDForUUID(vol->actorUuid);
            }
        }
        if (vol->cachedActorFormId == 0) return;

        const int MAX_ENTRIES = Config::GetSingleton()->GetEntriesPerVolume();
        double queryStart = (vol->volumeNumber == 1) ? 0.0 : vol->startTime;

        // For the active (latest) volume use 0.0 so entries written after the
        // last update are visible even if UpdateDiaryForActorInternal hasn't run yet.
        // For sealed older volumes, respect vol->endTime as the upper-time cutoff.
        double queryEnd = 0.0;
        {
            auto* allVols = GetAllVolumesForActor(vol->actorUuid);
            if (allVols && !allVols->empty() &&
                allVols->back().volumeNumber != vol->volumeNumber) {
                // A newer volume exists → this one is sealed.
                queryEnd = vol->endTime;
            }
        }

        auto liveEntries = Database::GetDiaryEntries(
            vol->cachedActorFormId, MAX_ENTRIES + 1, queryStart, queryEnd,
            vol->prevVolumeLastCreationTime, vol->prevVolumeCountAtBoundary);
        int liveCount = static_cast<int>(liveEntries.size());

        // For sealed volumes, cap to MAX_ENTRIES so boundary tie-breaking is deterministic.
        if (vol->endTime > 0.0 && liveCount > MAX_ENTRIES) {
            liveEntries.resize(MAX_ENTRIES);
            liveCount = MAX_ENTRIES;
        }

        // Fast path: nothing changed and cache is warm with current-format text — nothing to do.
        // If the cached text is in the old format (no <font> tags, generated before font-tag
        // support was added), fall through to force a one-time regeneration even when the
        // entry count hasn't changed.  This upgrades stale DB rows automatically on first open.
        // EXCEPTION: if liveCount == 0 we have nothing to regenerate FROM — in that case
        // the cached text (e.g. externally loaded DB text) must be preserved as-is regardless
        // of format.  Regenerating with an empty entry list would replace real content with
        // "All entries removed", which is wrong for test/imported books.
        bool textIsCurrentFormat = vol->cachedBookText.find("<font face='") != std::string::npos;
        if (liveCount == vol->lastKnownEntryCount && !vol->cachedBookText.empty() && textIsCurrentFormat) {
            return;
        }
        // EXCEPTION (see comment above): nothing to regenerate FROM when liveCount==0 —
        // preserve whatever cached text exists, regardless of format.  This handles both
        // old-format rows and current-format text for test/imported entries that were never
        // written to the SkyrimNet API DB.
        if (liveCount == 0 && !vol->cachedBookText.empty()) {
            SKSE::log::info("[SNPD] {} vol {} has no live API entries — preserving cached text (test/imported data)",
                vol->actorName, vol->volumeNumber);
            return;
        }

        if (!textIsCurrentFormat && !vol->cachedBookText.empty()) {
            SKSE::log::info("[SNPD] {} vol {} has old-format text (no font tags) — regenerating from {} live entries",
                vol->actorName, vol->volumeNumber, liveCount);
        }

        // Entries were deleted — update sealed endTime so QueueSealedVolumeRecovery
        // doesn't probe beyond the now-missing entry's timestamp and spawn a duplicate volume.
        if (liveCount < vol->lastKnownEntryCount) {
            SKSE::log::info("[SNPD] {} vol {} shrank {} → {} entries on open",
                vol->actorName, vol->volumeNumber, vol->lastKnownEntryCount, liveCount);
            if (vol->endTime > 0.0 && !liveEntries.empty()) {
                double newEnd = liveEntries.back().entry_date;
                if (newEnd != vol->endTime) {
                    SKSE::log::debug("[SNPD]   sealed endTime updated {:.2f} → {:.2f}", vol->endTime, newEnd);
                    DiaryDB::GetSingleton()->UpdateEndTime(vol->actorUuid, vol->volumeNumber, newEnd);
                    vol->endTime = newEnd;
                }
            }
        }

        // Reformat and persist to DB + in-memory cache.
        std::string bookText = FormatDiaryEntries(
            liveEntries, vol->actorName, vol->startTime, vol->endTime, MAX_ENTRIES);
        DiaryDB::GetSingleton()->UpdateBookText(vol->actorUuid, vol->volumeNumber, bookText, liveCount);
        UpdateVolumeEntryCount(vol->actorUuid, vol->volumeNumber, liveCount);
        vol->lastKnownEntryCount = liveCount;
        vol->cachedBookText      = std::move(bookText);
    }

} // namespace SkyrimNetDiaries
