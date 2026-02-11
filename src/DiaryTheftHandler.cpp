#include "PCH.h"
#include "DiaryTheftHandler.h"
#include <unordered_set>
#include <mutex>

namespace DiaryTheftHandler {
    
    // Track NPCs the player recently interacted with for legitimate trades
    namespace {
        std::unordered_set<RE::FormID> g_currentDialogueNPCs; // NPCs currently in active dialogue
        std::unordered_set<RE::FormID> g_currentLegitimateTradeNPCs; // NPCs in legitimate trade (dialogue+container)
        bool g_consoleIsOpen = false; // Track if console is currently open
        std::mutex g_dialogueMutex;
    }
    
    // Menu event handler to track dialogue
    class MenuEventHandler : public RE::BSTEventSink<RE::MenuOpenCloseEvent> {
    public:
        static MenuEventHandler* GetSingleton() {
            static MenuEventHandler singleton;
            return &singleton;
        }

        RE::BSEventNotifyControl ProcessEvent(const RE::MenuOpenCloseEvent* a_event,
                                               RE::BSTEventSource<RE::MenuOpenCloseEvent>*) override {
            if (!a_event || a_event->menuName != "Dialogue Menu") {
                return RE::BSEventNotifyControl::kContinue;
            }

            if (a_event->opening) {
                // Dialogue menu opened - track the NPC
                auto* menuTopicManager = RE::MenuTopicManager::GetSingleton();
                if (menuTopicManager && menuTopicManager->speaker) {
                    std::lock_guard<std::mutex> lock(g_dialogueMutex);
                    auto formID = menuTopicManager->speaker.get()->GetFormID();
                    g_currentDialogueNPCs.insert(formID);
                }
            } else {
                // Dialogue menu closed - untrack the NPC
                auto* menuTopicManager = RE::MenuTopicManager::GetSingleton();
                if (menuTopicManager && menuTopicManager->speaker) {
                    std::lock_guard<std::mutex> lock(g_dialogueMutex);
                    auto formID = menuTopicManager->speaker.get()->GetFormID();
                    g_currentDialogueNPCs.erase(formID);
                }
            }

            return RE::BSEventNotifyControl::kContinue;
        }

    private:
        MenuEventHandler() = default;
        MenuEventHandler(const MenuEventHandler&) = delete;
        MenuEventHandler(MenuEventHandler&&) = delete;
        MenuEventHandler& operator=(const MenuEventHandler&) = delete;
        MenuEventHandler& operator=(MenuEventHandler&&) = delete;
    };
    
    // Console menu event handler to detect console usage
    class ConsoleMenuHandler : public RE::BSTEventSink<RE::MenuOpenCloseEvent> {
    public:
        static ConsoleMenuHandler* GetSingleton() {
            static ConsoleMenuHandler singleton;
            return &singleton;
        }

        RE::BSEventNotifyControl ProcessEvent(const RE::MenuOpenCloseEvent* a_event,
                                               RE::BSTEventSource<RE::MenuOpenCloseEvent>*) override {
            if (!a_event || a_event->menuName != "Console") {
                return RE::BSEventNotifyControl::kContinue;
            }

            std::lock_guard<std::mutex> lock(g_dialogueMutex);
            g_consoleIsOpen = a_event->opening;

            return RE::BSEventNotifyControl::kContinue;
        }

    private:
        ConsoleMenuHandler() = default;
        ConsoleMenuHandler(const ConsoleMenuHandler&) = delete;
        ConsoleMenuHandler(ConsoleMenuHandler&&) = delete;
        ConsoleMenuHandler& operator=(const ConsoleMenuHandler&) = delete;
        ConsoleMenuHandler& operator=(ConsoleMenuHandler&&) = delete;
    };
    
    // Container menu event handler to clear dialogue tracking
    class ContainerMenuHandler : public RE::BSTEventSink<RE::MenuOpenCloseEvent> {
    public:
        static ContainerMenuHandler* GetSingleton() {
            static ContainerMenuHandler singleton;
            return &singleton;
        }

