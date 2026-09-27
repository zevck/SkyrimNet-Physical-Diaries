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

#include "PCH.h"
#include "DiaryTheftHandler.h"
#include "BookManager.h"
#include "Database.h"
#include "DiaryDB.h"
#include <mutex>

namespace DiaryTheftHandler {

    namespace {

        // Menu state for telling a willing handover from theft: a container opened
        // while dialogue is open (and the console isn't) is the dialogue trade menu,
        // e.g. a follower's inventory.  Only open/close is tracked, never the speaker
        // (MenuTopicManager::speaker needs an id the VR Address Library lacks).
        bool g_dialogueIsOpen = false;
        bool g_consoleIsOpen = false;
        bool g_legitimateTradeActive = false;
        std::mutex g_menuMutex;

        double GameTimeSeconds() {
            auto* calendar = RE::Calendar::GetSingleton();
            return calendar ? calendar->GetCurrentGameTime() * 86400.0 : 0.0;
        }

        // SkyrimNet UUID of a live actor, or "" when SkyrimNet doesn't know it.
        std::string UuidOf(const RE::Actor* actor) {
            std::string uuid = SkyrimNetDiaries::Database::GetUUIDFromFormID(actor->GetFormID());
            return uuid == "0" ? std::string{} : uuid;
        }

        // True if the player's copy of `book` carries ownership data, i.e. the engine
        // flagged it as stolen (pickpocketed or taken).  An unflagged copy came from a
        // trade, the console or similar.
        bool PlayerCopyIsStolen(RE::PlayerCharacter* player, RE::TESBoundObject* book) {
            auto inv = player->GetInventory([book](RE::TESBoundObject& a_obj) { return &a_obj == book; });
            auto it = inv.find(book);
            if (it == inv.end() || it->second.first <= 0 || !it->second.second || !it->second.second->extraLists) {
                return false;
            }
            for (auto* xList : *it->second.second->extraLists) {
                if (xList && xList->HasType(RE::ExtraDataType::kOwnership)) return true;
            }
            return false;
        }

        // Tracks dialogue, console and container menus in one sink.
        class MenuSink : public RE::BSTEventSink<RE::MenuOpenCloseEvent> {
        public:
            static MenuSink* GetSingleton() {
                static MenuSink singleton;
                return &singleton;
            }

            RE::BSEventNotifyControl ProcessEvent(const RE::MenuOpenCloseEvent* a_event,
                                                  RE::BSTEventSource<RE::MenuOpenCloseEvent>*) override {
                if (!a_event) return RE::BSEventNotifyControl::kContinue;
                std::lock_guard<std::mutex> lock(g_menuMutex);
                if (a_event->menuName == "Dialogue Menu") {
                    g_dialogueIsOpen = a_event->opening;
                } else if (a_event->menuName == "Console") {
                    g_consoleIsOpen = a_event->opening;
                } else if (a_event->menuName == "ContainerMenu") {
                    g_legitimateTradeActive = a_event->opening && g_dialogueIsOpen && !g_consoleIsOpen;
                }
                return RE::BSEventNotifyControl::kContinue;
            }

        private:
            MenuSink() = default;
        };

        void RecordTheft(const std::string& actorUuid, const std::string& actorName,
                         const std::string& bookName, int volumeNumber) {
            const double gameTime = GameTimeSeconds();
            auto* diaryDB = SkyrimNetDiaries::DiaryDB::GetSingleton();
            diaryDB->AddStolenVolume(actorUuid, volumeNumber, gameTime);
            // Stamp the time so a later load of an earlier save sees the travel back.
            diaryDB->UpdateLastKnownGameTime(actorUuid, gameTime);
            SKSE::log::info("[Physical Diaries] Player stole '{}' (vol {}) from {}", bookName, volumeNumber, actorName);
        }

        void RecordReturn(const std::string& actorUuid, const std::string& actorName,
                          const std::string& bookName, int volumeNumber) {
            auto* diaryDB = SkyrimNetDiaries::DiaryDB::GetSingleton();
            diaryDB->RemoveStolenVolume(actorUuid, volumeNumber);
            diaryDB->UpdateLastKnownGameTime(actorUuid, GameTimeSeconds());
            SKSE::log::info("[Physical Diaries] '{}' (vol {}) returned to {} - removed from stolen list",
                            bookName, volumeNumber, actorName);
        }

