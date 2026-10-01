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

#include "NpcDiaries.h"
#include "Config.h"
#include "Database.h"
#include "Localization.h"
#include "VolumeSync.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <fstream>
#include <random>
#include <thread>

namespace SkyrimNetDiaries::NpcDiaries {

    namespace {
        constexpr std::string_view kPlugin = "SkyrimNet Physical Diaries.esp";
        constexpr RE::FormID kDailyFactionId = 0x819, kEnabledGlobalId = 0x81A;
        constexpr RE::FormID kAskTopicId = 0x81D, kStopTopicId = 0x820;

        // Sleeping, waiting or opening the map this many game hours before the run hour runs the day's
        // diaries first: those skip time, and SkyrimNet only reads the current day (docs/NPC_DIARIES.md#when).
        constexpr float kEarlyHours = 4.0f;
        constexpr auto kTickInterval = std::chrono::seconds(10);
        constexpr auto kStagger = std::chrono::seconds(3);
        constexpr double kDaySeconds = 86400.0, kWeekSeconds = 604800.0;

        RE::TESFaction* g_dailyFaction = nullptr;
        RE::TESGlobal* g_enabledGlobal = nullptr;
        RE::TESFaction* g_skyrimNetBlacklist = nullptr;
        RE::TESFaction* g_skyrimNetWhitelist = nullptr;
        std::int32_t g_lastRunDay = -1;  // the game day (whole days of GameDaysPassed) last run; game thread

        std::int32_t Today() {
            auto* calendar = RE::Calendar::GetSingleton();
            return calendar ? static_cast<std::int32_t>(std::floor(calendar->GetCurrentGameTime())) : -1;
        }

        float Hour() {
            auto* calendar = RE::Calendar::GetSingleton();
            return calendar ? calendar->GetHour() : 0.0f;
        }

        // SkyrimNet's own diary switch (its config/Diary.yaml); missing or unreadable counts as on.
        bool SkyrimNetDiariesEnabled() {
            std::ifstream file("Data/SKSE/Plugins/SkyrimNet/config/Diary.yaml");
            std::string line;
            while (std::getline(file, line)) {
                if (line.starts_with("enabled:")) return line.find("false") == std::string::npos;
            }
            return true;
        }

        // The parts of SkyrimNet's actor filter that matter here: its blacklist and whitelist factions, and
        // actors that can speak (its race test).  The Papyrus generator doesn't apply the filter itself.
        bool MayWrite(RE::Actor* actor) {
            if (!actor || actor->IsPlayerRef() || actor->IsDead()) return false;
            if (g_skyrimNetBlacklist && actor->IsInFaction(g_skyrimNetBlacklist)) return false;
            if (g_skyrimNetWhitelist && actor->IsInFaction(g_skyrimNetWhitelist)) return true;
            const auto* race = actor->GetRace();
            return race && race->AllowsPCDialogue();
        }

        // A follower or the player's spouse (marriage makes the relationship Lover).
        bool IsClose(RE::Actor* actor) {
            if (actor->IsPlayerTeammate()) return true;
            auto* player = RE::PlayerCharacter::GetSingleton();
            auto* npc = actor->GetActorBase();
            auto* playerBase = player ? player->GetActorBase() : nullptr;
            const auto* relationship = npc && playerBase ? RE::BGSRelationship::GetRelationship(npc, playerBase) : nullptr;
            return relationship && relationship->level == RE::BGSRelationship::RELATIONSHIP_LEVEL::kLover;
        }

        // SkyrimNet writes the entry (async); too few uncovered events and it declines before any LLM call.
        void RequestEntry(RE::FormID formId) {
            auto* actor = RE::TESForm::LookupByID<RE::Actor>(formId);
            auto* vm = RE::BSScript::Internal::VirtualMachine::GetSingleton();
            if (!actor || !vm) return;
            auto* args = RE::MakeFunctionArguments(static_cast<RE::Actor*>(actor));
            RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor> callback;
            if (!vm->DispatchStaticCall("SkyrimNetApi", "GenerateDiaryEntry", args, callback)) {
                SKSE::log::warn("[NpcDiaries] SkyrimNetApi.GenerateDiaryEntry couldn't be called for 0x{:X}", formId);
            }
        }

        // All at once before a time skip (SkyrimNet reads the day when each generation starts), else spaced out.
        void Dispatch(std::vector<RE::FormID> picks, bool now) {
            if (now) {
                for (const auto formId : picks) RequestEntry(formId);
                return;
            }
            std::thread([picks = std::move(picks)]() {
                for (const auto formId : picks) {
                    SKSE::GetTaskInterface()->AddTask([formId]() { RequestEntry(formId); });
                    std::this_thread::sleep_for(kStagger);
                }
            }).detach();
        }