        RE::BSEventNotifyControl ProcessEvent(const RE::MenuOpenCloseEvent* a_event,
                                               RE::BSTEventSource<RE::MenuOpenCloseEvent>*) override {
            if (!a_event || a_event->menuName != "ContainerMenu") {
                return RE::BSEventNotifyControl::kContinue;
            }

            if (a_event->opening) {
                // Container menu opened - check if Dialogue Menu is currently open AND console is NOT open
                // If dialogue is open and console is closed, this is a legitimate trade
                std::lock_guard<std::mutex> lock(g_dialogueMutex);
                
                g_currentLegitimateTradeNPCs.clear();
                
                if (g_consoleIsOpen) {
                    // Console is open - this is a console command (openactorcontainer)
                } else if (!g_currentDialogueNPCs.empty()) {
                    // Dialogue is open and console is not - legitimate trade
                    g_currentLegitimateTradeNPCs = g_currentDialogueNPCs;
                } else {
                    // Neither dialogue nor console - probably container activation
                }
            } else {
                // Container menu closed - clear legitimate trade flags
                std::lock_guard<std::mutex> lock(g_dialogueMutex);
                g_currentLegitimateTradeNPCs.clear();
            }

            return RE::BSEventNotifyControl::kContinue;
        }

    private:
        ContainerMenuHandler() = default;
        ContainerMenuHandler(const ContainerMenuHandler&) = delete;
        ContainerMenuHandler(ContainerMenuHandler&&) = delete;
        ContainerMenuHandler& operator=(const ContainerMenuHandler&) = delete;
        ContainerMenuHandler& operator=(ContainerMenuHandler&&) = delete;
    };
    
    class ContainerChangeHandler : public RE::BSTEventSink<RE::TESContainerChangedEvent> {
    public:
        static ContainerChangeHandler* GetSingleton() {
            static ContainerChangeHandler singleton;
            return &singleton;
        }

        RE::BSEventNotifyControl ProcessEvent(const RE::TESContainerChangedEvent* a_event,
                                               RE::BSTEventSource<RE::TESContainerChangedEvent>*) override {
            if (!a_event) {
                return RE::BSEventNotifyControl::kContinue;
            }

            auto* player = RE::PlayerCharacter::GetSingleton();
            if (!player) {
                return RE::BSEventNotifyControl::kContinue;
            }

            // Get the item being transferred
            auto* baseItem = RE::TESForm::LookupByID<RE::TESBoundObject>(a_event->baseObj);
            if (!baseItem || baseItem->GetFormType() != RE::FormType::Book) {
                return RE::BSEventNotifyControl::kContinue;
            }

            auto* book = baseItem->As<RE::TESObjectBOOK>();
            if (!book) {
                return RE::BSEventNotifyControl::kContinue;
            }

            // Check if it's a diary (name contains "Diary" or "diary")
            std::string bookName = book->GetFullName();
            if (bookName.find("Diary") == std::string::npos && bookName.find("diary") == std::string::npos) {
                return RE::BSEventNotifyControl::kContinue;
            }

            // Case 1: Player RECEIVED a diary from an NPC (potential theft)
            if (a_event->newContainer == player->GetFormID()) {
                HandleDiaryAcquired(a_event, bookName);
            }
            // Case 2: Player GAVE a diary to an NPC (potential return)
            else if (a_event->oldContainer == player->GetFormID()) {
                HandleDiaryReturned(a_event, bookName);
            }

            return RE::BSEventNotifyControl::kContinue;
        }

