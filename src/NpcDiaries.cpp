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
#include <atomic>
#include <chrono>
#include <cmath>
#include <random>
#include <set>
#include <thread>

namespace SkyrimNetDiaries::NpcDiaries {

    namespace {
        // Sleeping, waiting or fast travel this many game hours before the run hour runs the day's diaries first:
        // they skip time, and SkyrimNet only reads the current day (docs/NPC_DIARIES.md#when).
        constexpr float kEarlyHours = 4.0f;
        constexpr auto kTickInterval = std::chrono::seconds(10);
        constexpr auto kStagger = std::chrono::seconds(3);
        constexpr double kDaySeconds = 86400.0, kWeekSeconds = 604800.0;
        constexpr std::string_view kNamePlaceholder = "{Name}";

        RE::TESFaction* g_dailyFaction = nullptr;
        RE::TESGlobal* g_enabledGlobal = nullptr;
        RE::TESFaction* g_skyrimNetBlacklist = nullptr;
        RE::TESFaction* g_skyrimNetWhitelist = nullptr;

        // Game thread.  The day last run (whole days of GameDaysPassed), the NPCs asked to write daily (by FormID:
        // SkyrimNet's activity data is keyed by name), and a counter Revert bumps so leftover requests stop.
        std::int32_t g_lastRunDay = -1;
        std::set<RE::FormID> g_writers;
        std::uint32_t g_session = 0;

        std::int32_t Today() {
            auto* calendar = RE::Calendar::GetSingleton();
            return calendar ? static_cast<std::int32_t>(std::floor(calendar->GetCurrentGameTime())) : -1;
        }

        float Hour() {
            auto* calendar = RE::Calendar::GetSingleton();
            return calendar ? calendar->GetHour() : 0.0f;
        }

        bool Enabled() { return Config::GetSingleton()->Get(Config::kNpcDiaries) != 0; }

        // SkyrimNet's global AI toggle and its diary switch, read live through its Papyrus config natives (the
        // C++ API can't).  A Papyrus call answers later, so they are cached: at load, MCM changes, every clock tick.
        std::atomic<bool> g_globalAI{ true };
        std::atomic<bool> g_skyrimNetDiaries{ true };

        class StoreBool : public RE::BSScript::IStackCallbackFunctor {
        public:
            explicit StoreBool(std::atomic<bool>& a_target) : target_(a_target) {}
            void operator()(RE::BSScript::Variable a_result) override {
                if (a_result.IsBool()) target_ = a_result.GetBool();
            }
            void SetObject(const RE::BSTSmartPointer<RE::BSScript::Object>&) override {}

        private:
            std::atomic<bool>& target_;
        };

        void ReadSkyrimNetBool(const char* config, const char* path, std::atomic<bool>& target) {
            auto* vm = RE::BSScript::Internal::VirtualMachine::GetSingleton();
            if (!vm) return;
            auto* args = RE::MakeFunctionArguments(RE::BSFixedString(config), RE::BSFixedString(path), true);
            RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor> callback(new StoreBool(target));
            vm->DispatchStaticCall("SkyrimNetApi", "GetConfigBool", args, callback);
        }

        void RefreshSkyrimNetSwitches() {
            ReadSkyrimNetBool("game", "general.globalAIEnabled", g_globalAI);
            ReadSkyrimNetBool("Diary", "enabled", g_skyrimNetDiaries);
        }

        // Part of SkyrimNet's actor filter: its black/whitelist factions and speaking races.  SkyrimNet applies
        // the full filter itself from Beta 26; before that the Papyrus generator didn't.
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
            auto* args = RE::MakeFunctionArguments(std::move(actor));
            RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor> callback;
            if (!vm->DispatchStaticCall("SkyrimNetApi", "GenerateDiaryEntry", args, callback)) {
                SKSE::log::warn("[NpcDiaries] SkyrimNetApi.GenerateDiaryEntry couldn't be called for 0x{:X}", formId);
            }
        }

