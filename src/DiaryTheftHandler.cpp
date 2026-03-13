#include "PCH.h"
#include "DiaryTheftHandler.h"
#include "BookManager.h"
#include "Database.h"
#include <mutex>

namespace DiaryTheftHandler {
    
    // Track dialogue/container state for legitimate trade detection.
    // We only need to know whether dialogue was open when a container opened —
    // not which specific NPC, so we avoid MenuTopicManager::speaker.get()
    // which uses a REL::ID lookup that is unavailable without the VR Address Library.
    namespace {
        bool g_dialogueIsOpen = false;      // Is "Dialogue Menu" currently open?
        bool g_consoleIsOpen = false;        // Is the console currently open?
        bool g_legitimateTradeActive = false; // Container opened during dialogue (no console)
        std::mutex g_dialogueMutex;
    }
    
    // Menu event handler to track dialogue (open/close only — no speaker lookup)
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

            std::lock_guard<std::mutex> lock(g_dialogueMutex);
            g_dialogueIsOpen = a_event->opening;

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

            std::lock_guard<std::mutex> lock(g_dialogueMutex);
            if (a_event->opening) {
                // Legitimate trade = container opened while dialogue is active and console is not
                g_legitimateTradeActive = !g_consoleIsOpen && g_dialogueIsOpen;
            } else {
                g_legitimateTradeActive = false;
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

            // Look up our tracking data first — this is the authoritative ownership check.
            // actorFormId was stored at DPF book-creation time and is more reliable than
            // a substring name search (handles titled NPCs, apostrophes, etc.).
            auto* bookManager = SkyrimNetDiaries::BookManager::GetSingleton();
            auto* bookData = bookManager->GetBookForFormID(a_event->baseObj);
            if (!bookData) {
                return; // Not one of our tracked diary volumes
            }

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

            // Confirm this diary belongs to the source actor via UUID comparison.
            // UUIDs are stable across load-order changes (unlike stored FormIDs which break
            // for ESL-flagged plugins). We resolve the source actor's UUID from its live
            // FormID (safe — we hold an in-memory actor pointer, so the runtime FormID is
            // always correct). Falls back to name-in-title for volumes whose actorUuid was
            // not yet assigned by SkyrimNet at creation time.
            std::string sourceUuid = SkyrimNetDiaries::Database::GetUUIDFromFormID(sourceActor->GetFormID());
            if (!sourceUuid.empty() && sourceUuid != "0" &&
                !bookData->actorUuid.empty() && bookData->actorUuid != "0") {
                if (sourceUuid != bookData->actorUuid) {
                    SKSE::log::info("[Physical Diaries] Diary '{}' belongs to UUID '{}', not {} ('{}') — ignoring",
                                   bookName, bookData->actorUuid,
                                   sourceActor->GetName(), sourceUuid);
                    return;
                }
            } else {
                // Legacy fallback: UUID not yet available, use name-in-title check
                std::string sourceName = sourceActor->GetName();
                if (bookName.find(sourceName) == std::string::npos) {
                    return;
                }
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
                bool isLegitimateTransfer = false;
                {
                    std::lock_guard<std::mutex> lock(g_dialogueMutex);
                    isLegitimateTransfer = g_legitimateTradeActive;
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

            // Apply the stolen diary effect for ANY volume that belongs to this NPC
            // Even old volumes matter - they are still personal diaries
            if (bookData) {
                SKSE::log::debug("[Physical Diaries] Applying theft tracking for {} volume {} - any diary theft matters", 
                               sourceActor->GetName(), bookData->volumeNumber);
                // Derive UUID fresh from the actor's FormID so it always matches what
                // IsDiaryStolen uses (GetUUIDFromFormID on the same FormID).  Using
                // bookData->actorUuid caused mismatches when SkyrimNet hadn't yet
                // assigned a stable UUID at book-creation time.
                std::string liveUuid = SkyrimNetDiaries::Database::GetUUIDFromFormID(sourceActor->GetFormID());
                if (liveUuid.empty() || liveUuid == "0") {
                    SKSE::log::warn("[Physical Diaries] Could not resolve UUID for {} (FormID 0x{:X}) — falling back to stored UUID",
                                   sourceActor->GetName(), sourceActor->GetFormID());
                    liveUuid = bookData->actorUuid;
                }
                SKSE::log::debug("[Physical Diaries] Using live UUID {} for theft tracking (stored: {})",
                               liveUuid, bookData->actorUuid);
                ApplyStolenDiaryEffect(sourceActor, bookName, liveUuid, bookData->volumeNumber);
            } else {
                SKSE::log::warn("[Physical Diaries] Could not find book data for stolen diary FormID 0x{:X}", 
                               a_event->baseObj);
            }
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

            // Use BookManager as the authoritative source for volume tracking.
            // More reliable than scanning the NPC's current inventory, which may not
            // include newer volumes that are stored in containers elsewhere.
            auto* bookManager = SkyrimNetDiaries::BookManager::GetSingleton();
            auto* bookData = bookManager->GetBookForFormID(a_event->baseObj);
            if (!bookData) {
                // Not one of our tracked diary volumes - nothing to do
                return;
            }

            // Confirm the diary actually belongs to the NPC receiving it.
            // Giving someone else's diary to an NPC must not clear that NPC's stolen marker.
            // Use live UUID from the actor's FormID (same as what IsDiaryStolen and the
            // theft-recording path use) rather than bookData->actorUuid which may have
            // been stored before SkyrimNet assigned a stable UUID.
            std::string destUuid = SkyrimNetDiaries::Database::GetUUIDFromFormID(destActor->GetFormID());
            if (destUuid.empty() || destUuid == "0") {
                SKSE::log::warn("[Physical Diaries] Could not resolve live UUID for {} — using stored UUID for return check",
                               destActor->GetName());
                destUuid = bookData->actorUuid;
            }
            // The book's owner UUID may also be the stale version; resolve it live too.
            std::string bookOwnerUuid = SkyrimNetDiaries::Database::GetUUIDFromFormID(
                SkyrimNetDiaries::Database::GetFormIDForUUID(bookData->actorUuid));
            if (bookOwnerUuid.empty() || bookOwnerUuid == "0") {
                bookOwnerUuid = bookData->actorUuid;
            }
            if (bookOwnerUuid != destUuid) {
                SKSE::log::info("[Physical Diaries] '{}' belongs to UUID '{}', not '{}' ({}) - ignoring for theft purposes",
                               bookName, bookOwnerUuid, destActor->GetName(), destUuid);
                return;
            }

            int returnedVolume = bookData->volumeNumber;
            int maxVolume = returnedVolume;

            auto* allVolumes = bookManager->GetAllVolumesForActor(bookData->actorUuid);
            if (allVolumes) {
                for (const auto& vol : *allVolumes) {
                    if (vol.volumeNumber > maxVolume) maxVolume = vol.volumeNumber;
                }
            }

            if (returnedVolume < maxVolume) {
                SKSE::log::info("[Physical Diaries] '{}' returned (v{}) but NPC has written up to v{} - removing from stolen list but may still show stolen if other volumes missing",
                               bookName, returnedVolume, maxVolume);
                RemoveStolenDiaryEffect(destActor, bookName, destUuid, returnedVolume);
            } else {
                SKSE::log::info("[Physical Diaries] '{}' returned (v{}, NPC's latest) - removing from stolen list",
                               bookName, returnedVolume);
                RemoveStolenDiaryEffect(destActor, bookName, destUuid, returnedVolume);
            }
        }

        void ApplyStolenDiaryEffect(RE::Actor* actor, const std::string& bookName, const std::string& actorUuid, int volumeNumber) {
            if (!actor) {
                SKSE::log::warn("[Physical Diaries] ApplyStolenDiaryEffect called with null actor");
                return;
            }
            
            // Record this specific volume as stolen in DB
            auto calendar = RE::Calendar::GetSingleton();
            if (calendar) {
                double gameTime = calendar->GetCurrentGameTime() * 86400.0;
                auto* diaryDB = SkyrimNetDiaries::DiaryDB::GetSingleton();
                
                diaryDB->AddStolenVolume(actorUuid, volumeNumber, gameTime);
                // Update last_known_game_time so backwards time travel detection works
                // even if the player doesn't save after stealing
                diaryDB->UpdateLastKnownGameTime(actorUuid, gameTime);
                
                SKSE::log::debug("[Physical Diaries] Recorded volume {} theft for {} at game time {:.2f} (UUID: {}, book: {})", 
                               volumeNumber, actor->GetName(), gameTime, actorUuid, bookName);
            } else {
                SKSE::log::error("[Physical Diaries] Failed to get Calendar singleton for theft tracking");
            }
        }

        void RemoveStolenDiaryEffect(RE::Actor* actor, const std::string& bookName, const std::string& actorUuid, int volumeNumber) {
            if (!actor) {
                SKSE::log::warn("[Physical Diaries] RemoveStolenDiaryEffect called with null actor");
                return;
            }
            
            // Remove this specific volume from stolen list
            auto* diaryDB = SkyrimNetDiaries::DiaryDB::GetSingleton();
            diaryDB->RemoveStolenVolume(actorUuid, volumeNumber);
            
            // Update last_known_game_time for backwards time travel detection
            auto calendar = RE::Calendar::GetSingleton();
            if (calendar) {
                double gameTime = calendar->GetCurrentGameTime() * 86400.0;
                diaryDB->UpdateLastKnownGameTime(actorUuid, gameTime);
            }
            
            SKSE::log::debug("[Physical Diaries] Removed volume {} from stolen list for {} (returned: {})",
                           volumeNumber, actor->GetName(), bookName);
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
        // Verify that required forms exist in the ESP
        // This is just a diagnostic check - the decorator system doesn't need these
        SKSE::log::info("[Physical Diaries] ESP verification: Using database-backed theft tracking");
    }
}