    private:
        void HandleDiaryAcquired(const RE::TESContainerChangedEvent* a_event, const std::string& bookName) {
            auto* player = RE::PlayerCharacter::GetSingleton();
            
            // Get source actor (who had the diary)
            auto* sourceRef = RE::TESForm::LookupByID<RE::TESObjectREFR>(a_event->oldContainer);
            if (!sourceRef) {
                return; // Not from a reference (crafting, etc.)
            }

            auto* sourceActor = sourceRef->As<RE::Actor>();
            if (!sourceActor || sourceActor == player) {
                return; // Not from an NPC or from player themselves
            }

            // Ignore dead actors (corpse looting doesn't count as theft for our purposes)
            if (sourceActor->IsDead()) {
                return;
            }

            // Check if this diary actually belongs to the source NPC
            std::string sourceName = sourceActor->GetName();
            if (bookName.find(sourceName) == std::string::npos) {
                return;
            }

            // Check if the book was flagged as stolen (has ownership data)
            // This is the reliable way to detect actual theft vs console commands/legitimate trades
            auto* baseItem = RE::TESForm::LookupByID<RE::TESBoundObject>(a_event->baseObj);
            if (!baseItem) {
                return;
            }

            bool wasStolen = false;
            auto inv = player->GetInventory();
            for (const auto& [item, invData] : inv) {
                if (item->GetFormID() == baseItem->GetFormID() && invData.first > 0) {
                    // Found the item in player's inventory - check if any instances are stolen
                    if (invData.second && invData.second->extraLists) {
                        for (auto& xList : *invData.second->extraLists) {
                            if (xList && xList->HasType(RE::ExtraDataType::kOwnership)) {
                                // Item has ownership data (stolen flag)
                                wasStolen = true;
                                break;
                            }
                        }
                    }
                    break;
                }
            }

            if (!wasStolen) {
                // Check if this NPC is currently in a legitimate trade session (container menu opened within 3s of dialogue)
                bool isLegitimateTransfer = false;
                
                {
                    std::lock_guard<std::mutex> lock(g_dialogueMutex);
                    isLegitimateTransfer = g_currentLegitimateTradeNPCs.contains(sourceActor->GetFormID());
                }
                
                if (isLegitimateTransfer) {
                    // Dispatch mod event for SkyrimNet trigger
                    HandleLegitimateTransfer(sourceActor, bookName);
                } else {
                    // Not flagged as stolen and not a legitimate trade
                    // This is either a console command or edge case - just ignore
                }
                
                return;
            }

            // Item is flagged as stolen - this was actual theft (pickpocketing or stealing)
            SKSE::log::info("[Physical Diaries] Player stole {} from {} (item flagged as stolen)", 
                           bookName, sourceActor->GetName());

            // Apply the stolen diary effect
            ApplyStolenDiaryEffect(sourceActor, bookName);
        }

        void HandleDiaryReturned(const RE::TESContainerChangedEvent* a_event, const std::string& bookName) {
            auto* player = RE::PlayerCharacter::GetSingleton();
            
            // Get destination actor (who received the diary back)
            auto* destRef = RE::TESForm::LookupByID<RE::TESObjectREFR>(a_event->newContainer);
            if (!destRef) {
                return; // Not to a reference (dropped, etc.)
            }

            auto* destActor = destRef->As<RE::Actor>();
            if (!destActor || destActor == player) {
                return; // Not to an NPC or to player themselves
            }

            // Ignore dead actors
            if (destActor->IsDead()) {
                return;
            }

            // Extract volume number from the returned diary name (e.g., "Lydia's Diary, v2" -> 2)
            int returnedVolume = 1; // Default to volume 1 if no version number
            size_t vPos = bookName.find(", v");
            if (vPos != std::string::npos) {
                try {
                    returnedVolume = std::stoi(bookName.substr(vPos + 3));
                } catch (...) {
                    // Failed to parse, assume v1
                }
            }

            // Check if the NPC has a higher volume number in their inventory
            // If they do, this returned diary is old and the NPC has moved on
            std::string npcName = destActor->GetName();
            int highestNPCVolume = returnedVolume; // Assume returned volume is highest unless we find higher
            
            auto npcInv = destActor->GetInventory();
            for (const auto& [item, invData] : npcInv) {
                if (item->GetFormType() != RE::FormType::Book) continue;
                
                auto* book = item->As<RE::TESObjectBOOK>();
                if (!book) continue;
                
                std::string itemName = book->GetFullName();
                
                // Check if this is their diary
                if ((itemName.find("Diary") == std::string::npos && itemName.find("diary") == std::string::npos) ||
                    itemName.find(npcName) == std::string::npos) {
                    continue;
                }
                
                // Extract volume number
                size_t vPos2 = itemName.find(", v");
                if (vPos2 != std::string::npos) {
                    try {
                        int volume = std::stoi(itemName.substr(vPos2 + 3));
                        if (volume > highestNPCVolume) {
                            highestNPCVolume = volume;
                        }
                    } catch (...) {}
                }
            }
            
            if (returnedVolume < highestNPCVolume) {
                SKSE::log::info("[Physical Diaries] {} returned v{} but NPC has v{} - they've moved on, keeping faction marker", 
                               bookName, returnedVolume, highestNPCVolume);
            } else {
                SKSE::log::info("[Physical Diaries] {} returned (v{}, NPC's latest) - removing faction marker", 
                               bookName, returnedVolume);
                RemoveStolenDiaryEffect(destActor, bookName);
            }
        }

