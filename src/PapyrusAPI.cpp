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

#include "PapyrusAPI.h"
#include "BookManager.h"
#include "Config.h"
#include "Database.h"
#include "DiaryDB.h"
#include "DiaryTheftHandler.h"
#include "VolumeSync.h"
#include <spdlog/spdlog.h>

namespace SkyrimNetDiaries::PapyrusAPI {

    namespace {

    // Shared by both diary-event natives.
    void UpdateDiaryForFormID(RE::FormID formId) {
        try {
            // During a load, wait for the post-load sync: until then DiaryDB may still be
            // the previous save's, so even the theft clear below must not run yet.
            if (!SkyrimNetDiaries::IsPostLoadSyncReady()) {
                SKSE::log::debug("[PapyrusAPI] Diary event for FormID 0x{:X} waits for the post-load sync", formId);
                SkyrimNetDiaries::DeferUntilSyncReady(formId, UpdateDiaryForFormID);
                return;
            }

            // The NPC is writing: clear theft tracking (in C++, since Papyrus can't resolve
            // NPCs in unloaded cells).
            std::string uuid = SkyrimNetDiaries::Database::GetUUIDFromFormID(formId);
            if (!uuid.empty()) {
                DiaryTheftHandler::ClearStolenVolumes(uuid);
            }

            SkyrimNetDiaries::UpdateDiaryForActorInternal(formId);
        } catch (const std::exception& e) {
            SKSE::log::error("[PapyrusAPI] UpdateDiaryForFormID exception: {}", e.what());
        } catch (...) {
            SKSE::log::error("[PapyrusAPI] UpdateDiaryForFormID: unknown exception");
        }
    }

    // Legacy entry point for older EventListener scripts.  FormIDs >= 0x80000000 (ESL, high load order) arrive
    // clamped to 0x7FFFFFFF from Papyrus' signed ints; UpdateDiaryFromEvent avoids that.
    void UpdateDiaryForActorWrapper(RE::StaticFunctionTag*, std::int32_t formId) {
        SKSE::log::debug("[PapyrusAPI] UpdateDiaryForActor called with FormID 0x{:X}", formId);
        UpdateDiaryForFormID(static_cast<RE::FormID>(formId));
    }

    // Takes the SkyrimNet_DiaryCreated JSON payload and reads actorFormId as an
    // unsigned 32-bit value, which Papyrus can't represent.
    void UpdateDiaryFromEventWrapper(RE::StaticFunctionTag*, RE::BSFixedString json) {
        try {
            auto payload = nlohmann::json::parse(json.c_str(), nullptr, false);
            if (payload.is_discarded() || !payload.is_object() || !payload.contains("actorFormId")) {
                SKSE::log::warn("[PapyrusAPI] SkyrimNet_DiaryCreated payload has no actorFormId: {}", json.c_str());
                return;
            }
            const auto& value = payload["actorFormId"];
            RE::FormID formId = 0;
            if (value.is_number_unsigned()) {
                formId = static_cast<RE::FormID>(value.get<std::uint64_t>());
            } else if (value.is_number_integer()) {
                formId = static_cast<RE::FormID>(value.get<std::int64_t>());  // signed encoding of the same bits
            } else if (value.is_string()) {
                formId = static_cast<RE::FormID>(std::stoull(value.get<std::string>(), nullptr, 0));
            }
            if (formId == 0) {
                SKSE::log::warn("[PapyrusAPI] SkyrimNet_DiaryCreated has unusable actorFormId: {}", value.dump());
                return;
            }
            SKSE::log::debug("[PapyrusAPI] UpdateDiaryFromEvent: FormID 0x{:X}", formId);
            UpdateDiaryForFormID(formId);
        } catch (const std::exception& e) {
            SKSE::log::error("[PapyrusAPI] UpdateDiaryFromEvent exception: {}", e.what());
        } catch (...) {
            SKSE::log::error("[PapyrusAPI] UpdateDiaryFromEvent: unknown exception");
        }
    }

    // MCM Debug log toggle

