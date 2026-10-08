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
#include <functional>
#include <memory>
#include <optional>
#include <thread>

namespace SkyrimNetDiaries::NpcDiaries {

    namespace {
        constexpr auto kTickInterval = std::chrono::seconds(10);
        constexpr auto kStagger = std::chrono::seconds(3);
        // While the Sleep/Wait menu is open: a wait passes the writing hour in well under a second.
        constexpr auto kWaitPoll = std::chrono::milliseconds(100);
        constexpr double kDaySeconds = 86400.0;
        // Enough that barks, a fight or a reloaded save's later events don't push the day's conversation out.
        constexpr int kRecentEvents = 200;
        // SkyrimNet's dialogue spoken by or at the actor (event_constants.h's DIALOGUE_EVENTS less vanilla topics,
        // barks and monologue): a whole point each; anything else a quarter.  docs/NPC_DIARIES.md#who-writes
        constexpr std::string_view kDialogueTypes[] = { "dialogue", "dialogue_player_text", "dialogue_player_stt",
                                                        "dialogue_player_telepathy", "dialogue_npc_telepathy",
                                                        "gamemaster_dialogue" };
        constexpr double kOtherEventWeight = 0.25;
        constexpr std::string_view kNamePlaceholder = "{Name}";

        RE::TESFaction* g_skyrimNetBlacklist = nullptr;
        RE::TESFaction* g_skyrimNetWhitelist = nullptr;

        // Game thread.  The day last run (whole days of GameDaysPassed), this save's daily writers in the order
        // they were added, and a counter Revert bumps so leftover requests stop.
        std::int32_t g_lastRunDay = -1;
        std::vector<RE::FormID> g_writers;
        std::uint32_t g_session = 0;

        void Notify(std::string text, RE::Actor* actor) {
            if (const auto at = text.find(kNamePlaceholder); at != std::string::npos) {
                text.replace(at, kNamePlaceholder.size(), actor->GetDisplayFullName());
            }
            RE::SendHUDMessage::ShowHUDMessage(text.c_str());
        }

        std::int32_t Today() {
            auto* calendar = RE::Calendar::GetSingleton();
            return calendar ? static_cast<std::int32_t>(std::floor(calendar->GetCurrentGameTime())) : -1;
        }

        float Hour() {
            auto* calendar = RE::Calendar::GetSingleton();
            return calendar ? calendar->GetHour() : 0.0f;
        }

        bool Enabled() { return Config::GetSingleton()->Get(Config::kNpcDiaries) != 0; }

        // SkyrimNet's global AI toggle, diary switch and day boundary, read through its Papyrus config natives (the
        // C++ API can't) only when they matter: a run is due, or the MCM opens.  As last read.
        std::atomic<bool> g_globalAI{ true };
        std::atomic<bool> g_skyrimNetDiaries{ true };
        std::atomic<bool> g_dayBoundary{ true };  // shown in the MCM; SkyrimNet applies it

        // The SkyrimNet settings read; `key` names the MCM ones (GetNpcSetting / SetNpcSetting).
        struct SkyrimNetSetting {
            std::string_view key;
            const char* config;
            const char* path;
            std::atomic<bool>* value;
        };
        const SkyrimNetSetting kSkyrimNetSettings[] = {
            { {}, "game", "general.globalAIEnabled", &g_globalAI },
            { "SkyrimNetDiaries", "Diary", "enabled", &g_skyrimNetDiaries },
            { "SkyrimNetDayBoundary", "Diary", "respect_day_boundary", &g_dayBoundary },
        };

        const SkyrimNetSetting* FindSkyrimNetSetting(std::string_view a_key) {
            if (a_key.empty()) return nullptr;
            for (const auto& s : kSkyrimNetSettings) {
                if (s.key == a_key) return &s;
            }
            return nullptr;
        }

        // A Papyrus call answers later, on a VM thread: `then` runs once every read in the batch has answered.
        struct PendingReads {
            std::atomic<int> left{ 0 };
            std::function<void()> then;
            void Answer() {
                if (left.fetch_sub(1) == 1 && then) then();
            }
        };

        class StoreBool : public RE::BSScript::IStackCallbackFunctor {
        public:
            StoreBool(std::atomic<bool>& a_target, std::shared_ptr<PendingReads> a_pending) :
                target_(a_target), pending_(std::move(a_pending)) {}
            void operator()(RE::BSScript::Variable a_result) override {
                if (a_result.IsBool()) target_ = a_result.GetBool();
                pending_->Answer();
            }
            void SetObject(const RE::BSTSmartPointer<RE::BSScript::Object>&) override {}

