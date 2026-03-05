#include "BookManager.h"
#include "Database.h"
#include "DynamicBookFrameworkAPI.h"
#include "DPF_API.h"
#include <fstream>
#include <cstring>
#include <algorithm>
#include <cctype>
#include <mutex>
#include <unordered_map>

// External declarations from main.cpp
extern std::string FormatDiaryEntries(const std::vector<SkyrimNetDiaries::DiaryEntry>&, const std::string&, double, double, int maxEntries);
extern bool WriteDynamicBookFile(const std::string& bookTitle, const std::string& text, const std::string& actorSubfolder);

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
            SKSE::log::info("DPF.Create() callback invoked for {}", actorName);
            
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
                    // DEBUG: Check if template already has clean teaches.spell
                    SKSE::log::info("Template book teaches.spell = 0x{:X}, flags = 0x{:X}",
                                   reinterpret_cast<uintptr_t>(templateBook->data.teaches.spell),
                                   templateBook->data.flags.underlying());
                    // Copy basic properties
                    newBook->data.type = templateBook->data.type;
                    newBook->inventoryModel = templateBook->inventoryModel;
                    newBook->itemCardDescription = templateBook->itemCardDescription;
                    newBook->weight = 0.5f;
                    newBook->value = 0;
                    
                    // CRITICAL: Clear flags by setting to 0, then selectively set safe ones
                    // Do NOT copy flags from template
                    newBook->data.flags = static_cast<RE::OBJ_BOOK::Flag>(0);
                    newBook->data.flags.set(RE::OBJ_BOOK::Flag::kCantTake);
                    
                    // Template already has clean teaches data - don't touch it
                    // Clearing/nullifying causes DPF serializer to crash on save
                    
                    SKSE::log::info("Book initialization complete with flags: 0x{:X}", newBook->data.flags.underlying());
                } else {
                    SKSE::log::error("Failed to find template book - using minimal initialization");
                    // Without template, zero out teaches structure but let DPF handle the rest
                    std::memset(&newBook->data.teaches, 0, sizeof(newBook->data.teaches));
                    newBook->data.flags.set(RE::OBJ_BOOK::Flag::kCantTake);
                    
                    SKSE::log::info("Cleared teaches data without template");
                }
                
                // Set book name with volume number
                std::string bookName = name + "'s Diary";
                if (volume > 1) {
                    bookName += ", v" + std::to_string(volume);
                }
                newBook->SetFullName(bookName.c_str());
                
                SKSE::log::info("Configured book: '{}'", bookName);
                
                // Format diary entries (use cached if available, otherwise fetch from database)
                std::vector<SkyrimNetDiaries::DiaryEntry> allEntries;
                
                if (!cachedEntries.empty()) {
                    // Use pre-cached entries (avoids database reopening)
                    SKSE::log::info("Using {} pre-cached diary entries for {} (no DB access needed)", cachedEntries.size(), name);
                    allEntries = cachedEntries;
                } else {
                    // Fallback: fetch from API if entries weren't cached (rare)
                    try {
                        uint32_t actorFormId = SkyrimNetDiaries::Database::GetFormIDForUUID(uuid);
                        if (actorFormId != 0) {
                            allEntries = SkyrimNetDiaries::Database::GetDiaryEntries(actorFormId, 5000, start, 0.0, prevVolLastCT, prevVolCountAtBoundary);
                        }
                        SKSE::log::info("Retrieved {} diary entries from API for {} (UUID: {}, startTime: {})", 
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
                            
                            // Write to persistent file and load into DBF memory
                            WriteDynamicBookFile(bookName, bookText, bioTemplate);
                            DynamicBookFramework_API::SetDynamicText(bookName.c_str(), bookText.c_str());
                            
                            SKSE::log::info("Set dynamic text for '{}': {} diary entries (of {} total)", 
                                          bookName, entriesToTake, allEntries.size());
                            
                            // Calculate the actual endTime based on the last entry included in this volume
                            double volumeEndTime = end;  // Default to the passed endTime
                            if (!entriesToFormat.empty()) {
                                // Use the timestamp of the last entry included as the endTime for this volume
                                volumeEndTime = entriesToFormat.back().entry_date;
                                SKSE::log::info("Volume {} endTime set to last entry timestamp: {}", volume, volumeEndTime);
                            }
                            
                            // Register book with the calculated endTime using FormID
                            bookManager->RegisterBook(uuid, name, newBook->GetFormID(), start, volumeEndTime, volume, journalTemplate, bioTemplate, prevVolLastCT, prevVolCountAtBoundary);
                            
                            // If there are more than max entries, log that additional volumes should be created
                            if (allEntries.size() > MAX_ENTRIES_PER_BOOK) {
                                SKSE::log::info("Note: Actor has {} entries total. Volume {} contains first {}. Additional entries will be in next volume when diary is stolen/returned.",
                                              allEntries.size(), volume, MAX_ENTRIES_PER_BOOK);
                            }
                            
                            // Add to NPC inventory immediately
                            // NOTE: DPF callbacks are async and may not be on the main thread
                            // We need to defer the inventory add to the main game thread
                            SKSE::log::info("Attempting to add book to actor 0x{:X} ({})...", targetFormID, name);
                            
                            SKSE::GetTaskInterface()->AddTask([targetFormID, newBook, bookName, name = std::string(name), bioTemplate]() {
                                RE::Actor* targetActor = nullptr;
                                
                                // Handle player diaries (UUID = "player_special")
                                if (bioTemplate == "player_special" || targetFormID == 0x14) {
                                    targetActor = RE::PlayerCharacter::GetSingleton();
                                    SKSE::log::info("Player diary detected - assigning to player (FormID 0x14)");
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
                                            SKSE::log::info("✓ Found actor 0x{:X} ({}) in cache - skipping search", targetFormID, name);
                                        }
                                    }
                                }
                                
                                // Only do expensive search if not in cache
                                if (!foundInCache) {
                                    SKSE::log::info("Looking up actor for book '{}', targetFormID=0x{:X}, bioTemplate='{}'", bookName, targetFormID, bioTemplate);
                                
                                // For actors with bio_template_name, use name + last 3 digits matching
                                // bio_template_name format: "actorname_XYZ" where XYZ are last 3 hex digits of REFERENCE FormID
                                if (!bioTemplate.empty() && bioTemplate.find('_') != std::string::npos) {
                                    SKSE::log::info("Using bio_template_name matching for: '{}'", bioTemplate);
                                    
                                    // Extract last 3 digits from bio_template_name (e.g., "fetri_el_874" -> "874")
                                    size_t lastUnderscore = bioTemplate.find_last_of('_');
                                    std::string expectedLast3;
                                    if (lastUnderscore != std::string::npos && lastUnderscore + 1 < bioTemplate.length()) {
                                        expectedLast3 = bioTemplate.substr(lastUnderscore + 1);
                                        // Convert to lowercase for case-insensitive matching
                                        std::transform(expectedLast3.begin(), expectedLast3.end(), expectedLast3.begin(), ::tolower);
                                        SKSE::log::info("Searching for actor '{}' with REFERENCE FormID ending in '{}'", name, expectedLast3);
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
                                                    auto actorRef = handle.get();
                                                    if (actorRef) {
                                                        auto actorBase = actorRef->GetActorBase();
                                                        if (actorBase) {
                                                            std::string actorName = actorBase->GetFullName();
                                                            uint32_t refFormID = actorRef->GetFormID();
                                                            
                                                            actorsChecked++;
                                                            
                                                            // Only check if name matches first (optimization)
                                                            if (actorName == name) {
                                                                nameMatches++;
                                                                // Get last 3 hex digits of REFERENCE FormID
                                                                char refLast3[4];
                                                                snprintf(refLast3, sizeof(refLast3), "%03x", refFormID & 0xFFF);
                                                                
                                                                SKSE::log::info("  Found '{}' at 0x{:X} (last3: {})", actorName, refFormID, refLast3);
                                                                
                                                                // Match last 3 digits of reference FormID
                                                                if (expectedLast3 == refLast3) {
                                                                    targetActor = actorRef.get();
                                                                    SKSE::log::info("✓ Matched! Found actor via bio_template_name: '{}' (0x{:X})", actorName, refFormID);
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
                                    SKSE::log::info("No bio_template_name, using FormID lookup for 0x{:X}", targetFormID);
                                    targetActor = RE::TESForm::LookupByID<RE::Actor>(targetFormID);
                                }
                                
                                    // Cache the result (even if nullptr) to avoid repeating search
                                    {
                                        std::lock_guard<std::mutex> lock(g_actorCacheMutex);
                                        g_actorCache[targetFormID] = targetActor;
                                        if (targetActor) {
                                            SKSE::log::info("✓ Cached actor 0x{:X} ({}) for subsequent volumes", targetFormID, name);
                                        } else {
                                            SKSE::log::warn("Cached nullptr for actor 0x{:X} to avoid repeated searches", targetFormID);
                                        }
                                    }
                                } // End of !foundInCache block
                                } // End of player check
                                
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
                                                SKSE::log::info("Removed kCantTake flag from '{}' - book is now safe to take", bookName);
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
            SKSE::log::info("Using cached journal template for {}: {}", actorName, it->second);
            return it->second;
        }
        
        std::string selectedTemplate;
        
        // Check for Nightingale NPCs (special journal)
        if (!nightingaleTemplate_.empty()) {
            if (actorName == "Karliah" || actorName == "Gallus" || actorName == "Mercer Frey") {
                selectedTemplate = nightingaleTemplate_;
                SKSE::log::info("Selected Nightingale journal for {}", actorName);
                actorTemplates_[actorUuid] = selectedTemplate;
                return selectedTemplate;
            }
        }
        
        // If no variants configured, use base template
        if (journalTemplates_.empty()) {
            selectedTemplate = templateBookEditorId_;
            SKSE::log::info("No journal variants - using base template for {}", actorName);
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
        
        SKSE::log::info("Selected journal template for {}: {}", actorName, selectedTemplate);
        actorTemplates_[actorUuid] = selectedTemplate;
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

        SKSE::log::info("Looking for template book with Editor ID: {}", templateToUse);
        
        // Find book by Editor ID - need to iterate since there's no direct lookup
        RE::TESObjectBOOK* templateBook = nullptr;
        auto& books = dataHandler->GetFormArray<RE::TESObjectBOOK>();
        for (auto book : books) {
            if (book && book->GetFormEditorID()) {
                std::string editorId = book->GetFormEditorID();
                if (editorId == templateToUse) {
                    templateBook = book;
                    SKSE::log::info("Found template book: {} (FormID: 0x{:X})", 
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

        SKSE::log::info("Dispatching async DPF.Create() for {} (volume {}) with {} cached entries...", actorName, volumeNumber, entries.size());
        
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
        
        SKSE::log::info("DPF.Create() dispatched - book will be created asynchronously");
        
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
        SKSE::log::info("Would set text: {}", bookText);
        
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
        
        SKSE::log::info("Set book text ({} characters) for book 0x{:X}", text.length(), book->GetFormID());
        SKSE::log::warn("Note: Book text injection via memory is limited - text may not display");
    }

    // Register diary with Dynamic Book Framework INI
    bool BookManager::RegisterDiaryInDBF(const std::string& bookTitle)
    {
        try {
            auto iniPath = std::filesystem::current_path() / "Data" / "SKSE" / "Plugins" / "DynamicBookFramework" / "Configs" / "SkyrimNetPhysicalDiaries.ini";
            
            // Ensure parent directories exist
            std::filesystem::create_directories(iniPath.parent_path());
            
            // Check if this book is already registered
            bool needsSection = false;
            if (std::filesystem::exists(iniPath)) {
                std::ifstream checkFile(iniPath);
                if (checkFile.is_open()) {
                    std::string line;
                    bool foundSection = false;
                    while (std::getline(checkFile, line)) {
                        if (line == "[Books]") {
                            foundSection = true;
                        }
                        if (line.find(bookTitle) != std::string::npos) {
                            checkFile.close();
                            SKSE::log::info("Book '{}' already registered in DBF INI", bookTitle);
                            return true;
                        }
                    }
                    needsSection = !foundSection;
                    checkFile.close();
                }
            } else {
                needsSection = true;
            }
            
            // Append to INI file
            std::ofstream file(iniPath, std::ios::app);
            if (!file.is_open()) {
                SKSE::log::error("Failed to open DBF INI for writing: {}", iniPath.string());
                return false;
            }
            
            // Add [Books] section if needed
            if (needsSection) {
                file << "[Books]\n";
            }
            
            // Format: BookTitle = Filename.txt
            file << bookTitle << " = " << bookTitle << ".txt\n";
            file.close();
            
            SKSE::log::info("Registered '{}' in DBF INI", bookTitle);
            return true;
            
        } catch (const std::exception& e) {
            SKSE::log::error("Exception registering diary in DBF INI: {}", e.what());
            return false;
        }
    }

    // Write diary text to file for Dynamic Book Framework
    bool BookManager::WriteDynamicBookFile(const std::string& bookTitle, const std::string& text)
    {
        auto booksPath = std::filesystem::current_path() / "Data" / "SKSE" / "Plugins" / "DynamicBookFramework" / "Books";
        
        try {
            // Use fixed folder name for SkyrimNet diaries
            std::string saveFolder = "SkyrimNet";
            
            auto saveBooksPath = booksPath / saveFolder;
            std::filesystem::create_directories(saveBooksPath);
            
            auto filePath = saveBooksPath / (bookTitle + ".txt");
            std::ofstream file(filePath);
            
            if (!file.is_open()) {
                SKSE::log::error("Failed to open file for writing: {}", filePath.string());
                return false;
            }
            
            file << text;
            file.close();
            
            SKSE::log::info("Wrote book file: {}", filePath.string());
            
            // Register in INI
            RegisterDiaryInDBF(bookTitle);
            
            // Reload DBF mappings
            DynamicBookFramework_API::ReloadBookMappings();
            
            return true;
            
        } catch (const std::exception& e) {
            SKSE::log::error("Exception writing diary file: {}", e.what());
            return false;
        }
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
                                   int prevVolumeCountAtBoundary) {
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

        books_[actorUuid].push_back(data);
        SKSE::log::info("Registered book for actor {}: FormID 0x{:X}, Volume {} (template: {}, subfolder: {})",
                       actorUuid, bookFormId, volumeNumber, journalTemplate, bioTemplateName);
    }

    void BookManager::UpdateBookEndTime(const std::string& actorUuid, int volumeNumber, double endTime) {
        auto it = books_.find(actorUuid);
        if (it != books_.end()) {
            for (auto& book : it->second) {
                if (book.volumeNumber == volumeNumber) {
                    book.endTime = endTime;
                    SKSE::log::info("Updated book endTime for {} volume {} (FormID 0x{:X}) to {}", 
                                   actorUuid, volumeNumber, book.bookFormId, endTime);
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
                    SKSE::log::info("Removed volume {} for {} (FormID 0x{:X}) - player returned book",
                                  volumeNum, actorName, formId);
                    
                    // If this was the only volume, remove the UUID entry entirely
                    if (volumes.empty()) {
                        books_.erase(uuid);
                        SKSE::log::info("Removed last volume for {} - cleared all tracking", actorName);
                        return 2; // Was the latest (and only) volume
                    }
                    
                    if (isLatestVolume) {
                        SKSE::log::info("Volume {} was the latest - NPC can get updates", volumeNum);
                        return 2; // Was the latest volume, allow updates
                    } else {
                        SKSE::log::info("Volume {} is old - newer volumes exist, this volume stays frozen", volumeNum);
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

                    SKSE::log::info("[Regen] '{}' vol={} stored startTime={:.2f} endTime={:.2f} -> queryStart={:.2f} queryEnd={:.2f} limit={}",
                                    bookTitle, bookData.volumeNumber,
                                    bookData.startTime, bookData.endTime,
                                    queryStart, queryEnd, MAX_ENTRIES_PER_VOLUME + 1);

                    auto volumeEntries = SkyrimNetDiaries::Database::GetDiaryEntries(
                        actorFormId, MAX_ENTRIES_PER_VOLUME + 1,
                        queryStart, queryEnd, bookData.prevVolumeLastCreationTime,
                        bookData.prevVolumeCountAtBoundary);

                    SKSE::log::info("[Regen] '{}' API returned {} entries; first={:.2f} last={:.2f}",
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

                    ::WriteDynamicBookFile(bookTitle, bookText, bookData.bioTemplateName);
                    DynamicBookFramework_API::SetDynamicText(bookTitle.c_str(), bookText.c_str());

                    // Update in-memory cache so future opens this session use the fast path
                    // with the freshly regenerated (e.g. re-fonted) text.
                    bookData.cachedBookText = bookText;

                    totalRegenerated++;
                }
            }

            SKSE::log::info("[BookManager] Regenerated {} diary volumes from API", totalRegenerated);

        } catch (const std::exception& e) {
            SKSE::log::error("[BookManager] Exception regenerating diary texts: {}", e.what());
        }
    }

    void BookManager::Save(SKSE::SerializationInterface* a_intfc) {
        // DPF handles book form persistence, but we need to save our tracking data (UUID→volume mappings)
        // First pass: count total volumes across all UUIDs
        std::uint32_t totalVolumes = 0;
        for (const auto& [uuid, volumes] : books_) {
            totalVolumes += static_cast<std::uint32_t>(volumes.size());
        }
        
        SKSE::log::info("Saving tracking data for {} volumes across {} actors", totalVolumes, books_.size());
        if (!a_intfc->WriteRecordData(&totalVolumes, sizeof(totalVolumes))) {
            SKSE::log::error("Failed to write total volume count");
            return;
        }

        // Second pass: write each volume
        for (const auto& [uuid, volumes] : books_) {
            for (const auto& data : volumes) {
                // Write UUID length and string
                std::uint32_t uuidLen = static_cast<std::uint32_t>(uuid.length());
                if (!a_intfc->WriteRecordData(&uuidLen, sizeof(uuidLen))) {
                    SKSE::log::error("Failed to write UUID length");
                    continue;
                }
                if (!a_intfc->WriteRecordData(uuid.c_str(), uuidLen)) {
                    SKSE::log::error("Failed to write UUID");
                    continue;
                }
                
                // Write actor name
                std::uint32_t nameLen = static_cast<std::uint32_t>(data.actorName.length());
                if (!a_intfc->WriteRecordData(&nameLen, sizeof(nameLen))) {
                    SKSE::log::error("Failed to write actor name length");
                    continue;
                }
                if (!a_intfc->WriteRecordData(data.actorName.c_str(), nameLen)) {
                    SKSE::log::error("Failed to write actor name");
                    continue;
                }
                
                // Write FormID
                if (!a_intfc->WriteRecordData(&data.bookFormId, sizeof(data.bookFormId))) {
                    SKSE::log::error("Failed to write book FormID");
                    continue;
                }

                // Write timestamps
                if (!a_intfc->WriteRecordData(&data.startTime, sizeof(data.startTime))) {
                    SKSE::log::error("Failed to write startTime");
                    continue;
                }
                if (!a_intfc->WriteRecordData(&data.endTime, sizeof(data.endTime))) {
                    SKSE::log::error("Failed to write endTime");
                    continue;
                }
                
                // Write volume number
                if (!a_intfc->WriteRecordData(&data.volumeNumber, sizeof(data.volumeNumber))) {
                    SKSE::log::error("Failed to write volume number");
                    continue;
                }
                
                // Write journal template (new in this version)
                std::uint32_t templateLen = static_cast<std::uint32_t>(data.journalTemplate.length());
                if (!a_intfc->WriteRecordData(&templateLen, sizeof(templateLen))) {
                    SKSE::log::error("Failed to write journal template length");
                    continue;
                }
                if (templateLen > 0) {
                    if (!a_intfc->WriteRecordData(data.journalTemplate.c_str(), templateLen)) {
                        SKSE::log::error("Failed to write journal template");
                        continue;
                    }
                }
                
                // Write entry count for deletion tracking
                if (!a_intfc->WriteRecordData(&data.lastKnownEntryCount, sizeof(data.lastKnownEntryCount))) {
                    SKSE::log::error("Failed to write entry count");
                    continue;
                }

                // Write bio template name (actor-specific file subfolder)
                std::uint32_t bioNameLen = static_cast<std::uint32_t>(data.bioTemplateName.length());
                if (!a_intfc->WriteRecordData(&bioNameLen, sizeof(bioNameLen))) {
                    SKSE::log::error("Failed to write bio template name length");
                    continue;
                }
                if (bioNameLen > 0) {
                    if (!a_intfc->WriteRecordData(data.bioTemplateName.c_str(), bioNameLen)) {
                        SKSE::log::error("Failed to write bio template name");
                        continue;
                    }
                }

                // Write prevVolumeLastCreationTime (v2: used to exclude boundary entries from previous volume)
                if (!a_intfc->WriteRecordData(&data.prevVolumeLastCreationTime, sizeof(data.prevVolumeLastCreationTime))) {
                    SKSE::log::error("Failed to write prevVolumeLastCreationTime");
                    continue;
                }
                // Write prevVolumeCountAtBoundary (v3: exact count of prev-vol entries at boundary)
                if (!a_intfc->WriteRecordData(&data.prevVolumeCountAtBoundary, sizeof(data.prevVolumeCountAtBoundary))) {
                    SKSE::log::error("Failed to write prevVolumeCountAtBoundary");
                    continue;
                }
            }
        }
        
        // Save actor template mappings (UUID -> template choice)
        std::uint32_t actorTemplateCount = static_cast<std::uint32_t>(actorTemplates_.size());
        if (!a_intfc->WriteRecordData(&actorTemplateCount, sizeof(actorTemplateCount))) {
            SKSE::log::error("Failed to write actor template count");
            return;
        }
        
        for (const auto& [uuid, templateName] : actorTemplates_) {
            // Write UUID
            std::uint32_t uuidLen = static_cast<std::uint32_t>(uuid.length());
            if (!a_intfc->WriteRecordData(&uuidLen, sizeof(uuidLen))) {
                SKSE::log::error("Failed to write UUID length for actor template");
                continue;
            }
            if (!a_intfc->WriteRecordData(uuid.c_str(), uuidLen)) {
                SKSE::log::error("Failed to write UUID for actor template");
                continue;
            }
            
            // Write template name
            std::uint32_t templateLen = static_cast<std::uint32_t>(templateName.length());
            if (!a_intfc->WriteRecordData(&templateLen, sizeof(templateLen))) {
                SKSE::log::error("Failed to write template name length");
                continue;
            }
            if (!a_intfc->WriteRecordData(templateName.c_str(), templateLen)) {
                SKSE::log::error("Failed to write template name");
                continue;
            }
        }

        SKSE::log::info("Saved {} diary volumes and {} actor template mappings", totalVolumes, actorTemplates_.size());
    }

    void BookManager::Load(SKSE::SerializationInterface* a_intfc, std::uint32_t version) {
        // DPF persists the book forms, we just need to restore our tracking data
        std::uint32_t totalVolumes;
        if (!a_intfc->ReadRecordData(&totalVolumes, sizeof(totalVolumes))) {
            SKSE::log::error("Failed to read total volume count");
            return;
        }

        SKSE::log::info("Loading tracking data for {} diary volumes", totalVolumes);

        for (std::uint32_t i = 0; i < totalVolumes; ++i) {
            // Read UUID
            std::uint32_t uuidLen;
            if (!a_intfc->ReadRecordData(&uuidLen, sizeof(uuidLen))) {
                SKSE::log::error("Failed to read UUID length");
                break;
            }

            std::string uuid;
            uuid.resize(uuidLen);
            if (!a_intfc->ReadRecordData(uuid.data(), uuidLen)) {
                SKSE::log::error("Failed to read UUID");
                break;
            }

            // Read actor name
            std::uint32_t nameLen;
            if (!a_intfc->ReadRecordData(&nameLen, sizeof(nameLen))) {
                SKSE::log::error("Failed to read actor name length");
                break;
            }

            std::string actorName;
            actorName.resize(nameLen);
            if (!a_intfc->ReadRecordData(actorName.data(), nameLen)) {
                SKSE::log::error("Failed to read actor name");
                break;
            }

            // Read FormID and resolve it (may have changed due to load order)
            RE::FormID oldFormId;
            if (!a_intfc->ReadRecordData(&oldFormId, sizeof(oldFormId))) {
                SKSE::log::error("Failed to read book FormID");
                break;
            }

            RE::FormID newFormId;
            if (!a_intfc->ResolveFormID(oldFormId, newFormId)) {
                SKSE::log::error("Failed to resolve FormID 0x{:X} for {} - DPF form may not exist", oldFormId, actorName);
                // Skip this entry entirely if we can't resolve the FormID
                // This prevents crashes from referencing invalid forms
                
                // Read remaining data to keep stream aligned
                double dummyStart, dummyEnd;
                int dummyVol;
                a_intfc->ReadRecordData(&dummyStart, sizeof(dummyStart));
                a_intfc->ReadRecordData(&dummyEnd, sizeof(dummyEnd));
                a_intfc->ReadRecordData(&dummyVol, sizeof(dummyVol));
                // Read template string (may not exist in older saves)
                std::uint32_t dummyTemplateLen = 0;
                if (a_intfc->ReadRecordData(&dummyTemplateLen, sizeof(dummyTemplateLen)) && dummyTemplateLen > 0) {
                    std::string dummyTemplate;
                    dummyTemplate.resize(dummyTemplateLen);
                    a_intfc->ReadRecordData(dummyTemplate.data(), dummyTemplateLen);
                }
                // Read entry count (may not exist in older saves)
                int dummyCount = 0;
                a_intfc->ReadRecordData(&dummyCount, sizeof(dummyCount));
                // Read bio template name (may not exist in older saves)
                std::uint32_t dummyBioLen = 0;
                if (a_intfc->ReadRecordData(&dummyBioLen, sizeof(dummyBioLen)) && dummyBioLen > 0) {
                    std::string dummyBio;
                    dummyBio.resize(dummyBioLen);
                    a_intfc->ReadRecordData(dummyBio.data(), dummyBioLen);
                }
                // Read prevVolumeLastCreationTime (version 2+)
                if (version >= 2) {
                    double dummyPrevLastCT = 0.0;
                    a_intfc->ReadRecordData(&dummyPrevLastCT, sizeof(dummyPrevLastCT));
                }
                // Read prevVolumeCountAtBoundary (version 3+)
                if (version >= 3) {
                    int dummyCountAtBoundary = 0;
                    a_intfc->ReadRecordData(&dummyCountAtBoundary, sizeof(dummyCountAtBoundary));
                }
                continue;
            }
            
            // Verify the form actually exists and is a book
            auto* form = RE::TESForm::LookupByID(newFormId);
            if (!form || form->GetFormType() != RE::FormType::Book) {
                SKSE::log::error("FormID 0x{:X} resolved but form is invalid/not a book for {}", newFormId, actorName);
                // Read remaining data to keep stream aligned
                double dummyStart, dummyEnd;
                int dummyVol;
                a_intfc->ReadRecordData(&dummyStart, sizeof(dummyStart));
                a_intfc->ReadRecordData(&dummyEnd, sizeof(dummyEnd));
                a_intfc->ReadRecordData(&dummyVol, sizeof(dummyVol));
                // Read template string (may not exist in older saves)
                std::uint32_t dummyTemplateLen = 0;
                if (a_intfc->ReadRecordData(&dummyTemplateLen, sizeof(dummyTemplateLen)) && dummyTemplateLen > 0) {
                    std::string dummyTemplate;
                    dummyTemplate.resize(dummyTemplateLen);
                    a_intfc->ReadRecordData(dummyTemplate.data(), dummyTemplateLen);
                }
                // Read entry count (may not exist in older saves)
                int dummyCount = 0;
                a_intfc->ReadRecordData(&dummyCount, sizeof(dummyCount));
                // Read bio template name (may not exist in older saves)
                std::uint32_t dummyBioLen = 0;
                if (a_intfc->ReadRecordData(&dummyBioLen, sizeof(dummyBioLen)) && dummyBioLen > 0) {
                    std::string dummyBio;
                    dummyBio.resize(dummyBioLen);
                    a_intfc->ReadRecordData(dummyBio.data(), dummyBioLen);
                }
                // Read prevVolumeLastCreationTime (version 2+)
                if (version >= 2) {
                    double dummyPrevLastCT2 = 0.0;
                    a_intfc->ReadRecordData(&dummyPrevLastCT2, sizeof(dummyPrevLastCT2));
                }
                // Read prevVolumeCountAtBoundary (version 3+)
                if (version >= 3) {
                    int dummyCountAtBoundary2 = 0;
                    a_intfc->ReadRecordData(&dummyCountAtBoundary2, sizeof(dummyCountAtBoundary2));
                }
                continue;
            }

            // Read timestamps
            double startTime, endTime;
            if (!a_intfc->ReadRecordData(&startTime, sizeof(startTime))) {
                SKSE::log::error("Failed to read startTime");
                break;
            }
            if (!a_intfc->ReadRecordData(&endTime, sizeof(endTime))) {
                SKSE::log::error("Failed to read endTime");
                break;
            }
            
            // Read volume number
            int volumeNumber = 1;
            if (!a_intfc->ReadRecordData(&volumeNumber, sizeof(volumeNumber))) {
                SKSE::log::warn("Failed to read volume number, defaulting to 1");
                volumeNumber = 1;
            }
            
            // Read journal template (optional - may not exist in older saves)
            std::string journalTemplate;
            std::uint32_t templateLen = 0;
            if (a_intfc->ReadRecordData(&templateLen, sizeof(templateLen))) {
                if (templateLen > 0) {
                    journalTemplate.resize(templateLen);
                    if (!a_intfc->ReadRecordData(journalTemplate.data(), templateLen)) {
                        SKSE::log::warn("Failed to read journal template - using default");
                        journalTemplate.clear();
                    }
                }
            }
            
            // Read entry count (optional - may not exist in older saves)
            int entryCount = 0;
            if (!a_intfc->ReadRecordData(&entryCount, sizeof(entryCount))) {
                SKSE::log::debug("Entry count not found in save (older version), will be calculated on first check");
                entryCount = 0;
            }

            // Read bio template name (optional - may not exist in older saves)
            std::string bioTemplateName;
            std::uint32_t bioNameLen = 0;
            if (a_intfc->ReadRecordData(&bioNameLen, sizeof(bioNameLen)) && bioNameLen > 0) {
                bioTemplateName.resize(bioNameLen);
                if (!a_intfc->ReadRecordData(bioTemplateName.data(), bioNameLen)) {
                    SKSE::log::warn("Failed to read bio template name - will re-derive on next book open");
                    bioTemplateName.clear();
                }
            }

            // Read prevVolumeLastCreationTime (version 2+: boundary de-dup for same-date entries).
            double prevVolLastCT = 0.0;
            if (version >= 2) {
                if (!a_intfc->ReadRecordData(&prevVolLastCT, sizeof(prevVolLastCT))) {
                    SKSE::log::debug("prevVolumeLastCreationTime not found in save - defaulting to 0.0");
                }
            }
            // Read prevVolumeCountAtBoundary (version 3+: exact removal count at boundary).
            int prevVolCountAtBoundary = 0;
            if (version >= 3) {
                if (!a_intfc->ReadRecordData(&prevVolCountAtBoundary, sizeof(prevVolCountAtBoundary))) {
                    SKSE::log::debug("prevVolumeCountAtBoundary not found in save - defaulting to 0");
                }
            }

            // Migrate old cosaves: volume 1 should always use startTime=0.0 so the
            // oldest diary entry is never excluded by an exclusive API lower bound.
            if (volumeNumber == 1 && startTime > 0.0) {
                SKSE::log::info("Migrating volume 1 startTime from {} to 0.0 for {}", startTime, actorName);
                startTime = 0.0;
            }

            // Restore the tracking data (book already exists via DPF persistence)
            DiaryBookData data;
            data.actorUuid = uuid;
            data.actorName = actorName;
            data.bookFormId = newFormId;  // Use resolved FormID
            data.startTime = startTime;
            data.endTime = endTime;
            data.volumeNumber = volumeNumber;
            data.journalTemplate = journalTemplate;
            data.bioTemplateName = bioTemplateName;
            data.lastKnownEntryCount = entryCount;
            data.prevVolumeLastCreationTime = prevVolLastCT;
            data.prevVolumeCountAtBoundary = prevVolCountAtBoundary;
            
            // Add to vector for this UUID
            books_[uuid].push_back(data);
            
            SKSE::log::info("Restored tracking: {} Volume {} (FormID: 0x{:X} -> 0x{:X}) [{}-{}] (template: {})", 
                           actorName, volumeNumber, oldFormId, newFormId, startTime, endTime, 
                           journalTemplate.empty() ? "default" : journalTemplate);
        }

        SKSE::log::info("Restored tracking data for {} diary volumes across {} actors", totalVolumes, books_.size());
        
        // Load actor template mappings (optional - may not exist in older saves)
        std::uint32_t actorTemplateCount = 0;
        if (a_intfc->ReadRecordData(&actorTemplateCount, sizeof(actorTemplateCount))) {
            SKSE::log::info("Loading {} actor template mappings", actorTemplateCount);
            
            for (std::uint32_t i = 0; i < actorTemplateCount; ++i) {
                // Read UUID
                std::uint32_t uuidLen;
                if (!a_intfc->ReadRecordData(&uuidLen, sizeof(uuidLen))) {
                    SKSE::log::error("Failed to read UUID length for actor template");
                    break;
                }
                
                std::string uuid;
                uuid.resize(uuidLen);
                if (!a_intfc->ReadRecordData(uuid.data(), uuidLen)) {
                    SKSE::log::error("Failed to read UUID for actor template");
                    break;
                }
                
                // Read template name
                std::uint32_t templateLen;
                if (!a_intfc->ReadRecordData(&templateLen, sizeof(templateLen))) {
                    SKSE::log::error("Failed to read template name length");
                    break;
                }
                
                std::string templateName;
                templateName.resize(templateLen);
                if (!a_intfc->ReadRecordData(templateName.data(), templateLen)) {
                    SKSE::log::error("Failed to read template name");
                    break;
                }
                
                actorTemplates_[uuid] = templateName;
            }
            
            if (actorTemplateCount > 0) {
                SKSE::log::info("Restored {} actor template mappings", actorTemplates_.size());
            }
        }
    }

    void BookManager::Revert() {
        // DO NOT dispose DPF forms here - they are global across all saves
        // Disposing them would break forms that other saves still reference
        // DPF forms are managed per-save via co-save serialization
        // Only the current save's BookManager data needs to be cleared
        
        books_.clear();
        actorTemplates_.clear();
        SKSE::log::info("Cleared book tracking data for new game (DPF forms preserved for other saves)");
    }

} // namespace SkyrimNetDiaries