    bool MCM_GetDebugLog(RE::StaticFunctionTag*) {
        return SkyrimNetDiaries::Config::GetSingleton()->GetDebugLog();
    }
    void MCM_SetDebugLog(RE::StaticFunctionTag*, bool v) {
        SkyrimNetDiaries::Config::GetSingleton()->SetDebugLog(v);
        SkyrimNetDiaries::Config::GetSingleton()->Save();
        spdlog::default_logger()->set_level(v ? spdlog::level::debug : spdlog::level::info);
        SKSE::log::info("Debug logging {}", v ? "enabled" : "disabled");
    }

    bool MCM_RegenerateTextsOnly(RE::StaticFunctionTag*) {
        SKSE::log::info("[PapyrusAPI] MCM_RegenerateTextsOnly called");
        SkyrimNetDiaries::BookManager::GetSingleton()->RegenerateAllDiaryTexts();
        return true;
    }

    bool MCM_ResetAllDiaries(RE::StaticFunctionTag*) {
        SKSE::log::info("[PapyrusAPI] MCM_ResetAllDiaries called");
        int affected = SkyrimNetDiaries::ResetAllDiariesInternal();
        return affected >= 0;
    }

    // MCM Config getter/setter natives

    std::int32_t MCM_GetEntriesPerVolume(RE::StaticFunctionTag*) {
        return static_cast<std::int32_t>(SkyrimNetDiaries::Config::GetSingleton()->GetEntriesPerVolume());
    }
    void MCM_SetEntriesPerVolume(RE::StaticFunctionTag*, std::int32_t v) {
        SkyrimNetDiaries::Config::GetSingleton()->SetEntriesPerVolume(static_cast<int>(v));
        SkyrimNetDiaries::Config::GetSingleton()->Save();
    }

    std::int32_t MCM_GetFontSizeTitle(RE::StaticFunctionTag*) {
        return static_cast<std::int32_t>(SkyrimNetDiaries::Config::GetSingleton()->GetFontSizeTitle());
    }
    void MCM_SetFontSizeTitle(RE::StaticFunctionTag*, std::int32_t v) {
        SkyrimNetDiaries::Config::GetSingleton()->SetFontSizeTitle(static_cast<int>(v));
        SkyrimNetDiaries::Config::GetSingleton()->Save();
    }

    std::int32_t MCM_GetFontSizeDate(RE::StaticFunctionTag*) {
        return static_cast<std::int32_t>(SkyrimNetDiaries::Config::GetSingleton()->GetFontSizeDate());
    }
    void MCM_SetFontSizeDate(RE::StaticFunctionTag*, std::int32_t v) {
        SkyrimNetDiaries::Config::GetSingleton()->SetFontSizeDate(static_cast<int>(v));
        SkyrimNetDiaries::Config::GetSingleton()->Save();
    }

    std::int32_t MCM_GetFontSizeContent(RE::StaticFunctionTag*) {
        return static_cast<std::int32_t>(SkyrimNetDiaries::Config::GetSingleton()->GetFontSizeContent());
    }
    void MCM_SetFontSizeContent(RE::StaticFunctionTag*, std::int32_t v) {
        SkyrimNetDiaries::Config::GetSingleton()->SetFontSizeContent(static_cast<int>(v));
        SkyrimNetDiaries::Config::GetSingleton()->Save();
    }

    std::int32_t MCM_GetFontSizeSmall(RE::StaticFunctionTag*) {
        return static_cast<std::int32_t>(SkyrimNetDiaries::Config::GetSingleton()->GetFontSizeSmall());
    }
    void MCM_SetFontSizeSmall(RE::StaticFunctionTag*, std::int32_t v) {
        SkyrimNetDiaries::Config::GetSingleton()->SetFontSizeSmall(static_cast<int>(v));
        SkyrimNetDiaries::Config::GetSingleton()->Save();
    }

