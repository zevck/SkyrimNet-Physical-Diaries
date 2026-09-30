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
#include "BookEditor.h"
#include "BookManager.h"
#include "BookTextHook.h"
#include "Config.h"
#include "Database.h"
#include "DiaryDB.h"
#include "DiaryTheftHandler.h"
#include "DynamicForms.h"
#include "InterPluginAPI.h"
#include "Localization.h"
#include "PapyrusAPI.h"
#include "SaveFolder.h"
#include "Serialization.h"
#include "TimelineGate.h"
#include "VolumeSync.h"
#include "WritingMode.h"
#include <spdlog/sinks/basic_file_sink.h>
#include <atomic>
#include <chrono>
#include <thread>
#include <unordered_set>

namespace {

    // Queues a one-button message box with the game's own (localized) OK label.
    void ShowWarning(std::string text) {
        SKSE::GetTaskInterface()->AddTask([text = std::move(text)]() {
            auto* msgBoxData = RE::UIMessageDataFactory::Create<RE::MessageBoxData>();
            if (!msgBoxData) return;
            const char* ok = "OK";
            if (auto* settings = RE::GameSettingCollection::GetSingleton()) {
                if (auto* setting = settings->GetSetting("sOk"); setting && setting->GetString() && *setting->GetString()) {
                    ok = setting->GetString();
                }
            }
            msgBoxData->bodyText = text.c_str();
            msgBoxData->buttonText.push_back(ok);
            msgBoxData->cancelButtonIndex = 0;
            RE::MessageBoxMenu::QueueMessage(msgBoxData);
        });
    }

    // Bumped whenever a session ends (a load or a new game) so a post-load setup
    // still waiting from an earlier load gives up instead of running against the new one.
    std::atomic<std::uint32_t> g_loadGeneration{ 0 };

    // Ends the current session before a load or a new game.  Nothing from it may
    // leak into the next: the actor cache, and the previous save's DiaryDB (reopened by the post-load sync, or by
    // SaveCallback if a save happens first).  A new game gets no kPreLoadGame, so
    // without this it would keep writing into the previous character's DiaryDB.
    void EndSession() {
        ++g_loadGeneration;
        SkyrimNetDiaries::TimelineGate::Reset();
        SkyrimNetDiaries::BookEditor::Reset();
        SkyrimNetDiaries::BookManager::ClearActorCache();
        SkyrimNetDiaries::DiaryDB::GetSingleton()->Close();
        SkyrimNetDiaries::SaveFolder::Clear();
        SkyrimNetDiaries::DiaryTheftHandler::ClearStolenCache();
        // Drops diary events still deferred from the previous session.
        SkyrimNetDiaries::SetPostLoadSyncReady(false);
    }

    // SkyrimNet readiness is polled every 100 ms for up to a minute.
    constexpr auto kSetupPollInterval = std::chrono::milliseconds(100);
    constexpr auto kMemorySystemTimeout = std::chrono::seconds(60);