        void SendSharedEvent(RE::Actor* actor, const std::string& bookName) {
            // "PhysicalDiary_Shared": the player took the diary openly through the
            // dialogue trade menu.  Other mods (SkyrimNet triggers) can react to it.
            auto* eventSource = SKSE::GetModCallbackEventSource();
            if (!eventSource) {
                SKSE::log::warn("[Physical Diaries] Cannot send mod event - event source unavailable");
                return;
            }
            SKSE::ModCallbackEvent event{ "PhysicalDiary_Shared", bookName.c_str(),
                                          static_cast<float>(actor->GetFormID()), actor };
            eventSource->SendEvent(&event);
            SKSE::log::info("[Physical Diaries] ✓ Sent 'PhysicalDiary_Shared' mod event for {} (FormID: 0x{:X}, legitimate transfer of {})",
                            actor->GetName(), actor->GetFormID(), bookName);
        }

        // Watches diary volumes moving between the player and their owner.  A book is
        // a diary only if BookManager tracks its base FormID; names are never used
        // (they're localized, and same-named NPCs share them).
        class ContainerChangeHandler : public RE::BSTEventSink<RE::TESContainerChangedEvent> {
        public:
            static ContainerChangeHandler* GetSingleton() {
                static ContainerChangeHandler singleton;
                return &singleton;
            }

            RE::BSEventNotifyControl ProcessEvent(const RE::TESContainerChangedEvent* a_event,
                                                  RE::BSTEventSource<RE::TESContainerChangedEvent>*) override {
                try {
                    if (!a_event) return RE::BSEventNotifyControl::kContinue;
                    auto* player = RE::PlayerCharacter::GetSingleton();
                    if (!player) return RE::BSEventNotifyControl::kContinue;

                    const bool toPlayer = a_event->newContainer == player->GetFormID();
                    const bool fromPlayer = a_event->oldContainer == player->GetFormID();
                    if (!toPlayer && !fromPlayer) return RE::BSEventNotifyControl::kContinue;

                    auto* vol = SkyrimNetDiaries::BookManager::GetSingleton()->GetBookForFormID(a_event->baseObj);
                    if (!vol) return RE::BSEventNotifyControl::kContinue;  // not one of our diary volumes

                    // The NPC on the other side of the transfer, alive.
                    auto* otherRef = RE::TESForm::LookupByID<RE::TESObjectREFR>(toPlayer ? a_event->oldContainer
                                                                                        : a_event->newContainer);
                    auto* other = otherRef ? otherRef->As<RE::Actor>() : nullptr;
                    if (!other || other == player || other->IsDead()) return RE::BSEventNotifyControl::kContinue;

                    // Only the diary's owner counts: taking or giving someone else's
                    // diary changes nobody's theft state.
                    const std::string otherUuid = UuidOf(other);
                    if (otherUuid.empty() || otherUuid != vol->actorUuid) {
                        SKSE::log::debug("[Physical Diaries] '{}' (vol {}) moved to/from {}, who doesn't own it — ignored",
                                         vol->actorName, vol->volumeNumber, other->GetName());
                        return RE::BSEventNotifyControl::kContinue;
                    }

                    auto* book = RE::TESForm::LookupByID<RE::TESBoundObject>(a_event->baseObj);
                    const std::string bookName = book ? book->GetName() : vol->actorName;
                    const int volumeNumber = vol->volumeNumber;

                    if (fromPlayer) {
                        RecordReturn(otherUuid, other->GetName(), bookName, volumeNumber);
                    } else if (book && PlayerCopyIsStolen(player, book)) {
                        RecordTheft(otherUuid, other->GetName(), bookName, volumeNumber);
                    } else {
                        bool legitimateTrade = false;
                        {
                            std::lock_guard<std::mutex> lock(g_menuMutex);
                            legitimateTrade = g_legitimateTradeActive;
                        }
                        // Not flagged as stolen and not a trade: console or an edge case, ignored.
                        if (legitimateTrade) SendSharedEvent(other, bookName);
                    }
                } catch (const std::exception& e) {
                    SKSE::log::error("[DiaryTheftHandler] container change exception: {}", e.what());
                } catch (...) {
                    SKSE::log::error("[DiaryTheftHandler] container change: unknown exception");
                }
                return RE::BSEventNotifyControl::kContinue;
            }

        private:
            ContainerChangeHandler() = default;
        };

    } // namespace