        void ApplyStolenDiaryEffect(RE::Actor* actor, const std::string& bookName) {
            // Look up the faction that marks a stolen diary
            auto* faction = RE::TESForm::LookupByEditorID<RE::TESFaction>("SNPD_DiaryStolenFaction");
            if (!faction) {
                SKSE::log::error("[Physical Diaries] CRITICAL: Failed to find SNPD_DiaryStolenFaction in ESP");
                SKSE::log::error("[Physical Diaries] Make sure the ESP contains a faction with Editor ID: SNPD_DiaryStolenFaction");
                return;
            }

            SKSE::log::info("[Physical Diaries] Found SNPD_DiaryStolenFaction (FormID: 0x{:X})", faction->GetFormID());

            // Check if actor is already in this faction
            if (actor->IsInFaction(faction)) {
                SKSE::log::info("[Physical Diaries] {} already in SNPD_DiaryStolenFaction (diary already marked stolen)", 
                               actor->GetName());
                return;
            }

            // Add actor to the faction with rank 1
            actor->AddToFaction(faction, 1);
            
            // VERIFY the faction was actually added
            if (actor->IsInFaction(faction)) {
                SKSE::log::info("[Physical Diaries] ✓ SUCCESS: Added {} to SNPD_DiaryStolenFaction (stolen: {})", 
                               actor->GetName(), bookName);
            } else {
                SKSE::log::error("[Physical Diaries] ✗ FAILED: AddToFaction() called but actor not in faction!", 
                                actor->GetName());
            }
        }

        void RemoveStolenDiaryEffect(RE::Actor* actor, const std::string& bookName) {
            // Look up the faction that marks a stolen diary
            auto* faction = RE::TESForm::LookupByEditorID<RE::TESFaction>("SNPD_DiaryStolenFaction");
            if (!faction) {
                SKSE::log::error("[Physical Diaries] CRITICAL: Failed to find SNPD_DiaryStolenFaction in ESP");
                return;
            }

            // Check if actor is in this faction
            if (!actor->IsInFaction(faction)) {
                SKSE::log::info("[Physical Diaries] {} not in SNPD_DiaryStolenFaction (diary was never marked stolen or already removed)", 
                               actor->GetName());
                return;
            }

            // Remove actor from the faction
            actor->RemoveFromFaction(faction);
            
            // VERIFY the faction was actually removed
            if (!actor->IsInFaction(faction)) {
                SKSE::log::info("[Physical Diaries] ✓ SUCCESS: Removed {} from SNPD_DiaryStolenFaction (returned: {})", 
                               actor->GetName(), bookName);
            } else {
                SKSE::log::error("[Physical Diaries] ✗ FAILED: RemoveFromFaction() called but actor still in faction!", 
                                actor->GetName());
            }
        }

        void HandleLegitimateTransfer(RE::Actor* actor, const std::string& bookName) {
            // Send mod event for SkyrimNet to detect and generate NPC dialogue
            // Event: "PhysicalDiary_Shared" with actor FormID as numeric argument
            
            auto* eventSource = SKSE::GetModCallbackEventSource();
            if (!eventSource) {
                SKSE::log::warn("[Physical Diaries] Cannot send mod event - event source unavailable");
                return;
            }
            
            SKSE::ModCallbackEvent event{
                "PhysicalDiary_Shared",
                bookName.c_str(),
                static_cast<float>(actor->GetFormID()),
                actor
            };
            
            eventSource->SendEvent(&event);
            
            SKSE::log::info("[Physical Diaries] ✓ Sent 'PhysicalDiary_Shared' mod event for {} (FormID: 0x{:X}, legitimate transfer of {})", 
                           actor->GetName(), actor->GetFormID(), bookName);
        }