    bool MCM_GetShowDateHeaders(RE::StaticFunctionTag*) {
        return SkyrimNetDiaries::Config::GetSingleton()->GetShowDateHeaders();
    }
    void MCM_SetShowDateHeaders(RE::StaticFunctionTag*, bool v) {
        SkyrimNetDiaries::Config::GetSingleton()->SetShowDateHeaders(v);
        SkyrimNetDiaries::Config::GetSingleton()->Save();
    }

    bool MCM_GetPlayerDiaryBooks(RE::StaticFunctionTag*) {
        return SkyrimNetDiaries::Config::GetSingleton()->GetPlayerDiaryBooks();
    }
    // 1 on, 0 off, -1 the default (off with writing installed, on without).
    void MCM_SetPlayerDiaryBooks(RE::StaticFunctionTag*, std::int32_t v) {
        auto* config = SkyrimNetDiaries::Config::GetSingleton();
        const bool was = config->GetPlayerDiaryBooks();
        config->SetPlayerDiaryBooks(static_cast<int>(v));
        config->Save();
        // Turned on: catch the player's diary up now (the same update a new entry runs) rather
        // than at their next entry or the next load.
        if (!was && config->GetPlayerDiaryBooks()) {
            SKSE::GetTaskInterface()->AddTask([]() { SkyrimNetDiaries::UpdateDiaryForActorInternal(0x14); });
        }
    }

    std::int32_t MCM_GetEditKey(RE::StaticFunctionTag*) {
        return static_cast<std::int32_t>(SkyrimNetDiaries::Config::GetSingleton()->GetEditKey());
    }
    void MCM_SetEditKey(RE::StaticFunctionTag*, std::int32_t v) {
        SkyrimNetDiaries::Config::GetSingleton()->SetEditKey(static_cast<int>(v));
        SkyrimNetDiaries::Config::GetSingleton()->Save();
    }

    std::int32_t MCM_GetDeleteKey(RE::StaticFunctionTag*) {
        return static_cast<std::int32_t>(SkyrimNetDiaries::Config::GetSingleton()->GetDeleteKey());
    }
    void MCM_SetDeleteKey(RE::StaticFunctionTag*, std::int32_t v) {
        SkyrimNetDiaries::Config::GetSingleton()->SetDeleteKey(static_cast<int>(v));
        SkyrimNetDiaries::Config::GetSingleton()->Save();
    }

    std::int32_t MCM_GetNewEntryKey(RE::StaticFunctionTag*) {
        return static_cast<std::int32_t>(SkyrimNetDiaries::Config::GetSingleton()->GetNewEntryKey());
    }
    void MCM_SetNewEntryKey(RE::StaticFunctionTag*, std::int32_t v) {
        SkyrimNetDiaries::Config::GetSingleton()->SetNewEntryKey(static_cast<int>(v));
        SkyrimNetDiaries::Config::GetSingleton()->Save();
    }

    RE::BSFixedString MCM_GetFontFace(RE::StaticFunctionTag*) {
        return SkyrimNetDiaries::Config::GetSingleton()->GetFontFace().c_str();
    }
    void MCM_SetFontFace(RE::StaticFunctionTag*, RE::BSFixedString v) {
        SkyrimNetDiaries::Config::GetSingleton()->SetFontFace(v.c_str());
        SkyrimNetDiaries::Config::GetSingleton()->Save();
    }

