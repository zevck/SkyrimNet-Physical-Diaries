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
#include <unordered_set>

namespace SkyrimNetDiaries::DiaryTheftHandler {

    namespace {

        // A container opened during dialogue (console closed) is a willing trade.  Only open/close is tracked,
        // never the speaker (MenuTopicManager::speaker has no VR Address Library id).  See docs/THEFT.md.
        bool g_dialogueIsOpen = false;
        bool g_consoleIsOpen = false;

        // In-memory copy of the actors in stolen_volumes, for the decorator.
        std::mutex g_stolenMutex;
        std::unordered_set<std::string> g_stolenUuids;
        bool g_legitimateTradeActive = false;
        std::mutex g_menuMutex;

        // True if the player's copy of `book` has ownership data: the engine's stolen flag.  An unflagged copy
        // came from a trade, the console or similar.
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
            // stolen_at lets a later load of an earlier save drop the theft (ReconcileAfterLoad).
            SkyrimNetDiaries::DiaryDB::GetSingleton()->AddStolenVolume(
                actorUuid, volumeNumber, SkyrimNetDiaries::CurrentGameTimeSeconds());
            SyncStolenCache();
            SKSE::log::info("[Physical Diaries] Player stole '{}' (vol {}) from {}", bookName, volumeNumber, actorName);
        }

        void RecordReturn(const std::string& actorUuid, const std::string& actorName,
                          const std::string& bookName, int volumeNumber) {
            SkyrimNetDiaries::DiaryDB::GetSingleton()->RemoveStolenVolume(actorUuid, volumeNumber);
            SyncStolenCache();
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

        // Watches diary volumes moving between the player and their owner.  A book is a diary only if
        // BookManager tracks its base FormID; never by name (localized, and shared by same-named NPCs).
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
                    const std::string otherUuid = SkyrimNetDiaries::Database::GetUUIDFromFormID(other->GetFormID());
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
        SkyrimNetDiaries::DiaryDB::GetSingleton()->ClearAllStolenVolumes(actorUuid);
        SyncStolenCache();
    }

    bool IsDiaryStolen(const std::string& actorUuid) {
        std::lock_guard<std::mutex> lock(g_stolenMutex);
        return g_stolenUuids.contains(actorUuid);
    }

    void SyncStolenCache() {
        auto uuids = SkyrimNetDiaries::DiaryDB::GetSingleton()->LoadStolenActorUuids();
        std::lock_guard<std::mutex> lock(g_stolenMutex);
        g_stolenUuids = std::unordered_set<std::string>(std::make_move_iterator(uuids.begin()),
                                                        std::make_move_iterator(uuids.end()));
    }

    void ClearStolenCache() {
        std::lock_guard<std::mutex> lock(g_stolenMutex);
        g_stolenUuids.clear();
    }

    void RegisterStolenDecorator() {
        const bool ok = SkyrimNetDiaries::Database::RegisterDecorator(
            "snpd_diary_stolen", "\"true\" if the player has stolen any of this NPC's diary volumes",
            [](RE::Actor* actor) -> std::string {
                if (!actor) return "";
                try {
                    const auto uuid = SkyrimNetDiaries::Database::GetUUIDFromFormID(actor->GetFormID());
                    return !uuid.empty() && IsDiaryStolen(uuid) ? "true" : "false";
                } catch (...) {
                    return "";
                }
            });
        if (ok) {
            SKSE::log::info("Registered snpd_diary_stolen decorator (native)");
        } else {
            SKSE::log::error("[DiaryTheftHandler] snpd_diary_stolen decorator not registered — NPCs won't notice stolen diaries");
        }
    }

    void ReconcileAfterLoad() {
        try {
            // Thefts recorded after the loaded save's game time happened in a timeline
            // the player has left; earlier ones stand.
            const double now = SkyrimNetDiaries::CurrentGameTimeSeconds();
            if (now <= 0.0) return;
            const int removed = SkyrimNetDiaries::DiaryDB::GetSingleton()->RemoveStolenVolumesAfter(now);
            if (removed > 0) {
                SKSE::log::info("[Theft Reconciliation] Dropped {} theft record(s) made after this save's game time", removed);
            }
            SyncStolenCache();
        } catch (const std::exception& e) {
            SKSE::log::error("[DiaryTheftHandler] ReconcileAfterLoad exception: {}", e.what());
        } catch (...) {
            SKSE::log::error("[DiaryTheftHandler] ReconcileAfterLoad: unknown exception");
        }
    }
} // namespace SkyrimNetDiaries::DiaryTheftHandler