        // All at once before a time skip (SkyrimNet reads the day when each generation starts), else spaced out.
        // A spaced-out request still waiting is dropped after a load, or with the feature or SkyrimNet's AI off.
        void Dispatch(std::vector<RE::FormID> picks, bool now) {
            if (now) {
                for (const auto formId : picks) RequestEntry(formId);
                return;
            }
            std::thread([picks = std::move(picks), session = g_session]() {
                for (const auto formId : picks) {
                    SKSE::GetTaskInterface()->AddTask([formId, session]() {
                        if (session == g_session && Enabled() && g_globalAI && IsPostLoadSyncReady()) RequestEntry(formId);
                    });
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

        // Every NPC asked to write daily (uncapped: the player's choice), from SNPD's own list, since SkyrimNet's
        // activity rows are keyed by name.  Faction members added another way (console, mods) join from those rows.
        std::vector<Candidate> DailyWriters(const nlohmann::json& list) {
            std::set<RE::FormID> ids;
            for (auto it = g_writers.begin(); it != g_writers.end();) {
                auto* actor = RE::TESForm::LookupByID<RE::Actor>(*it);
                if (actor && g_dailyFaction && !actor->IsInFaction(g_dailyFaction)) {
                    it = g_writers.erase(it);  // removed from the faction some other way
                    continue;
                }
                ids.insert(*it++);
            }
            for (const auto& a : list) {
                const auto formId = static_cast<RE::FormID>(a.value("formId", std::int64_t{ 0 }));
                auto* actor = RE::TESForm::LookupByID<RE::Actor>(formId);
                if (actor && g_dailyFaction && actor->IsInFaction(g_dailyFaction)) ids.insert(formId);
            }
            std::vector<Candidate> writers;
            for (const auto formId : ids) {
                auto* actor = RE::TESForm::LookupByID<RE::Actor>(formId);
                if (MayWrite(actor)) writers.push_back({ formId, actor->GetDisplayFullName(), 0.0 });
            }
            return writers;
        }

        // The day's diaries: every daily writer, then DailyRandom weighted picks from SkyrimNet's activity data.
        void RunPicks(std::int32_t day, bool now) {
            auto list = nlohmann::json::parse(Database::GetActorEngagement(kDaySeconds, kWeekSeconds), nullptr, false);
            if (!list.is_array()) {
                SKSE::log::warn("[NpcDiaries] No activity data from SkyrimNet: only daily writers today");
                list = nlohmann::json::array();
            }
            auto* config = Config::GetSingleton();
            const bool closeBoost = config->Get(Config::kNpcCloseBoost) != 0;
            const auto writers = DailyWriters(list);
            std::vector<Candidate> pool;
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
                if (std::ranges::any_of(writers, [formId](const Candidate& w) { return w.formId == formId; })) continue;
                if (!MayWrite(actor)) {
                    ++filtered;
                    continue;
                }
                // Their newest entry is after their last event: everything is written up, SkyrimNet would decline.
                const auto newest = Database::GetDiaryEntries(formId, 1);
                if (!newest.empty() && newest.back().entry_date >= lastEvent) {
                    ++writtenUp;
                    continue;
                }
                Candidate c{ formId, actor->GetDisplayFullName(),
                             1.0 + 4.0 * a.value("recentMemoryImportanceShort", 0.0) +
                                 0.1 * std::min(a.value("recentEventCountShort", 0), 20) };
                if (closeBoost && IsClose(actor)) c.score *= 2.0;
                pool.push_back(std::move(c));
            }
            SKSE::log::info("[NpcDiaries] Day {}: {} actor(s) from SkyrimNet: {} quiet in the last day, {} not loaded as "
                            "actors, {} filtered out, {} written up, {} candidate(s); {} daily writer(s)",
                            day, list.size(), quiet, unknown, filtered, writtenUp, pool.size(), writers.size());
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

        void Run(std::int32_t day, bool now) {
            g_lastRunDay = day;
            try {
                RunPicks(day, now);
            } catch (const std::exception& e) {
                SKSE::log::error("[NpcDiaries] Day {}: {}", day, e.what());
            } catch (...) {
                SKSE::log::error("[NpcDiaries] Day {}: unknown exception", day);
            }
        }

        // Not before the post-load sync, not twice a day, not before `fromHour`; never while SkyrimNet's AI or
        // diaries are off (every request would fail with an error).  Turned back on, the day still runs.
        bool Due(float fromHour) {
            if (!Enabled() || !IsPostLoadSyncReady()) return false;
            auto* player = RE::PlayerCharacter::GetSingleton();
            if (!player || !player->Is3DLoaded()) return false;  // the main menu, or a load in progress
            const auto today = Today();
            if (today < 0 || today == g_lastRunDay || Hour() < fromHour) return false;
            if (!g_globalAI || !g_skyrimNetDiaries) {
                static std::int32_t loggedDay = -1;
                if (std::exchange(loggedDay, today) != today) {
                    SKSE::log::info("[NpcDiaries] Day {}: waiting, SkyrimNet's {} off", today,
                                    !g_globalAI ? "AI is" : "diaries are");
                }
                return false;
            }
            return true;
        }

        float RunHour() { return static_cast<float>(Config::GetSingleton()->Get(Config::kNpcRunHour)); }

        class MenuSink : public RE::BSTEventSink<RE::MenuOpenCloseEvent> {
        public:
            RE::BSEventNotifyControl ProcessEvent(const RE::MenuOpenCloseEvent* a_event,
                                                  RE::BSTEventSource<RE::MenuOpenCloseEvent>*) override {
                if (!a_event || !a_event->opening) return RE::BSEventNotifyControl::kContinue;
                // Fast travel and carriages: the loading screen opens before the time skip, with the flag set
                // (doors: not set).  Now, not as a task: a later frame can be after the skip.
                auto* player = RE::PlayerCharacter::GetSingleton();
                if (a_event->menuName == RE::LoadingMenu::MENU_NAME && player && player->GetPlayerFlags().fastTraveling &&
                    Due(RunHour() - kEarlyHours)) {
                    SKSE::log::info("[NpcDiaries] Fast travel at {:.2f}: the day's diaries first", Hour());
                    Run(Today(), true);
                }
                if (a_event->menuName == RE::SleepWaitMenu::MENU_NAME) {
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
                        if (!Enabled()) return;
                        RefreshSkyrimNetSwitches();  // answers by the next tick
                        if (Due(RunHour())) Run(Today(), false);
                    });
                }
            }).detach();
        }
    }

