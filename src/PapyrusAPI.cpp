#include "PapyrusAPI.h"
#include "BookManager.h"
#include "Config.h"
#include "Database.h"
#include "DiaryDB.h"
#include "VolumeSync.h"
#include <spdlog/spdlog.h>

namespace PapyrusAPI {

    // Shared by both diary-event natives.
    void UpdateDiaryForFormID(RE::FormID formId) {
        // Clear theft tracking here in C++ so it always runs regardless of whether the
        // Papyrus caller was able to resolve the Actor object (NPCs not in a loaded cell
        // will return None from Game.GetForm, which silently skips SetTheftCleared).
        std::string uuid = SkyrimNetDiaries::Database::GetUUIDFromFormID(formId);
        if (!uuid.empty() && uuid != "0") {
            auto* diaryDB = SkyrimNetDiaries::DiaryDB::GetSingleton();
            diaryDB->ClearAllStolenVolumes(uuid);
            auto calendar = RE::Calendar::GetSingleton();
            if (calendar) {
                diaryDB->UpdateLastKnownGameTime(uuid, calendar->GetCurrentGameTime() * 86400.0);
            }
        }

        SkyrimNetDiaries::UpdateDiaryForActorInternal(formId);
    }

    // Legacy entry point, kept for older EventListener scripts.  Papyrus ints are
    // signed, so FormIDs >= 0x80000000 (ESL and high load-order NPCs) arrive clamped
    // to 0x7FFFFFFF from a string conversion; UpdateDiaryFromEvent avoids that.
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
        }
    }

    RE::BSFixedString GetDiaryTheftStatus(RE::StaticFunctionTag*, RE::Actor* akActor) {
        if (!akActor) {
            return "{\"error\": \"null actor\"}";
        }
        
        std::string uuid = SkyrimNetDiaries::Database::GetUUIDFromFormID(akActor->GetFormID());
        if (uuid.empty() || uuid == "0") {
            return "{\"stolen\": false}";  // Unknown actor = no theft tracking
        }
        
        bool hasStolen = SkyrimNetDiaries::DiaryDB::GetSingleton()->HasAnyStolenVolumes(uuid);
        
        // If any volume is stolen, diary is stolen
        if (hasStolen) {
            return "{\"stolen\": true, \"chronicled\": false}";
        }
        
        return "{\"stolen\": false}";
    }
    
    RE::BSFixedString IsDiaryStolen(RE::StaticFunctionTag*, RE::Actor* akActor) {
        if (!akActor) {
            SKSE::log::debug("[IsDiaryStolen] Null actor - returning false");
            return "false";
        }
        
        std::string uuid = SkyrimNetDiaries::Database::GetUUIDFromFormID(akActor->GetFormID());
        if (uuid.empty() || uuid == "0") {
            SKSE::log::debug("[IsDiaryStolen] {} - no UUID, returning false", akActor->GetName());
            return "false";  // Unknown actor = no theft tracking
        }
        
        bool hasStolen = SkyrimNetDiaries::DiaryDB::GetSingleton()->HasAnyStolenVolumes(uuid);
        
        SKSE::log::debug("[IsDiaryStolen] {} (UUID: {}) - has stolen volumes: {}", 
                       akActor->GetName(), uuid, hasStolen ? "YES" : "NO");
        
        return hasStolen ? "true" : "false";
    }
    
    void SetTheftCleared(RE::StaticFunctionTag*, RE::Actor* akActor) {
        if (!akActor) {
            SKSE::log::warn("[PapyrusAPI] SetTheftCleared called with null actor");
            return;
        }
        
        std::string uuid = SkyrimNetDiaries::Database::GetUUIDFromFormID(akActor->GetFormID());
        if (uuid.empty() || uuid == "0") {
            SKSE::log::warn("[PapyrusAPI] SetTheftCleared: Unable to get UUID for actor {}", akActor->GetName());
            return;
        }
        
        SKSE::log::debug("[PapyrusAPI] SetTheftCleared called for {} (FormID: 0x{:X}, UUID: {})", 
                       akActor->GetName(), akActor->GetFormID(), uuid);
        
        // Clear ALL stolen volumes for this actor - they wrote a new diary entry
        auto* diaryDB = SkyrimNetDiaries::DiaryDB::GetSingleton();
        diaryDB->ClearAllStolenVolumes(uuid);
        
        // Update last_known_game_time for backwards time travel detection
        auto calendar = RE::Calendar::GetSingleton();
        if (calendar) {
            double gameTime = calendar->GetCurrentGameTime() * 86400.0;
            diaryDB->UpdateLastKnownGameTime(uuid, gameTime);
        }
        
        SKSE::log::debug("[PapyrusAPI] Cleared all stolen volumes for {} (UUID: {}) - diary entry written", 
                       akActor->GetName(), uuid);
    }

    // -------------------------------------------------------------------------
    // MCM Maintenance natives
    // -------------------------------------------------------------------------

    // -------------------------------------------------------------------------
    // MCM Debug log toggle
    // -------------------------------------------------------------------------

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
        auto* bookManager = SkyrimNetDiaries::BookManager::GetSingleton();
        if (!bookManager) return false;
        bookManager->RegenerateAllDiaryTexts();
        return true;
    }

    bool MCM_ResetAllDiaries(RE::StaticFunctionTag*) {
        SKSE::log::info("[PapyrusAPI] MCM_ResetAllDiaries called");
        int affected = SkyrimNetDiaries::ResetAllDiariesInternal();
        return affected >= 0;
    }

    // -------------------------------------------------------------------------
    // MCM Config getter/setter natives
    // -------------------------------------------------------------------------

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
        a_vm->RegisterFunction("GetDiaryTheftStatus", "SkyrimNetDiaries_API", GetDiaryTheftStatus);
        a_vm->RegisterFunction("IsDiaryStolen",       "SkyrimNetDiaries_API", IsDiaryStolen);
        a_vm->RegisterFunction("SetTheftCleared",     "SkyrimNetDiaries_API", SetTheftCleared);

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
        a_vm->RegisterFunction("GetFontFace",         "SkyrimNetDiaries_MCM", MCM_GetFontFace);
        a_vm->RegisterFunction("SetFontFace",         "SkyrimNetDiaries_MCM", MCM_SetFontFace);

        SKSE::log::info("Registered Physical Diary Papyrus API functions");
        return true;
    }

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

} // namespace PapyrusAPI