    bool RegisterFunctions(RE::BSScript::IVirtualMachine* a_vm) {
        if (!a_vm) {
            SKSE::log::error("Failed to register Papyrus functions - VM is null");
            return false;
        }

        a_vm->RegisterFunction("UpdateDiaryForActor",  "SkyrimNetDiaries_Native", UpdateDiaryForActorWrapper);
        a_vm->RegisterFunction("UpdateDiaryFromEvent", "SkyrimNetDiaries_Native", UpdateDiaryFromEventWrapper);

        // MCM Debug log
        a_vm->RegisterFunction("GetDebugLog", "SkyrimNetDiaries_MCM", MCM_GetDebugLog);
        a_vm->RegisterFunction("SetDebugLog", "SkyrimNetDiaries_MCM", MCM_SetDebugLog);

        // MCM Maintenance
        a_vm->RegisterFunction("RegenerateTextsOnly", "SkyrimNetDiaries_MCM", MCM_RegenerateTextsOnly);
        a_vm->RegisterFunction("ResetAllDiaries",      "SkyrimNetDiaries_MCM", MCM_ResetAllDiaries);

        // MCM Config
        a_vm->RegisterFunction("GetEntriesPerVolume", "SkyrimNetDiaries_MCM", MCM_GetEntriesPerVolume);
        a_vm->RegisterFunction("SetEntriesPerVolume", "SkyrimNetDiaries_MCM", MCM_SetEntriesPerVolume);
        a_vm->RegisterFunction("GetFontSizeTitle",    "SkyrimNetDiaries_MCM", MCM_GetFontSizeTitle);
        a_vm->RegisterFunction("SetFontSizeTitle",    "SkyrimNetDiaries_MCM", MCM_SetFontSizeTitle);
        a_vm->RegisterFunction("GetFontSizeDate",     "SkyrimNetDiaries_MCM", MCM_GetFontSizeDate);
        a_vm->RegisterFunction("SetFontSizeDate",     "SkyrimNetDiaries_MCM", MCM_SetFontSizeDate);
        a_vm->RegisterFunction("GetFontSizeContent",  "SkyrimNetDiaries_MCM", MCM_GetFontSizeContent);
        a_vm->RegisterFunction("SetFontSizeContent",  "SkyrimNetDiaries_MCM", MCM_SetFontSizeContent);
        a_vm->RegisterFunction("GetFontSizeSmall",    "SkyrimNetDiaries_MCM", MCM_GetFontSizeSmall);
        a_vm->RegisterFunction("SetFontSizeSmall",    "SkyrimNetDiaries_MCM", MCM_SetFontSizeSmall);
        a_vm->RegisterFunction("GetShowDateHeaders",  "SkyrimNetDiaries_MCM", MCM_GetShowDateHeaders);
        a_vm->RegisterFunction("SetShowDateHeaders",  "SkyrimNetDiaries_MCM", MCM_SetShowDateHeaders);
        a_vm->RegisterFunction("GetPlayerDiaryBooks", "SkyrimNetDiaries_MCM", MCM_GetPlayerDiaryBooks);
        a_vm->RegisterFunction("SetPlayerDiaryBooks", "SkyrimNetDiaries_MCM", MCM_SetPlayerDiaryBooks);
        a_vm->RegisterFunction("GetEditKey",          "SkyrimNetDiaries_MCM", MCM_GetEditKey);
        a_vm->RegisterFunction("SetEditKey",          "SkyrimNetDiaries_MCM", MCM_SetEditKey);
        a_vm->RegisterFunction("GetDeleteKey",        "SkyrimNetDiaries_MCM", MCM_GetDeleteKey);
        a_vm->RegisterFunction("SetDeleteKey",        "SkyrimNetDiaries_MCM", MCM_SetDeleteKey);
        a_vm->RegisterFunction("GetNewEntryKey",      "SkyrimNetDiaries_MCM", MCM_GetNewEntryKey);
        a_vm->RegisterFunction("SetNewEntryKey",      "SkyrimNetDiaries_MCM", MCM_SetNewEntryKey);
        a_vm->RegisterFunction("GetFontFace",         "SkyrimNetDiaries_MCM", MCM_GetFontFace);
        a_vm->RegisterFunction("SetFontFace",         "SkyrimNetDiaries_MCM", MCM_SetFontFace);

        SKSE::log::info("Registered Physical Diary Papyrus API functions");
        return true;
    }

    } // namespace

    void Register() {
        auto papyrus = SKSE::GetPapyrusInterface();
        if (!papyrus) {
            SKSE::log::error("Failed to get Papyrus interface");
            return;
        }

        if (!papyrus->Register(RegisterFunctions)) {
            SKSE::log::error("Failed to register Papyrus API functions");
            return;
        }

        SKSE::log::info("Physical Diary Papyrus API registered successfully");
    }

} // namespace SkyrimNetDiaries::PapyrusAPI