        struct Candidate {
            RE::FormID formId = 0;
            std::string name;
            double score = 0.0;
        };

        // Weighted random, without repeats: busy, important NPCs are likelier, quiet ones still possible.
        std::vector<Candidate> PickWeighted(std::vector<Candidate> pool, int count, std::mt19937& rng) {
            std::vector<Candidate> picked;
            while (count-- > 0 && !pool.empty()) {
                double total = 0.0;
                for (const auto& c : pool) total += c.score;
                double roll = std::uniform_real_distribution<double>(0.0, total)(rng);
                auto it = pool.begin();
                while (it + 1 != pool.end() && (roll -= it->score) > 0.0) ++it;
                picked.push_back(std::move(*it));
                pool.erase(it);
            }
            return picked;
        }

        // The day's diaries: every whitelisted writer (a conscious choice, so uncapped), then DailyRandom weighted picks.
        void Run(std::int32_t day, bool now) {
            g_lastRunDay = day;
            if (!SkyrimNetDiariesEnabled()) {
                SKSE::log::info("[NpcDiaries] SkyrimNet's diaries are off (Diary.yaml): none today");
                return;
            }
            const auto list = nlohmann::json::parse(Database::GetActorEngagement(kDaySeconds, kWeekSeconds), nullptr, false);
            if (!list.is_array()) {
                SKSE::log::warn("[NpcDiaries] No activity data from SkyrimNet: no diaries today");
                return;
            }
            auto* config = Config::GetSingleton();
            const bool closeBoost = config->Get(Config::kNpcCloseBoost) != 0;
            std::vector<Candidate> writers, pool;
            int quiet = 0, unknown = 0, filtered = 0, writtenUp = 0;
            const double gameNow = CurrentGameTimeSeconds();
            for (const auto& a : list) {
                if (!a.is_object()) continue;
                const auto formId = static_cast<RE::FormID>(a.value("formId", std::int64_t{ 0 }));
                const double lastEvent = a.value("lastEventTime", 0.0);
                SKSE::log::debug("[NpcDiaries] {} (0x{:X}): last event {:.0f} (now {:.0f}), {} recent event(s), "
                                 "recent importance {:.2f}", a.value("name", std::string{}), formId, lastEvent, gameNow,
                                 a.value("recentEventCountShort", 0), a.value("recentMemoryImportanceShort", 0.0));
                // Against the game's time, not SkyrimNet's "recent" (measured from its newest event, which after
                // reloading an earlier save is ahead of the game).
                if (lastEvent < gameNow - kDaySeconds) {  // nothing new to write about
                    ++quiet;
                    continue;
                }
                auto* actor = RE::TESForm::LookupByID<RE::Actor>(formId);
                if (!actor) {
                    ++unknown;
                    continue;
                }
                if (!MayWrite(actor)) {
                    ++filtered;
                    continue;
                }
                // Their newest entry is after their last event: everything is written up, SkyrimNet would decline.
                const auto newest = Database::GetDiaryEntries(formId, 1);
                if (!newest.empty() && newest.back().entry_date >= a.value("lastEventTime", 0.0)) {
                    ++writtenUp;
                    continue;
                }
                Candidate c{ formId, actor->GetDisplayFullName(),
                             1.0 + 4.0 * a.value("recentMemoryImportanceShort", 0.0) +
                                 0.1 * std::min(a.value("recentEventCountShort", 0), 20) };
                if (closeBoost && IsClose(actor)) c.score *= 2.0;
                (g_dailyFaction && actor->IsInFaction(g_dailyFaction) ? writers : pool).push_back(std::move(c));
            }
            SKSE::log::info("[NpcDiaries] Day {}: {} actor(s) from SkyrimNet: {} quiet in the last day, {} not loaded as "
                            "actors, {} filtered out, {} written up, {} candidate(s)",
                            day, list.size(), quiet, unknown, filtered, writtenUp, writers.size() + pool.size());
            std::mt19937 rng{ std::random_device{}() };
            const auto random = PickWeighted(std::move(pool), config->Get(Config::kNpcDailyRandom), rng);

            std::vector<RE::FormID> picks;
            const auto add = [&](const std::vector<Candidate>& group, std::string_view why) {
                for (const auto& c : group) {
                    picks.push_back(c.formId);
                    SKSE::log::info("[NpcDiaries] Day {}: {} (0x{:X}, score {:.2f}, {})", day, c.name, c.formId, c.score, why);
                }
            };
            add(writers, "writes daily");
            add(random, "picked");
            SKSE::log::info("[NpcDiaries] Day {}: {} diary request(s), {} writing daily", day, picks.size(), writers.size());
            Dispatch(std::move(picks), now);
        }