        ContainerChangeHandler() = default;
        ContainerChangeHandler(const ContainerChangeHandler&) = delete;
        ContainerChangeHandler(ContainerChangeHandler&&) = delete;
        ContainerChangeHandler& operator=(const ContainerChangeHandler&) = delete;
        ContainerChangeHandler& operator=(ContainerChangeHandler&&) = delete;
    };

    void Register() {
        auto* eventSourceHolder = RE::ScriptEventSourceHolder::GetSingleton();
        if (eventSourceHolder) {
            eventSourceHolder->AddEventSink(ContainerChangeHandler::GetSingleton());
            SKSE::log::info("Registered diary theft/return event handler (TESContainerChangedEvent)");
            
            // Note: ESP verification moved to kDataLoaded message handler in main.cpp
            // (it needs to run after ESPs are loaded)
        } else {
            SKSE::log::error("Failed to get ScriptEventSourceHolder for diary theft handler");
        }
        
        // Register menu event handlers for dialogue tracking
        auto* ui = RE::UI::GetSingleton();
        if (ui) {
            ui->AddEventSink<RE::MenuOpenCloseEvent>(MenuEventHandler::GetSingleton());
            ui->AddEventSink<RE::MenuOpenCloseEvent>(ConsoleMenuHandler::GetSingleton());
            ui->AddEventSink<RE::MenuOpenCloseEvent>(ContainerMenuHandler::GetSingleton());
            SKSE::log::info("Registered dialogue, console, and container menu tracking for legitimate transfers");
        } else {
            SKSE::log::error("Failed to get UI singleton for menu tracking");
        }
    }
    
    void VerifyESPSetup() {
        SKSE::log::info("=== Verifying ESP Setup for Physical Diaries ===");
        
        // Check for the faction
        auto* faction = RE::TESForm::LookupByEditorID<RE::TESFaction>("SNPD_DiaryStolenFaction");
        if (!faction) {
            SKSE::log::error("✗ SNPD_DiaryStolenFaction NOT FOUND in loaded ESPs!");
            SKSE::log::error("  Make sure 'SkyrimNet Physical Diaries.esp' is active in your load order");
            SKSE::log::error("  and contains a faction with Editor ID: SNPD_DiaryStolenFaction");
            return;
        }
        
        SKSE::log::info("✓ Found SNPD_DiaryStolenFaction");
        SKSE::log::info("  - FormID: 0x{:X}", faction->GetFormID());
        SKSE::log::info("  - Name: {}", faction->GetFullName());
        
        SKSE::log::info("=== ESP Verification Complete ===");
    }
    
    bool ClearStolenDiaryMarker(RE::Actor* actor) {
        if (!actor) {
            return false;
        }
        
        // Look up the faction that marks a stolen diary
        auto* faction = RE::TESForm::LookupByEditorID<RE::TESFaction>("SNPD_DiaryStolenFaction");
        if (!faction) {
            SKSE::log::error("[Physical Diaries] Failed to find SNPD_DiaryStolenFaction for cleanup");
            return false;
        }
        
        // Check if actor is in this faction
        if (!actor->IsInFaction(faction)) {
            return false; // Not marked as stolen, nothing to clear
        }
        
        // Remove actor from the faction
        actor->RemoveFromFaction(faction);
        
        // VERIFY the faction was actually removed
        if (!actor->IsInFaction(faction)) {
            SKSE::log::info("[Physical Diaries] ✓ Cleared stolen diary marker from {} (new entry generated)", 
                           actor->GetName());
            return true;
        } else {
            SKSE::log::error("[Physical Diaries] ✗ Failed to clear stolen diary marker from {}", 
                            actor->GetName());
            return false;
        }
    }
}