    void OnDataLoaded() {
        // By EditorID, like SNPD's other records: a FormID can change in the CK or xEdit, an EditorID can't.
        g_dailyFaction = RE::TESForm::LookupByEditorID<RE::TESFaction>("SNPD_DailyDiaryFaction");
        g_enabledGlobal = RE::TESForm::LookupByEditorID<RE::TESGlobal>("SNPD_NpcDiaries");
        g_skyrimNetBlacklist = RE::TESForm::LookupByEditorID<RE::TESFaction>("SkyrimNet_ActorBlacklistFaction");
        g_skyrimNetWhitelist = RE::TESForm::LookupByEditorID<RE::TESFaction>("SkyrimNet_ActorWhitelistFaction");
        auto* loc = Localization::GetSingleton();
        if (auto* ask = RE::TESForm::LookupByEditorID<RE::TESTopic>("SNPD_DailyDiaryStartTopic")) {
            ask->fullName = loc->GetDailyDiaryAsk();
        }
        if (auto* stop = RE::TESForm::LookupByEditorID<RE::TESTopic>("SNPD_DailyDiaryStopTopic")) {
            stop->fullName = loc->GetDailyDiaryStop();
        }
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
        SKSE::log::info("[NpcDiaries] Ready ({})", Enabled() ? "on" : "off");
    }

    void SyncEnabled() {
        if (g_enabledGlobal) g_enabledGlobal->value = Enabled() ? 1.0f : 0.0f;
        if (Enabled()) RefreshSkyrimNetSwitches();
    }

    // Version 2: the day, then the daily writers (count, FormIDs).  Version 1 had only the day.
    void Save(SKSE::SerializationInterface* a_intfc, std::uint32_t a_type) {
        if (!a_intfc->OpenRecord(a_type, 2)) return;
        a_intfc->WriteRecordData(g_lastRunDay);
        a_intfc->WriteRecordData(static_cast<std::uint32_t>(g_writers.size()));
        for (const auto formId : g_writers) a_intfc->WriteRecordData(formId);
    }

    void Load(SKSE::SerializationInterface* a_intfc, std::uint32_t a_version) {
        std::int32_t day = -1;
        if (!a_intfc->ReadRecordData(day)) return;
        g_lastRunDay = day;
        std::uint32_t count = 0;
        if (a_version < 2 || !a_intfc->ReadRecordData(count)) return;
        for (std::uint32_t i = 0; i < count; ++i) {
            RE::FormID saved = 0, formId = 0;
            if (!a_intfc->ReadRecordData(saved)) return;
            if (a_intfc->ResolveFormID(saved, formId)) g_writers.insert(formId);  // the load order may have changed
        }
    }

    void Revert() {
        g_lastRunDay = -1;
        g_writers.clear();
        ++g_session;
    }

    void DailyDiaryChanged(RE::Actor* a_actor, bool a_daily) {
        if (!a_actor) return;
        if (a_daily) {
            g_writers.insert(a_actor->GetFormID());
        } else {
            g_writers.erase(a_actor->GetFormID());
        }
        auto* loc = Localization::GetSingleton();
        std::string text = a_daily ? loc->GetDailyDiaryOn() : loc->GetDailyDiaryOff();
        if (const auto at = text.find(kNamePlaceholder); at != std::string::npos) {
            text.replace(at, kNamePlaceholder.size(), a_actor->GetDisplayFullName());
        }
        RE::SendHUDMessage::ShowHUDMessage(text.c_str());
        SKSE::log::info("[NpcDiaries] {} {} writing daily", a_actor->GetDisplayFullName(), a_daily ? "started" : "stopped");
    }

}  // namespace SkyrimNetDiaries::NpcDiaries
