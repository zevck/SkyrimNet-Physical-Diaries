#include "BookManager.h"
#include "BookTextHook.h"
#include "Config.h"
#include "Database.h"
#include "DiaryDB.h"
#include "DiaryTheftHandler.h"
#include "InterPluginAPI.h"
#include "Localization.h"
#include "PapyrusAPI.h"
#include "SaveFolder.h"
#include "Serialization.h"
#include "VolumeSync.h"
#include <spdlog/sinks/basic_file_sink.h>
#include <unordered_set>

namespace {

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
                // Check for required dependency: Dynamic Persistent Forms
                {
                    auto* dataHandler = RE::TESDataHandler::GetSingleton();
                    bool dpfInstalled = dataHandler && dataHandler->LookupModByName("Dynamic Persistent Forms.esp");
                    if (!dpfInstalled) {
                        SKSE::log::error("kDataLoaded: 'Dynamic Persistent Forms.esp' is not installed — diary books cannot be created");
                        SKSE::GetTaskInterface()->AddTask([]() {
                            auto* msgBoxData = RE::UIMessageDataFactory::Create<RE::MessageBoxData>();
                            if (msgBoxData) {
                                msgBoxData->bodyText = "SkyrimNet Physical Diaries requires 'Dynamic Persistent Forms' to be installed.\n\nDiary books cannot be created without it. Please install Dynamic Persistent Forms and restart the game.";
                                msgBoxData->buttonText.push_back("OK");
                                msgBoxData->cancelOptionIndex = 0;
                                RE::MessageBoxMenu::QueueMessage(msgBoxData);
                                SKSE::log::info("kDataLoaded: DPF missing warning queued");
                            }
                        });
                    }
                }
                
                // Verify the diary template books resolve.  If they don't, every
                // diary creation silently fails with "Template book not found" spam.
                // Common causes: ESP not actually enabled, an EditorID-exposure
                // plugin (e.g. po3_Tweaks / Native EditorID Fix) missing or the wrong
                // runtime build, or a tool stripped the template records.
                {
                    auto* t1 = RE::TESForm::LookupByEditorID<RE::TESObjectBOOK>("SkyrimNetDiaryTemplate");
                    auto* t2 = RE::TESForm::LookupByEditorID<RE::TESObjectBOOK>("SkyrimNetDiaryTemplate2");
                    auto* t3 = RE::TESForm::LookupByEditorID<RE::TESObjectBOOK>("SkyrimNetDiaryTemplate3");
                    auto* tN = RE::TESForm::LookupByEditorID<RE::TESObjectBOOK>("SkyrimNetDiaryTemplateN");
                    if (!t1 || !t2 || !t3 || !tN) {
                        SKSE::log::error("================================================================");
                        SKSE::log::error("[Physical Diaries] DIARY TEMPLATE BOOKS NOT FOUND — diaries cannot be created!");
                        SKSE::log::error("  SkyrimNetDiaryTemplate:  {}", t1 ? "OK" : "MISSING");
                        SKSE::log::error("  SkyrimNetDiaryTemplate2: {}", t2 ? "OK" : "MISSING");
                        SKSE::log::error("  SkyrimNetDiaryTemplate3: {}", t3 ? "OK" : "MISSING");
                        SKSE::log::error("  SkyrimNetDiaryTemplateN: {}", tN ? "OK" : "MISSING");
                        SKSE::log::error("  Possible causes:");
                        SKSE::log::error("   1. 'SkyrimNet Physical Diaries.esp' is not enabled in the load order");
                        SKSE::log::error("   2. An EditorID-exposure plugin (po3_Tweaks / Native EditorID Fix) is");
                        SKSE::log::error("      missing or is the wrong runtime build (SE vs AE vs VR)");
                        SKSE::log::error("   3. The ESP was modified by a tool that stripped the template records");
                        SKSE::log::error("================================================================");
                        SKSE::GetTaskInterface()->AddTask([]() {
                            auto* msgBoxData = RE::UIMessageDataFactory::Create<RE::MessageBoxData>();
                            if (msgBoxData) {
                                msgBoxData->bodyText = "SkyrimNet Physical Diaries: diary template books were not found.\n\nDiaries cannot be created. Check that:\n - 'SkyrimNet Physical Diaries.esp' is enabled\n - Native EditorID Fix (or po3_Tweaks) is installed for your game version\n\nSee SkyrimNetPhysicalDiaries.log for details.";
                                msgBoxData->buttonText.push_back("OK");
                                msgBoxData->cancelOptionIndex = 0;
                                RE::MessageBoxMenu::QueueMessage(msgBoxData);
                            }
                        });
                    } else {
                        SKSE::log::info("[Physical Diaries] Diary template books verified (all 4 resolved)");
                    }
                }