    void Register() {
        auto* eventSourceHolder = RE::ScriptEventSourceHolder::GetSingleton();
        if (eventSourceHolder) {
            eventSourceHolder->AddEventSink(ContainerChangeHandler::GetSingleton());
            SKSE::log::info("Registered diary theft/return event handler (TESContainerChangedEvent)");
        } else {
            SKSE::log::error("Failed to get ScriptEventSourceHolder for diary theft handler");
        }

        auto* ui = RE::UI::GetSingleton();
        if (ui) {
            ui->AddEventSink<RE::MenuOpenCloseEvent>(MenuSink::GetSingleton());
            SKSE::log::info("Registered dialogue, console, and container menu tracking for legitimate transfers");
        } else {
            SKSE::log::error("Failed to get UI singleton for menu tracking");
        }
    }

    void ClearStolenVolumes(const std::string& actorUuid) {
        auto* diaryDB = SkyrimNetDiaries::DiaryDB::GetSingleton();
        diaryDB->ClearAllStolenVolumes(actorUuid);
        diaryDB->UpdateLastKnownGameTime(actorUuid, GameTimeSeconds());
    }

    void RegisterStolenDecorator() {
        try {
            // SkyrimNet clears decorator registrations on every load, and the Papyrus
            // OnInit that also registers it only runs on a new game.
            auto* vm = RE::BSScript::Internal::VirtualMachine::GetSingleton();
            if (vm) {
                auto* args = RE::MakeFunctionArguments(
                    RE::BSFixedString("snpd_diary_stolen"),
                    RE::BSFixedString("SkyrimNetDiaries_Decorators"),
                    RE::BSFixedString("IsDiaryStolen"));
                RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor> nullCb;
                bool ok = vm->DispatchStaticCall(
                    "SkyrimNetApi", "RegisterDecorator", args, nullCb);
                delete args;
                SKSE::log::info("kPostLoadGame: registered snpd_diary_stolen decorator via Papyrus VM ({})",
                                ok ? "dispatched" : "FAILED");
            } else {
                SKSE::log::warn("kPostLoadGame: Papyrus VM not available — snpd_diary_stolen not registered");
            }
        } catch (const std::exception& e) {
            SKSE::log::error("[DiaryTheftHandler] RegisterStolenDecorator exception: {}", e.what());
        } catch (...) {
            SKSE::log::error("[DiaryTheftHandler] RegisterStolenDecorator: unknown exception");
        }
    }

    void ReconcileAfterLoad() {
        try {
            // Detect backwards time travel and clear stolen volumes if detected
            auto calendar = RE::Calendar::GetSingleton();
            if (calendar) {
                double currentTime = calendar->GetCurrentGameTime() * 86400.0;
                auto* diaryDB = SkyrimNetDiaries::DiaryDB::GetSingleton();
                auto actorTemplates = diaryDB->LoadActorTemplates();

                SKSE::log::debug("[Theft Reconciliation] Checking {} actors for backwards time travel (current time: {:.2f} seconds)",
                               actorTemplates.size(), currentTime);

                int clearedCount = 0;
                for (const auto& [uuid, templateName] : actorTemplates) {
                    double lastKnownTime = diaryDB->GetLastKnownGameTime(uuid);

                    // Detect backwards time travel - clear all stolen volumes if save is from earlier in time
                    if (currentTime < lastKnownTime) {
                        SKSE::log::debug("[Theft Reconciliation] {} went back in time: loaded {:.2f}, last session {:.2f} — clearing stolen volumes",
                                         SkyrimNetDiaries::Database::GetActorName(uuid), currentTime, lastKnownTime);
                        diaryDB->ClearAllStolenVolumes(uuid);
                        clearedCount++;
                    }

                    // Update last known game time for all actors
                    diaryDB->UpdateLastKnownGameTime(uuid, currentTime);
                }

                if (clearedCount > 0) {
                    SKSE::log::info("[Theft Reconciliation] Cleared stolen volumes for {} actors due to backwards time travel", clearedCount);
                } else {
                    SKSE::log::debug("[Theft Reconciliation] No backwards time travel detected - all actors up to date");
                }
            }
        } catch (const std::exception& e) {
            SKSE::log::error("[DiaryTheftHandler] ReconcileAfterLoad exception: {}", e.what());
        } catch (...) {
            SKSE::log::error("[DiaryTheftHandler] ReconcileAfterLoad: unknown exception");
        }
    }
}