        private:
            std::atomic<bool>& target_;
            std::shared_ptr<PendingReads> pending_;
        };

        // A call that can't be sent counts as answered (the value stays as last read), so the batch still ends.
        void ReadSkyrimNetBool(const SkyrimNetSetting& setting, const std::shared_ptr<PendingReads>& pending) {
            auto* vm = RE::BSScript::Internal::VirtualMachine::GetSingleton();
            auto* args = RE::MakeFunctionArguments(RE::BSFixedString(setting.config), RE::BSFixedString(setting.path), true);
            RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor> callback(new StoreBool(*setting.value, pending));
            if (!vm || !vm->DispatchStaticCall("SkyrimNetApi", "GetConfigBool", args, callback)) {
                SKSE::log::warn("[NpcDiaries] Couldn't ask SkyrimNet for {} {} (SkyrimNetApi.GetConfigBool)", setting.config,
                                setting.path);
                pending->Answer();
            }
        }

        void ReadSkyrimNetSettings(std::function<void()> then = {}) {
            auto pending = std::make_shared<PendingReads>();
            pending->left = static_cast<int>(std::size(kSkyrimNetSettings));
            pending->then = std::move(then);
            for (const auto& setting : kSkyrimNetSettings) ReadSkyrimNetBool(setting, pending);
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

        // Has an entry dated this game day (the player's diary hotkey, SkyrimNet's own generation, a run before a
        // reload): not asked again, whatever events are left.
        bool WroteOn(RE::FormID formId, std::int32_t day) {
            const double start = day * kDaySeconds;
            return !Database::GetDiaryEntries(formId, 1, start, start + kDaySeconds - 0.001).empty();
        }

        // Every daily writer of this save (uncapped: the player's choice), unless switched off in the MCM.
        std::vector<Candidate> DailyWriters() {
            std::vector<Candidate> writers;
            if (Config::GetSingleton()->Get(Config::kNpcDailyWriters) == 0) return writers;
            for (const auto formId : g_writers) {
                auto* actor = RE::TESForm::LookupByID<RE::Actor>(formId);
                if (MayWrite(actor)) writers.push_back({ formId, actor->GetDisplayFullName(), 0.0 });
            }
            return writers;
        }

        // A listed actor's numbers, from SkyrimNet queries alone (no game state), so they can be gathered off the
        // game thread.
        struct Gathered {
            enum class State { Active, Quiet, WroteToday, WrittenUp } state = State::Active;
            RE::FormID formId = 0;
            std::string name;
            double lastEvent = 0.0;  // game seconds; 0: none
            double activity = 0.0;   // SkyrimNet dialogue spoken by or at them 1, other events kOtherEventWeight
        };

        struct Gathering {
            bool listed = true;  // false: SkyrimNet gave no list of actors
            std::vector<Gathered> actors;
            std::vector<RE::FormID> writersWrote;  // daily writers with an entry today
            double newestEvent = 0.0;
            long long ms = 0;
        };

        // The actor's own events: SkyrimNet's engagement data reads every game time as 0 (its database layer hands
        // `game_time` back as a time_point those queries don't expect), from windowStart on.  docs/NPC_DIARIES.md
        Gathered Assess(RE::FormID formId, std::string name, std::int32_t day, double gameNow, double windowStart) {
            Gathered g{ .formId = formId, .name = std::move(name) };
            const std::string uuidText = Database::GetUUIDFromFormID(formId);
            const std::uint64_t uuid = uuidText.empty() ? 0 : std::stoull(uuidText);
            std::vector<std::pair<double, double>> recent;  // time, weight
            const auto events = nlohmann::json::parse(Database::GetRecentEvents(formId, kRecentEvents), nullptr, false);
            for (const auto& e : events.is_array() ? events : nlohmann::json::array()) {
                const double t = e.is_object() ? e.value("gameTime", 0.0) : 0.0;
                if (t <= 0.0 || t > gameNow + 1.0) continue;  // none, or a later timeline's (an earlier save reloaded)
                g.lastEvent = std::max(g.lastEvent, t);
                if (t < windowStart) continue;
                const auto type = e.value("type", std::string{});
                const bool theirs = uuid != 0 && (e.value("originatingActor", std::uint64_t{ 0 }) == uuid ||
                                                  e.value("targetActor", std::uint64_t{ 0 }) == uuid);
                const bool dialogue = std::ranges::find(kDialogueTypes, type) != std::end(kDialogueTypes);
                recent.emplace_back(t, dialogue && theirs ? 1.0 : kOtherEventWeight);
            }
            if (g.lastEvent < windowStart) {
                g.state = Gathered::State::Quiet;  // nothing new to write about
                return g;
            }
            if (WroteOn(formId, day)) {
                g.state = Gathered::State::WroteToday;
                return g;
            }
            // SkyrimNet writes only what their newest entry doesn't cover: nothing left, and it would decline.
            const auto lastEntry = Database::GetDiaryEntries(formId, 1);
            const double covered = lastEntry.empty() ? 0.0 : lastEntry.back().entry_date;
            if (covered >= g.lastEvent) {
                g.state = Gathered::State::WrittenUp;
                return g;
            }
            for (const auto& [time, weight] : recent) {
                if (time > covered) g.activity += weight;
            }
            return g;
        }

        // Every listed actor, and which daily writers wrote today.  SkyrimNet queries only: any thread.
        Gathering Gather(std::int32_t day, double gameNow, bool dayBoundary, const std::vector<RE::FormID>& writers) {
            const auto started = std::chrono::steady_clock::now();
            Gathering g;
            auto list = nlohmann::json::parse(Database::GetActorEngagement(), nullptr, false);
            if (!list.is_array()) {
                g.listed = false;
                list = nlohmann::json::array();
            }
            // What SkyrimNet writes about: since midnight with its day boundary on, else the last game day.
            const double windowStart = dayBoundary ? std::floor(gameNow / kDaySeconds) * kDaySeconds : gameNow - kDaySeconds;
            for (const auto& a : list) {
                if (!a.is_object()) continue;
                const auto formId = static_cast<RE::FormID>(a.value("formId", std::int64_t{ 0 }));
                if (formId == 0 || std::ranges::find(writers, formId) != writers.end()) continue;
                auto assessed = Assess(formId, a.value("name", std::string{}), day, gameNow, windowStart);
                g.newestEvent = std::max(g.newestEvent, assessed.lastEvent);
                SKSE::log::debug("[NpcDiaries] {} (0x{:X}): last event {:.0f} (now {:.0f}, from {:.0f}), activity {:.2f}",
                                 assessed.name, formId, assessed.lastEvent, gameNow, windowStart, assessed.activity);
                g.actors.push_back(std::move(assessed));
            }
            for (const auto formId : writers) {
                if (WroteOn(formId, day)) g.writersWrote.push_back(formId);
            }
            g.ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();
            return g;
        }

        // Game thread: the game-state filters, the picks, the requests.
        void Pick(std::int32_t day, bool now, double gameNow, const Gathering& g, std::vector<Candidate> writers) {
            if (!g.listed) SKSE::log::warn("[NpcDiaries] No activity data from SkyrimNet: only daily writers today");
            const bool closeBoost = Config::GetSingleton()->Get(Config::kNpcCloseBoost) != 0;
            std::vector<Candidate> pool;
            int quiet = 0, unknown = 0, filtered = 0, wroteToday = 0, writtenUp = 0;
            for (const auto& a : g.actors) {
                auto* actor = RE::TESForm::LookupByID<RE::Actor>(a.formId);
                if (!actor) {
                    ++unknown;
                } else if (a.state == Gathered::State::Quiet) {
                    ++quiet;
                } else if (!MayWrite(actor)) {
                    ++filtered;
                } else if (a.state == Gathered::State::WroteToday) {
                    ++wroteToday;
                } else if (a.state == Gathered::State::WrittenUp) {
                    ++writtenUp;
                } else {
                    // Squared: the busiest NPCs write most nights, the barely active rarely.
                    Candidate c{ a.formId, actor->GetDisplayFullName(), a.activity * a.activity };
                    if (closeBoost && IsClose(actor)) c.score *= 2.0;
                    if (c.score > 0.0) pool.push_back(std::move(c));
                }
            }
            const auto writersWrote = std::erase_if(writers, [&g](const Candidate& w) {
                return std::ranges::find(g.writersWrote, w.formId) != g.writersWrote.end();
            });
            SKSE::log::info("[NpcDiaries] Day {}: newest event {:.0f}, now {:.0f} ({} ms asking SkyrimNet)", day,
                            g.newestEvent, gameNow, g.ms);
            SKSE::log::info("[NpcDiaries] Day {}: {} actor(s) from SkyrimNet: {} quiet, {} not loaded as "
                            "actors, {} filtered out, {} wrote today, {} written up, {} candidate(s); {} daily writer(s), "
                            "{} more wrote today", day, g.actors.size(), quiet, unknown, filtered, wroteToday, writtenUp,
                            pool.size(), writers.size(), writersWrote);
            std::mt19937 rng{ std::random_device{}() };
            const auto random = PickWeighted(std::move(pool), Config::GetSingleton()->Get(Config::kNpcDailyRandom), rng);

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

        // The day's diaries: every daily writer, then DailyRandom weighted picks.  During play the SkyrimNet queries
        // (a few per listed actor) run off the game thread; before a time skip inline, as its requests can't wait.
        void RunPicks(std::int32_t day, bool now) {
            auto writers = DailyWriters();
            std::vector<RE::FormID> writerIds;
            for (const auto& w : writers) writerIds.push_back(w.formId);
            const double gameNow = CurrentGameTimeSeconds();
            const bool dayBoundary = g_dayBoundary;
            if (now) {
                Pick(day, true, gameNow, Gather(day, gameNow, dayBoundary, writerIds), std::move(writers));
                return;
            }
            auto shared = std::make_shared<std::vector<Candidate>>(std::move(writers));
            std::thread([day, gameNow, dayBoundary, writerIds, shared, session = g_session]() {
                auto g = std::make_shared<Gathering>();
                try {
                    *g = Gather(day, gameNow, dayBoundary, writerIds);
                } catch (const std::exception& e) {
                    SKSE::log::error("[NpcDiaries] Day {}: {}", day, e.what());
                    return;
                }
                SKSE::GetTaskInterface()->AddTask([day, gameNow, g, shared, session]() {
                    if (session != g_session) return;  // a load since
                    try {
                        Pick(day, false, gameNow, *g, std::move(*shared));
                    } catch (const std::exception& e) {
                        SKSE::log::error("[NpcDiaries] Day {}: {}", day, e.what());
                    }
                });
            }).detach();
        }

        // A time skip's days already covered by its one run (see SkipEnds); 0 or below: none pending.
        std::int32_t g_skipThrough = -1;

        void Run(std::int32_t day, bool now) {
            // Never backwards (a skip's start day can run after a later one); a skip's run covers its days.
            g_lastRunDay = std::max({ g_lastRunDay, day, std::exchange(g_skipThrough, -1) });
            try {
                RunPicks(day, now);
            } catch (const std::exception& e) {
                SKSE::log::error("[NpcDiaries] Day {}: {}", day, e.what());
            } catch (...) {
                SKSE::log::error("[NpcDiaries] Day {}: unknown exception", day);
            }
        }

        float RunHour() { return static_cast<float>(Config::GetSingleton()->Get(Config::kNpcRunHour)); }

        // On, loaded and synced: a run may be due.
        bool Ready() {
            if (!Enabled() || !IsPostLoadSyncReady()) return false;
            auto* player = RE::PlayerCharacter::GetSingleton();
            return player && player->Is3DLoaded();  // not the main menu, or a load in progress
        }

        // Asks SkyrimNet first: with its AI or diaries off the player wants none, so the day is skipped (marked as
        // run; every request would fail with an error anyway).  Game thread.
        bool g_checking = false;

        void RunWhenAllowed(std::int32_t day, bool now) {
            if (g_checking) return;
            g_checking = true;
            ReadSkyrimNetSettings([day, now, session = g_session]() {
                SKSE::GetTaskInterface()->AddTask([day, now, session]() {
                    if (session != g_session) return;  // a load in between (Revert reset the flag)
                    g_checking = false;
                    if (day <= g_lastRunDay || !Ready()) return;
                    if (!g_globalAI || !g_skyrimNetDiaries) {
                        g_lastRunDay = std::max({ g_lastRunDay, day, std::exchange(g_skipThrough, -1) });
                        SKSE::log::info("[NpcDiaries] Day {}: skipped, SkyrimNet's {} off", day,
                                        !g_globalAI ? "AI is" : "diaries are");
                        return;
                    }
                    Run(day, now);
                });
            });
        }

        // Today has reached the writing hour and hasn't run.
        bool Due() {
            const auto today = Today();
            return today >= 0 && today > g_lastRunDay && Hour() >= RunHour() && Ready();
        }

        // A time skip (sleep, wait, fast travel, a carriage), however many days long, gives one run: during a wait
        // when the clock passes the writing hour, else when it ends.  Game thread.  docs/NPC_DIARIES.md#when
        std::int32_t g_skipStartDay = -1;
        bool g_skipActive = false;  // the clock's tick waits until it ends
        bool g_skipRan = false;
        std::atomic<bool> g_waiting{ false };

        // Menu events: the day is read at once (a fast travel's clock jumps during the load), the rest on the game
        // thread with the other run state.
        void SkipStarts() {
            const auto day = Today();
            SKSE::GetTaskInterface()->AddTask([day]() {
                g_skipStartDay = day;
                g_skipActive = true;
                g_skipRan = false;
            });
        }

        // Every day the skip passed the writing hour on counts as run: through today if it's past the hour now.
        void SkipEnds() {
            const auto start = std::exchange(g_skipStartDay, -1);
            const bool active = std::exchange(g_skipActive, false);
            const bool ran = std::exchange(g_skipRan, false);
            if (!active || start < 0 || !Ready()) return;
            const auto through = Hour() >= RunHour() ? Today() : Today() - 1;
            if (through < start || through <= g_lastRunDay) return;  // no writing hour passed, or already run
            if (ran) {
                // The wait's run may still be asking SkyrimNet for its settings: let it mark the days.
                if (g_checking) {
                    g_skipThrough = through;
                } else {
                    g_lastRunDay = through;
                }
                return;
            }
            SKSE::log::info("[NpcDiaries] Days {}-{}: a time skip passed the writing hour", start, through);
            RunWhenAllowed(through, true);
        }

        // Every kWaitPoll while sleeping or waiting: run the moment the clock passes the writing hour.
        void WatchWait() {
            g_waiting = true;
            std::thread([]() {
                while (g_waiting) {
                    SKSE::GetTaskInterface()->AddTask([]() {
                        if (g_waiting && g_skipActive && !g_skipRan && Due()) {
                            g_skipRan = true;
                            RunWhenAllowed(Today(), true);
                        }
                    });
                    std::this_thread::sleep_for(kWaitPoll);
                }
            }).detach();
        }

        class MenuSink : public RE::BSTEventSink<RE::MenuOpenCloseEvent> {
        public:
            RE::BSEventNotifyControl ProcessEvent(const RE::MenuOpenCloseEvent* a_event,
                                                  RE::BSTEventSource<RE::MenuOpenCloseEvent>*) override {
                if (!a_event) return RE::BSEventNotifyControl::kContinue;
                if (a_event->menuName == RE::SleepWaitMenu::MENU_NAME) {
                    if (a_event->opening) {
                        SkipStarts();
                        WatchWait();
                    } else {
                        g_waiting = false;
                        SKSE::GetTaskInterface()->AddTask([]() { SkipEnds(); });
                    }
                } else if (a_event->menuName == RE::LoadingMenu::MENU_NAME) {
                    // Fast travel and carriages skip time during the load: the flag is set when the screen
                    // opens (not for doors), and the game time is still the departure's.
                    auto* player = RE::PlayerCharacter::GetSingleton();
                    if (a_event->opening && player && player->GetPlayerFlags().fastTraveling) {
                        SkipStarts();
                    } else if (!a_event->opening) {
                        SKSE::GetTaskInterface()->AddTask([]() { SkipEnds(); });
                    }
                }
                return RE::BSEventNotifyControl::kContinue;
            }
        };

        void StartClock() {
            std::thread([]() {
                for (;;) {
                    std::this_thread::sleep_for(kTickInterval);
                    SKSE::GetTaskInterface()->AddTask([]() {
                        if (!g_skipActive && Due()) RunWhenAllowed(Today(), false);
                    });
                }
            }).detach();
        }
    }

    void OnDataLoaded() {
        g_skyrimNetBlacklist = RE::TESForm::LookupByEditorID<RE::TESFaction>("SkyrimNet_ActorBlacklistFaction");
        g_skyrimNetWhitelist = RE::TESForm::LookupByEditorID<RE::TESFaction>("SkyrimNet_ActorWhitelistFaction");
        if (auto* ui = RE::UI::GetSingleton()) {
            static MenuSink sink;
            ui->AddEventSink<RE::MenuOpenCloseEvent>(&sink);
        }
        StartClock();
        SKSE::log::info("[NpcDiaries] Ready ({})", Enabled() ? "on" : "off");
    }

    // Version 4: the day, then the daily writers (count, FormIDs).  Version 2 had the same layout (its writers came
    // from the old dialogue); versions 1 and 3 only the day.
    void Save(SKSE::SerializationInterface* a_intfc, std::uint32_t a_type) {
        if (!a_intfc->OpenRecord(a_type, 4)) return;
        a_intfc->WriteRecordData(g_lastRunDay);
        a_intfc->WriteRecordData(static_cast<std::uint32_t>(g_writers.size()));
        for (const auto formId : g_writers) a_intfc->WriteRecordData(formId);
    }

    void Load(SKSE::SerializationInterface* a_intfc, std::uint32_t a_version) {
        std::int32_t day = -1;
        if (!a_intfc->ReadRecordData(day)) return;
        g_lastRunDay = day;
        std::uint32_t count = 0;
        if ((a_version != 2 && a_version < 4) || !a_intfc->ReadRecordData(count)) return;
        for (std::uint32_t i = 0; i < count; ++i) {
            RE::FormID saved = 0, formId = 0;
            if (!a_intfc->ReadRecordData(saved)) return;
            if (a_intfc->ResolveFormID(saved, formId) && std::ranges::find(g_writers, formId) == g_writers.end()) {
                g_writers.push_back(formId);  // the load order may have changed; a plugin removed drops it
            }
        }
    }

    void Revert() {
        g_lastRunDay = -1;
        g_writers.clear();
        g_skipStartDay = -1;
        g_skipActive = false;
        g_skipRan = false;
        g_skipThrough = -1;
        g_waiting = false;
        g_checking = false;
        ++g_session;
    }

    void RefreshSkyrimNetSettings() { ReadSkyrimNetSettings(); }

    std::optional<bool> GetSkyrimNetDiarySetting(std::string_view a_key) {
        const auto* setting = FindSkyrimNetSetting(a_key);
        return setting ? std::optional<bool>(setting->value->load()) : std::nullopt;
    }

    bool SetSkyrimNetDiarySetting(std::string_view a_key, bool a_value) {
        const auto* setting = FindSkyrimNetSetting(a_key);
        auto* vm = RE::BSScript::Internal::VirtualMachine::GetSingleton();
        if (!setting || !vm) return false;
        const auto patch = nlohmann::json{ { setting->path, a_value } }.dump();
        auto* args = RE::MakeFunctionArguments(RE::BSFixedString(setting->config), RE::BSFixedString(patch.c_str()));
        RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor> callback;
        if (!vm->DispatchStaticCall("SkyrimNetApi", "PatchConfig", args, callback)) {
            SKSE::log::warn("[NpcDiaries] Couldn't change SkyrimNet's {} config (SkyrimNetApi.PatchConfig)", setting->config);
            return false;
        }
        *setting->value = a_value;
        SKSE::log::info("[NpcDiaries] SkyrimNet's {} config: {}", setting->config, patch);
        return true;
    }

    bool IsDailyWriter(const RE::Actor* a_actor) {
        return a_actor && std::ranges::find(g_writers, a_actor->GetFormID()) != g_writers.end();
    }

    int AddDailyWriter(RE::Actor* a_actor) {
        if (!a_actor || a_actor->IsPlayerRef()) return -1;
        if (IsDailyWriter(a_actor)) return 0;
        g_writers.push_back(a_actor->GetFormID());
        Notify(Localization::GetSingleton()->GetDailyDiaryOn(), a_actor);
        SKSE::log::info("[NpcDiaries] {} (0x{:08X}) writes daily", a_actor->GetDisplayFullName(), a_actor->GetFormID());
        return 1;
    }

    std::vector<std::string> DailyWriterNames() {
        std::vector<std::string> names;
        for (const auto formId : g_writers) {
            auto* actor = RE::TESForm::LookupByID<RE::Actor>(formId);
            names.push_back(actor ? actor->GetDisplayFullName() : std::format("0x{:08X}", formId));
        }
        return names;
    }

    void RemoveDailyWriter(int a_index) {
        if (a_index < 0 || a_index >= static_cast<int>(g_writers.size())) return;
        const auto formId = g_writers[a_index];
        g_writers.erase(g_writers.begin() + a_index);
        if (auto* actor = RE::TESForm::LookupByID<RE::Actor>(formId)) {
            Notify(Localization::GetSingleton()->GetDailyDiaryOff(), actor);
        }
        SKSE::log::info("[NpcDiaries] 0x{:08X} no longer writes daily", formId);
    }

}  // namespace SkyrimNetDiaries::NpcDiaries