                // Now that GMSTs are loaded, read localized month/day names
                SkyrimNetDiaries::Localization::GetSingleton()->ReadGMSTs();
            } catch (const std::exception& e) {
                SKSE::log::error("Exception in kDataLoaded: {}", e.what());
            } catch (...) {
                SKSE::log::error("Unknown exception in kDataLoaded");
            }
            break;
        }
        case SKSE::MessagingInterface::kSaveGame: {
            // Mark every tracked volume as having been written into a .ess save file.
            // On the next kPostLoadGame, QueueInventoryCheck will skip these volumes
            // so legitimately taken/stolen books are not re-added to NPC inventories.
            SkyrimNetDiaries::DiaryDB::GetSingleton()->MarkAllVolumesPersisted();
            // Sync the in-memory flag too so the current session stays consistent.
            for (auto& [uuid, volumes] : SkyrimNetDiaries::BookManager::GetSingleton()->GetAllBooksRef()) {
                for (auto& vol : volumes) vol.persistedInSave = true;
            }
            SKSE::log::debug("kPostSaveGame: all volumes marked as persisted");
            break;
        }

        case SKSE::MessagingInterface::kPostLoadGame: {
            // First, and independent of SkyrimNet: DPF has restored its forms by now.
            SkyrimNetDiaries::BookManager::SanitizeLoadedBookForms();

            if (!SkyrimNetDiaries::Database::InitializeAPI()) {
                SKSE::log::warn("Failed to initialize API (SkyrimNet may not be loaded yet)");
                break;
            }
            SKSE::log::info("✓ SkyrimNet API ready");

            // Detect save folder (from co-save SNDF record or SkyrimNet.log fallback).
            // Always re-detect on each load in case the player loaded a different save.
            SkyrimNetDiaries::SaveFolder::Clear();
            SkyrimNetDiaries::SaveFolder::DetectFromLog();

            // Open the per-save SQLite diary DB.
            if (!SkyrimNetDiaries::SaveFolder::Get().empty()) {
                SkyrimNetDiaries::DiaryDB::GetSingleton()->Open(SkyrimNetDiaries::SaveFolder::Get());
            } else {
                SKSE::log::warn("kPostLoadGame: save folder still unknown — DiaryDB not opened");
            }

            // Clear the actor reference cache — pointers from the previous load session
            // may be stale (or nullptr from failed lookups).  A fresh search runs on this load.
            SkyrimNetDiaries::BookManager::ClearActorCache();

            // Queue the post-load setup task with a retry loop: SkyrimNet's Papyrus-based
            // memory system re-initialises asynchronously on reload and may not be ready
            // for several frames.  We poll IsMemorySystemReady() and re-queue ourselves
            // on the next game frame until it is (capped at 300 attempts ≈ ~5 seconds).
            auto runSetup = std::make_shared<std::function<void()>>();
            *runSetup = [runSetup, attempt = 0]() mutable {
                if (!SkyrimNetDiaries::Database::IsMemorySystemReady()) {
                    if (++attempt >= 300) {
                        SKSE::log::error("kPostLoadGame: SkyrimNet memory system never became ready after 300 retries — giving up");
                        return;
                    }
                    if (attempt % 30 == 1) {
                        SKSE::log::debug("kPostLoadGame: waiting for SkyrimNet memory system (attempt {})...", attempt);
                    }
                    SKSE::GetTaskInterface()->AddTask([runSetup]() { (*runSetup)(); });
                    return;
                }

                if (attempt > 0) {
                    SKSE::log::info("kPostLoadGame: SkyrimNet memory system ready after {} retries", attempt);
                }

                // SkyrimNet clears decorator registrations on every load.
                DiaryTheftHandler::RegisterStolenDecorator();

                auto invalidActors = SkyrimNetDiaries::BookManager::GetSingleton()->LoadFromDB();

                // For volumes whose DPF form still exists in process memory but whose
                // inventory entry was wiped by a reload-without-save, re-add the book.
                SkyrimNetDiaries::BookManager::GetSingleton()->QueueInventoryCheck();

                // Clear stolen-volume records if this save is earlier in game time
                // than the last session (the theft happened in an abandoned timeline).
                DiaryTheftHandler::ReconcileAfterLoad();

                // Build skip set: deduplicated UUIDs being immediately recovered.
                std::unordered_set<std::string> skipUuids;
                if (!invalidActors.empty()) {
                    SKSE::log::info("kPostLoadGame: {} actor(s) had invalid FormIDs — queuing immediate recreation", invalidActors.size());
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
                SkyrimNetDiaries::QueueSealedVolumeRecovery(skipUuids);
                SkyrimNetDiaries::QueueBatchCatchUpScan(std::move(skipUuids));

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

extern "C" DLLEXPORT bool SKSEAPI SKSEPlugin_Load(const SKSE::LoadInterface* a_skse)
{
    InitializeLog();
    SKSE::log::info("Loading SkyrimNetPhysicalDiaries...");

    SKSE::Init(a_skse);
    
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

    // Initialize BookManager with template book Editor IDs from ESP
    // 4 templates total: base, 2 variants, and Nightingale special
    SkyrimNetDiaries::BookManager::GetSingleton()->Initialize(
        "SkyrimNetDiaryTemplate",      // Base template
        "SkyrimNetDiaryTemplate2",     // Variant 2
        "SkyrimNetDiaryTemplate3",     // Variant 3
        "",                              // Unused
        "",                              // Unused
        "SkyrimNetDiaryTemplateN"      // Nightingale journal
    );

    // Register C++ event handler for diary theft/return detection
    SKSE::log::debug("Registering diary theft/return event handler...");
    DiaryTheftHandler::Register();

    // Detect game language and initialize localization (must happen before BookTextHook).
    SkyrimNetDiaries::Localization::GetSingleton()->Initialize();

    // Install book text injection hook (replaces Dynamic Book Framework text delivery,
    // covers SE, AE and VR — see BookTextHook.cpp).
    BookTextHook::Install();

    // Register Papyrus native functions
    SKSE::log::debug("Registering Papyrus native functions...");
    PapyrusAPI::Register();

    SKSE::log::info("SkyrimNetPhysicalDiaries loaded successfully!");
    
    return true;
}