    void OnMessage(SKSE::MessagingInterface::Message* msg)
    {
        if (!msg) {
            return;
        }
        if (SkyrimNetDiaries::InterPluginAPI::HandleMessage(msg)) {
            return;
        }

        switch (msg->type) {
        case SKSE::MessagingInterface::kDataLoaded: {
            try {
                // The stolen-diary decorator for SkyrimNet's prompts (native, registered once).
                if (SkyrimNetDiaries::Database::InitializeAPI()) {
                    SkyrimNetDiaries::DiaryTheftHandler::RegisterStolenDecorator();
                }

                // Verify the diary template books resolve.  If they don't, every
                // diary creation silently fails with "Template book not found" spam.
                // Common causes: ESP not actually enabled, an EditorID-exposure
                // plugin (e.g. po3_Tweaks / Native EditorID Fix) missing or the wrong
                // runtime build, or a tool stripped the template records.
                {
                    std::vector<const char*> templates(std::begin(SkyrimNetDiaries::kJournalTemplates),
                                                       std::end(SkyrimNetDiaries::kJournalTemplates));
                    templates.push_back(SkyrimNetDiaries::kNightingaleTemplate);
                    std::vector<const char*> missing;
                    for (const char* id : templates) {
                        if (!RE::TESForm::LookupByEditorID<RE::TESObjectBOOK>(id)) missing.push_back(id);
                    }
                    if (!missing.empty()) {
                        SKSE::log::error("================================================================");
                        SKSE::log::error("[Physical Diaries] DIARY TEMPLATE BOOKS NOT FOUND — diaries cannot be created!");
                        for (const char* id : missing) SKSE::log::error("  {}: MISSING", id);
                        SKSE::log::error("  Possible causes:");
                        SKSE::log::error("   1. 'SkyrimNet Physical Diaries.esp' is not enabled in the load order");
                        SKSE::log::error("   2. An EditorID-exposure plugin (po3_Tweaks / Native EditorID Fix) is");
                        SKSE::log::error("      missing or is the wrong runtime build (SE vs AE vs VR)");
                        SKSE::log::error("   3. The ESP was modified by a tool that stripped the template records");
                        SKSE::log::error("================================================================");
                        ShowWarning(SkyrimNetDiaries::Localization::GetSingleton()->GetTemplatesMissingText());
                    } else {
                        SKSE::log::info("[Physical Diaries] Diary template books verified (all {} resolved)", templates.size());
                    }
                }

                // Needs the forms loaded: find SkyrimNet's keep/clear prompt text.
                SkyrimNetDiaries::TimelineGate::OnDataLoaded();

                // The player's diary editor, only with SNPD's book.swf installed (docs/EDITING.md).
                SkyrimNetDiaries::WritingMode::Detect();
                if (SkyrimNetDiaries::WritingMode::IsOn()) SkyrimNetDiaries::BookEditor::Register();

                // Now that GMSTs are loaded, read localized month/day names
                SkyrimNetDiaries::Localization::GetSingleton()->ReadGMSTs();
            } catch (const std::exception& e) {
                SKSE::log::error("Exception in kDataLoaded: {}", e.what());
            } catch (...) {
                SKSE::log::error("Unknown exception in kDataLoaded");
            }
            break;
        }
        case SKSE::MessagingInterface::kPreLoadGame: {
            // Diary events wait for this load's post-load sync (see SetPostLoadSyncReady).
            EndSession();
            break;
        }

        case SKSE::MessagingInterface::kNewGame: {
            EndSession();
            // No kPostLoadGame follows a new game, so nothing else would release diary events.
            SkyrimNetDiaries::SetPostLoadSyncReady(true);
            break;
        }

        case SKSE::MessagingInterface::kPostLoadGame: {
            // First, and independent of SkyrimNet: world copies of our books in the
            // loaded cells were built before the load callback filled the books in.
            DynamicForms::RebuildLoadedWorldCopies();

            if (!SkyrimNetDiaries::Database::InitializeAPI()) {
                SKSE::log::warn("Failed to initialize API (SkyrimNet may not be loaded yet)");
                SkyrimNetDiaries::SetPostLoadSyncReady(true);
                break;
            }
            SKSE::log::info("✓ SkyrimNet API ready");

            // The post-load setup waits for two things, polled every 100 ms:
            //   1. SkyrimNet's database (IsMemorySystemReady), for up to a minute.
            //   2. SkyrimNet's timeline decision (TimelineGate), with no limit while its
            //      keep/clear prompt is on screen.  Syncing earlier builds books from
            //      "future" entries that a Clear then deletes.
            // Everything that reads SkyrimNet's data, including opening DiaryDB, runs
            // only after both.
            const auto generation = g_loadGeneration.load();
            const auto start = std::chrono::steady_clock::now();
            auto runSetup = std::make_shared<std::function<void()>>();
            *runSetup = [runSetup, generation, start]() {
                if (generation != g_loadGeneration.load()) {
                    return;  // another load started; its own setup takes over
                }
                const auto retry = [runSetup]() {
                    std::thread([runSetup]() {
                        std::this_thread::sleep_for(kSetupPollInterval);
                        SKSE::GetTaskInterface()->AddTask([runSetup]() { (*runSetup)(); });
                    }).detach();
                };

                if (!SkyrimNetDiaries::Database::IsMemorySystemReady()) {
                    if (std::chrono::steady_clock::now() - start > kMemorySystemTimeout) {
                        SKSE::log::error("kPostLoadGame: SkyrimNet memory system not ready after {}s — diary books "
                                         "are paused until the next load", kMemorySystemTimeout.count());
                        SkyrimNetDiaries::PauseDiaryBooks();
                        SkyrimNetDiaries::SetPostLoadSyncReady(true);
                        return;
                    }
                    retry();
                    return;
                }
                if (!SkyrimNetDiaries::TimelineGate::IsSettled()) {
                    retry();
                    return;
                }

                const auto waited = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
                SKSE::log::info("kPostLoadGame: SkyrimNet ready, starting post-load sync (waited {:.1f}s)", waited);
                // A failure before this save's volumes are loaded pauses diary books, as
                // the other load failures do: with books_ incomplete, events would
                // create duplicate volumes.
                bool volumesLoaded = false;
                const auto pauseIfIncomplete = [&volumesLoaded]() {
                    if (!volumesLoaded) {
                        SKSE::log::error("kPostLoadGame: this save's volumes weren't loaded — diary books are paused until the next load");
                        SkyrimNetDiaries::PauseDiaryBooks();
                    }
                };
                try {
                    // Detect the save folder from SkyrimNet.log and open this save's DiaryDB.
                    SkyrimNetDiaries::SaveFolder::DetectFromLog();
                    auto* db = SkyrimNetDiaries::DiaryDB::GetSingleton();
                    if (!SkyrimNetDiaries::SaveFolder::Get().empty()) {
                        db->Open(SkyrimNetDiaries::SaveFolder::Get());
                    }
                    // Without this save's volumes every NPC would look new and get a
                    // second set of books, so create nothing this session instead.
                    if (!db->IsOpen()) {
                        SKSE::log::error("kPostLoadGame: couldn't find or open this save's SkyrimNet folder — "
                                         "diary books are paused until the next load");
                        SkyrimNetDiaries::PauseDiaryBooks();
                        SkyrimNetDiaries::SetPostLoadSyncReady(true);
                        return;
                    }

                    auto invalidActors = SkyrimNetDiaries::BookManager::GetSingleton()->LoadFromDB();
                    volumesLoaded = true;

                    // Match SkyrimNet's history: volumes reaching past this save lose the
                    // entries a Clear deleted.
                    SkyrimNetDiaries::ReconcileWithTimeline();

                    // Drop theft records made after this save's game time (they belong to a
                    // timeline the player has left).
                    SkyrimNetDiaries::DiaryTheftHandler::ReconcileAfterLoad();

                    // Diary events are handled again from here on; entries that arrived while
                    // we waited are picked up by the recovery and catch-up scans below.
                    SkyrimNetDiaries::SetPostLoadSyncReady(true);

                    // Build skip set: deduplicated UUIDs being immediately recovered.
                    std::unordered_set<std::string> skipUuids;
                    if (!invalidActors.empty()) {
                        SKSE::log::info("kPostLoadGame: {} actor(s) lost volumes with this save — queuing immediate recreation", invalidActors.size());
                        std::unordered_set<std::string> seen;
                        for (const auto& uuid : invalidActors) {
                            if (!seen.insert(uuid).second) continue;
                            uint32_t formId = SkyrimNetDiaries::Database::GetFormIDForUUID(uuid);
                            if (formId == 0) {
                                SKSE::log::warn("kPostLoadGame: could not resolve FormID for UUID {} — catch-up scan will handle it", uuid);
                                continue;
                            }
                            skipUuids.insert(uuid);
                            RE::FormID fid = static_cast<RE::FormID>(formId);
                            SKSE::GetTaskInterface()->AddTask([fid]() {
                                SkyrimNetDiaries::UpdateDiaryForActorInternal(fid);
                            });
                        }
                    }

                    // Pass skip set so these two paths don't re-queue the same actors.
                    SkyrimNetDiaries::QueueNewEntryRecovery(skipUuids);
                    SkyrimNetDiaries::QueueBatchCatchUpScan(std::move(skipUuids));
                } catch (const std::exception& e) {
                    SKSE::log::error("Exception in the post-load sync: {}", e.what());
                    pauseIfIncomplete();
                } catch (...) {
                    SKSE::log::error("Unknown exception in the post-load sync");
                    pauseIfIncomplete();
                }
                // Never leave diary events waiting for a sync that failed.
                SkyrimNetDiaries::SetPostLoadSyncReady(true);

            }; // end of runSetup lambda body

            SKSE::GetTaskInterface()->AddTask([runSetup]() { (*runSetup)(); });
            break;
        }
        }
    }