        // Not before the post-load sync, not twice a day, not before `fromHour`.
        bool Due(float fromHour) {
            if (Config::GetSingleton()->Get(Config::kNpcDiaries) == 0 || !IsPostLoadSyncReady()) return false;
            auto* player = RE::PlayerCharacter::GetSingleton();
            if (!player || !player->Is3DLoaded()) return false;  // the main menu, or a load in progress
            const auto today = Today();
            return today >= 0 && today != g_lastRunDay && Hour() >= fromHour;
        }

        float RunHour() { return static_cast<float>(Config::GetSingleton()->Get(Config::kNpcRunHour)); }

        class MenuSink : public RE::BSTEventSink<RE::MenuOpenCloseEvent> {
        public:
            RE::BSEventNotifyControl ProcessEvent(const RE::MenuOpenCloseEvent* a_event,
                                                  RE::BSTEventSource<RE::MenuOpenCloseEvent>*) override {
                if (a_event && a_event->opening &&
                    (a_event->menuName == RE::SleepWaitMenu::MENU_NAME || a_event->menuName == RE::MapMenu::MENU_NAME)) {
                    SKSE::GetTaskInterface()->AddTask([]() {
                        if (Due(RunHour() - kEarlyHours)) Run(Today(), true);
                    });
                }
                return RE::BSEventNotifyControl::kContinue;
            }
        };

        void StartClock() {
            std::thread([]() {
                for (;;) {
                    std::this_thread::sleep_for(kTickInterval);
                    SKSE::GetTaskInterface()->AddTask([]() {
                        if (Due(RunHour())) Run(Today(), false);
                    });
                }
            }).detach();
        }
    }

    void OnDataLoaded() {
        auto* data = RE::TESDataHandler::GetSingleton();
        if (!data) return;
        g_dailyFaction = data->LookupForm<RE::TESFaction>(kDailyFactionId, kPlugin);
        g_enabledGlobal = data->LookupForm<RE::TESGlobal>(kEnabledGlobalId, kPlugin);
        g_skyrimNetBlacklist = RE::TESForm::LookupByEditorID<RE::TESFaction>("SkyrimNet_ActorBlacklistFaction");
        g_skyrimNetWhitelist = RE::TESForm::LookupByEditorID<RE::TESFaction>("SkyrimNet_ActorWhitelistFaction");
        auto* loc = Localization::GetSingleton();
        if (auto* ask = data->LookupForm<RE::TESTopic>(kAskTopicId, kPlugin)) ask->fullName = loc->GetDailyDiaryAsk();
        if (auto* stop = data->LookupForm<RE::TESTopic>(kStopTopicId, kPlugin)) stop->fullName = loc->GetDailyDiaryStop();
        if (!g_dailyFaction || !g_enabledGlobal) {
            SKSE::log::error("[NpcDiaries] The ESP's daily-diary records are missing: NPC diaries are off");
            return;
        }
        SyncEnabled();
        if (auto* ui = RE::UI::GetSingleton()) {
            static MenuSink sink;
            ui->AddEventSink<RE::MenuOpenCloseEvent>(&sink);
        }
        StartClock();
        SKSE::log::info("[NpcDiaries] Ready ({})", Config::GetSingleton()->Get(Config::kNpcDiaries) ? "on" : "off");
    }

    void SyncEnabled() {
        if (g_enabledGlobal) g_enabledGlobal->value = Config::GetSingleton()->Get(Config::kNpcDiaries) ? 1.0f : 0.0f;
    }

    void Save(SKSE::SerializationInterface* a_intfc, std::uint32_t a_type) {
        if (a_intfc->OpenRecord(a_type, 1)) a_intfc->WriteRecordData(g_lastRunDay);
    }

    void Load(SKSE::SerializationInterface* a_intfc, std::uint32_t a_version) {
        std::int32_t day = -1;
        if (a_version == 1 && a_intfc->ReadRecordData(day)) g_lastRunDay = day;
    }

    void Revert() { g_lastRunDay = -1; }

    void DailyDiaryChanged(RE::Actor* a_actor, bool a_daily) {
        if (!a_actor) return;
        auto* loc = Localization::GetSingleton();
        std::string text = a_daily ? loc->GetDailyDiaryOn() : loc->GetDailyDiaryOff();
        if (const auto at = text.find("{Name}"); at != std::string::npos) text.replace(at, 6, a_actor->GetDisplayFullName());
        RE::SendHUDMessage::ShowHUDMessage(text.c_str());
        SKSE::log::info("[NpcDiaries] {} {} writing daily", a_actor->GetDisplayFullName(), a_daily ? "started" : "stopped");
    }

}  // namespace SkyrimNetDiaries::NpcDiaries