    void InitializeLog()
    {
        auto path = SKSE::log::log_directory();
        if (!path) {
            return;
        }

        *path /= "SkyrimNetPhysicalDiaries.log"sv;
        auto sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(path->string(), true);

        auto log = std::make_shared<spdlog::logger>("global log"s, std::move(sink));
        log->set_level(spdlog::level::info);
        log->flush_on(spdlog::level::info);

        spdlog::set_default_logger(std::move(log));
        spdlog::set_pattern("[%H:%M:%S] [%l] %v"s);

        const auto* plugin = SKSE::PluginDeclaration::GetSingleton();
        SKSE::log::info("{} v{}", plugin->GetName(), plugin->GetVersion());
    }
}

// SKSEPlugin_Version and SKSEPlugin_Query are auto-generated by add_commonlibsse_plugin
// in CMakeLists.txt via cmake/CommonLibSSE.cmake — do NOT declare them manually here.

SKSE_PLUGIN_LOAD(const SKSE::LoadInterface* a_skse)
{
    InitializeLog();
    SKSE::log::info("Loading SkyrimNetPhysicalDiaries...");

    // log = false: InitializeLog already set up our logger, and CommonLib's own
    // would replace it (and reopen the same file).
    SKSE::Init(a_skse, { .log = false });

    // Load configuration, then immediately save it back so MO2 writes the file into
    // the Overwrite folder.  This ensures user settings survive future mod updates
    // that would otherwise replace the shipped INI inside the mod folder.
    auto configPath = std::filesystem::current_path() / "Data" / "SKSE" / "Plugins" / "SkyrimNetPhysicalDiaries.ini";
    SkyrimNetDiaries::Config::GetSingleton()->Load(configPath);
    SkyrimNetDiaries::Config::GetSingleton()->Save();  // Persist to MO2 Overwrite on first run

    // Apply log level from config (must come after Load so INI value is available)
    if (SkyrimNetDiaries::Config::GetSingleton()->GetDebugLog()) {
        spdlog::default_logger()->set_level(spdlog::level::debug);
        SKSE::log::info("Debug logging enabled via config");
    }

    SKSE::log::debug("Registering for SKSE messaging interface...");
    auto messaging = SKSE::GetMessagingInterface();
    if (messaging) {
        messaging->RegisterListener(OnMessage);
    }

    SKSE::log::debug("Registering for SKSE serialization...");
    SkyrimNetDiaries::Serialization::Register();

    // Register C++ event handler for diary theft/return detection
    SKSE::log::debug("Registering diary theft/return event handler...");
    SkyrimNetDiaries::DiaryTheftHandler::Register();
    SkyrimNetDiaries::RegisterRetiredBookSweeper();

    // Detect game language and initialize localization (must happen before BookTextHook).
    SkyrimNetDiaries::Localization::GetSingleton()->Initialize();

    // Install book text injection hook (replaces Dynamic Book Framework text delivery,
    // covers SE, AE and VR — see BookTextHook.cpp).
    SkyrimNetDiaries::BookTextHook::Install();

    // Watch for SkyrimNet's keep/clear timeline prompt (see TimelineGate.h).
    SkyrimNetDiaries::TimelineGate::Install();

    // Register Papyrus native functions
    SKSE::log::debug("Registering Papyrus native functions...");
    SkyrimNetDiaries::PapyrusAPI::Register();

    SKSE::log::info("SkyrimNetPhysicalDiaries loaded successfully!");

    return true;
}
