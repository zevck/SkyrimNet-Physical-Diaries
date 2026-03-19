#include "PapyrusAPI.h"
#include "BookManager.h"
#include "Config.h"
#include "DiaryTheftHandler.h"
#include "Database.h"
#include "DiaryDB.h"
#include <spdlog/spdlog.h>

// Forward declarations from main.cpp (defined in global namespace)
extern void UpdateDiaryForActorInternal(RE::FormID formId);
extern int  ResetAllDiariesInternal();
extern void CreateAllVolumesForActor(const std::string& uuid, const std::string& actorName,
    RE::FormID formId, const std::string& bioTemplateName,
    std::vector<SkyrimNetDiaries::DiaryEntry> allEntries, int startingVolumeNumber);

namespace PapyrusAPI {

    bool ApplyStolenDiaryEffect(RE::StaticFunctionTag*, RE::Actor* akActor) {
        if (!akActor) {
            SKSE::log::warn("ApplyStolenDiaryEffect called with null actor");
            return false;
        }

        // Look up the ability spell that contains the SNPD_DiaryStolen magic effect
        auto* spell = RE::TESForm::LookupByEditorID<RE::SpellItem>("SNPD_DiaryStorageSpell");
        if (!spell) {
            SKSE::log::error("Failed to find SNPD_DiaryStorageSpell - make sure it exists in the ESP");
            return false;
        }

        // Check if actor already has this spell
        if (akActor->HasSpell(spell)) {
            SKSE::log::debug("{} already has SNPD_DiaryStorageSpell", akActor->GetName());
            return true; // Not an error, just already applied
        }

        // Add the spell (ability) to the actor
        akActor->AddSpell(spell);
        
        SKSE::log::debug("Applied SNPD_DiaryStorageSpell to {} (FormID: 0x{:X})", 
                       akActor->GetName(), akActor->GetFormID());
        return true;
    }

    bool RemoveStolenDiaryEffect(RE::StaticFunctionTag*, RE::Actor* akActor) {
        if (!akActor) {
            SKSE::log::warn("RemoveStolenDiaryEffect called with null actor");
            return false;
        }

        // Look up the ability spell that contains the SNPD_DiaryStolen magic effect
        auto* spell = RE::TESForm::LookupByEditorID<RE::SpellItem>("SNPD_DiaryStorageSpell");
        if (!spell) {
            SKSE::log::error("Failed to find SNPD_DiaryStorageSpell - make sure it exists in the ESP");
            return false;
        }

        // Check if actor has this spell
        if (!akActor->HasSpell(spell)) {
            SKSE::log::debug("{} doesn't have SNPD_DiaryStorageSpell (already removed or never applied)", akActor->GetName());
            return true; // Not an error, just not present
        }

        // Remove the spell (ability) from the actor
        akActor->RemoveSpell(spell);
        
        SKSE::log::debug("Removed SNPD_DiaryStorageSpell from {} (FormID: 0x{:X}) - diary was returned", 
                       akActor->GetName(), akActor->GetFormID());
        return true;
    }
    
    void UpdateDiaryForActorWrapper(RE::StaticFunctionTag*, std::int32_t formId) {
        SKSE::log::debug("[PapyrusAPI] UpdateDiaryForActor called with FormID 0x{:X}", formId);

        // Clear theft tracking here in C++ so it always runs regardless of whether the
        // Papyrus caller was able to resolve the Actor object (NPCs not in a loaded cell
        // will return None from Game.GetForm, which silently skips SetTheftCleared).
        std::string uuid = SkyrimNetDiaries::Database::GetUUIDFromFormID(static_cast<uint32_t>(formId));
        if (!uuid.empty() && uuid != "0") {
            auto* diaryDB = SkyrimNetDiaries::DiaryDB::GetSingleton();
            diaryDB->ClearAllStolenVolumes(uuid);
            auto calendar = RE::Calendar::GetSingleton();
            if (calendar) {
                diaryDB->UpdateLastKnownGameTime(uuid, calendar->GetCurrentGameTime() * 86400.0);
            }
        }

        ::UpdateDiaryForActorInternal(static_cast<RE::FormID>(formId));
    }

    RE::TESForm* GetStolenFaction(RE::StaticFunctionTag*) {
        return RE::TESForm::LookupByEditorID<RE::TESFaction>("SNPD_DiaryStolenFaction");
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
        int affected = ::ResetAllDiariesInternal();
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

    bool MCM_GenerateJapaneseTestDiaries(RE::StaticFunctionTag*) {
        SKSE::log::info("[PapyrusAPI] MCM_GenerateJapaneseTestDiaries called");

        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player) {
            SKSE::log::error("[PapyrusAPI] Could not get player singleton");
            return false;
        }

        RE::FormID playerFormId = player->GetFormID();
        std::string playerName = player->GetName();
        if (playerName.empty()) playerName = "\u65C5\u4EBA";  // 旅人 "traveller"

        std::string uuid = SkyrimNetDiaries::Database::GetUUIDFromFormID(playerFormId);
        if (uuid.empty() || uuid == "0") uuid = "jp_test_player";

        auto* calendar = RE::Calendar::GetSingleton();
        double baseTime = calendar ? calendar->GetCurrentGameTime() * 86400.0 : 7000000.0;
        // Ensure entries don't get negative entry_date values (FormatGameDate doesn't clamp).
        // Skyrim game start is ~17 Last Seed 4E 201; guarantee at least 15 game-days of headroom.
        static constexpr double kMinBase = 15.0 * 86400.0;
        if (baseTime < kMinBase) baseTime = kMinBase;

        // 15 entries spanning 15 game-days, mixing hiragana/katakana/kanji to exercise
        // multi-byte UTF-8 rendering and any byte-offset pagination logic.
        static const char* kContent[15] = {
            // Day 1 – arrival at Whiterun
            "\u30DB\u30EF\u30A4\u30C8\u30E9\u30F3\u306B\u5230\u7740\u3057\u305F\u3002\u57CE\u58C1\u306F\u9AD8\u304F\u30A2\u30FC\u30A4\u30F3\u306E\u5C71\u3005\u304C\u9060\u304F\u306B\u898B\u3048\u308B\u3002\u306F\u3058\u3081\u3066\u306E\u5BFF\u4E2D\u3001\u5C55\u671B\u306B\u611F\u52D5\u3057\u305F\u3002\u5546\u4EBA\u305F\u3061\u306F\u308F\u305F\u3057\u306E\u8A00\u8449\u306B\u8033\u3092\u50BE\u3051\u3066\u304F\u308C\u305F\u3002",
            // Day 2 – training with sword
            "\u5263\u306E\u7A3C\u304E\u3092\u7D9A\u3051\u305F\u3002\u6307\u5C0E\u8005\u306E\u8FBA\u7960\u306F\u53B3\u3057\u304F\u3001\u6BCE\u65E5\u5C4C\u306E\u5185\u3067\u7B4B\u8089\u304C\u75DB\u304F\u306A\u308B\u3002\u305D\u308C\u3067\u3082\u5263\u6280\u306F\u5C11\u3057\u305A\u3064\u4E0A\u9054\u3057\u3066\u3044\u308B\u3068\u601D\u3046\u3002\u5927\u5265\u6226\u58EB\u306B\u306A\u308B\u306B\u306F\u307E\u3060\u9053\u306F\u9577\u3044\u3002",
            // Day 3 – Companion quest
            "\u30B3\u30F3\u30D1\u30CB\u30AA\u30F3\u30BA\u306E\u4EFB\u52D9\u3092\u53D7\u3051\u305F\u3002\u8CC0\u76D7\u306E\u5DE3\u7A9F\u306B\u575F\u308A\u8FBC\u3080\u4E88\u5B9A\u3060\u3002\u96AA\u3057\u3044\u9053\u306E\u308A\u3060\u304C\u3001\u5831\u916C\u306F\u5341\u5206\u306A\u306F\u305A\u3060\u3002\u30D5\u30A1\u30FC\u30EC\u30F3\u30AC\u30FC\u306E\u538B\u6C17\u306B\u5727\u5012\u3055\u308C\u306A\u3044\u3088\u3046\u306B\u5FC3\u304C\u3051\u305F\u3002",
            // Day 4 – blizzard on the road
            "\u5C71\u9053\u3067\u5439\u96EA\u306B\u9042\u3063\u305F\u3002\u8996\u754C\u306F\u307B\u3068\u3093\u3069\u30BC\u30ED\u3002\u9A6C\u304C\u3072\u3069\u304F\u6015\u304C\u308A\u3001\u5C71\u5C0F\u5C4B\u306B\u9003\u3052\u8FBC\u3093\u3060\u3002\u696D\u8005\u306E\u7378\u7A81\u304D\u5C71\u7537\u306B\u6696\u304B\u3044\u8C46\u306E\u30B9\u30FC\u30D7\u3092\u3054\u3061\u305D\u3046\u306B\u306A\u3063\u305F\u3002",
            // Day 5 – night sky
            "\u591C\u3001\u661F\u7A7A\u304C\u7D20\u6674\u3089\u3057\u304B\u3063\u305F\u3002\u30EC\u30C3\u30C9\u30A6\u30A9\u30FC\u30BF\u30FC\u306E\u5149\u304C\u5C97\u306E\u5F7C\u65B9\u306B\u6CE2\u6253\u3064\u3002\u6545\u90F7\u306E\u5BB6\u6097\u304C\u306A\u3064\u304B\u3057\u304F\u601D\u3044\u5BF8\u3055\u308C\u305F\u3002\u65C5\u306B\u51FA\u305F\u7406\u7531\u3092\u4ECA\u4E00\u5EA6\u601D\u3044\u8FBA\u3063\u305F\u3002",
            // Day 6 – Dragonsreach
            "\u30C9\u30E9\u30B4\u30F3\u30BA\u30EA\u30FC\u30C1\u3067\u78B0\u8B58\u3092\u304B\u308F\u3057\u305F\u3002\u30D0\u30EB\u30B0\u30EB\u30FC\u30D5\u516C\u7235\u306F\u51B7\u305F\u3044\u773C\u3067\u308F\u305F\u3057\u3092\u898B\u305F\u3002\u5C71\u5CF0\u306E\u5467\u3073\u3001\u30C9\u30E9\u30B4\u30F3\u30DC\u30FC\u30F3\u306E\u770B\u5B66\u306B\u3064\u3044\u3066\u554F\u308F\u308C\u305F\u3002\u6B63\u76F4\u306B\u7B54\u3048\u308B\u3079\u304D\u304B\u60A9\u3093\u3060\u3002",
            // Day 7 – alchemy
            "\u30DB\u30EF\u30A4\u30C8\u30E9\u30F3\u306E\u9EC4\u91D1\u9B3C\u306E\u9EA6\u347C\u5E97\u3067\u932B\u91D1\u8853\u3092\u5B66\u3093\u3060\u3002\u836F\u306E\u5C71\u3068\u91CB\u8584\u304C\u58C1\u4E00\u9762\u306B\u4E26\u3093\u3067\u3044\u305F\u3002\u6CBB\u7642\u306E\u30DD\u30FC\u30B7\u30E7\u30F3\u3092\u4E8C\u672C\u4F5C\u308A\u3001\u4E00\u672C\u306F\u5931\u6557\u4F5C\u306B\u7D42\u308F\u3063\u305F\u3002\u5B66\u3073\u306F\u7E5A\u308A\u8FBA\u307F\u304C\u5FC5\u8981\u3060\u3002",
            // Day 8 – discovered a dragon burial mound
            "\u53E4\u3044\u7AEF\u306E\u9F99\u306E\u57CB\u846C\u5730\u3092\u767A\u898B\u3057\u305F\u3002\u5730\u9762\u306B\u5575\u308A\u8FBC\u307E\u308C\u305F\u7960\u5357\u5B57\u306F\u548C\u8A33\u3067\u304D\u306A\u304B\u3063\u305F\u3002\u9B54\u529B\u306E\u6B8B\u6ED5\u304C\u8582\u8086\u3092\u7ACB\u3066\u308B\u3002\u4F55\u304B\u3092\u7F6E\u3044\u3066\u304D\u305F\u8CA1\u5B9D\u304C\u5C71\u3068\u7A4D\u307E\u3063\u3066\u3044\u305F\u3002",
            // Day 9 – river crossing
            "\u30E6\u30FC\u30EA\u30A6\u30B9\u5DDD\u3092\u6BCE\u6E21\u3063\u305F\u3002\u6025\u6D41\u306F\u5F37\u304F\u518D\u5EA6\u6D41\u3055\u308C\u305D\u3046\u306B\u306A\u3063\u305F\u3002\u8352\u91CE\u306E\u30ED\u30B3\u304C\u5C0F\u5C4B\u306E\u5DE3\u306B\u5DE3\u3092\u4F5C\u3063\u3066\u3044\u305F\u306E\u3092\u898B\u3064\u3051\u305F\u3002\u9759\u304B\u306B\u305D\u3053\u3092\u901A\u308A\u62C5\u308E\u3046\u3068\u3059\u308B\u3068\u5287\u7684\u306A\u76D7\u8CCA\u6226\u3068\u306A\u3063\u305F\u3002",
            // Day 10 – Greybeards
            "\u9AD8\u9AD8\u306E\u9AD8\u6A4B\u306E\u9069\u8005\u305F\u3061\u306B\u4F1A\u3044\u306B\u884C\u3063\u305F\u3002\u9577\u3044\u9677\u6BB5\u3092\u767B\u308A\u5207\u308B\u306E\u306F\u8F9B\u304B\u3063\u305F\u3002\u767D\u9DF9\u9AD8\u9F62\u8005\u305F\u3061\u306F\u7121\u8A00\u3060\u3063\u305F\u304C\u3001\u305D\u306E\u76EE\u306F\u96F7\u9CF4\u308A\u306E\u308F\u305F\u3057\u3092\u898B\u629C\u3044\u3066\u3044\u305F\u3002\u30C9\u30E9\u30B4\u30F3\u30DC\u30FC\u30F3\u306E\u6559\u3048\u3092\u7A4D\u6975\u7684\u306B\u5B66\u3093\u3060\u3002",
            // Day 11 – Bleak Falls Barrow
            "\u30D6\u30EA\u30FC\u30AF\u30D5\u30A9\u30FC\u30EB\u30BA\u30D0\u30ED\u30A6\u3092\u63A2\u7D22\u3057\u305F\u3002\u9B5A\u9B5A\u305F\u308B\u52A0\u9663\u8DEF\u30C9\u30EC\u30A6\u30B0\u30EB\u304C\u6B69\u5C0E\u304F\u5148\u306B\u5DE8\u5927\u306A\u30C7\u30A1\u30A6\u30B0\u30EB\u30EA\u30FC\u30C1\u304C\u62D2\u3093\u3067\u3044\u305F\u3002\u9F8D\u306E\u7480\u59B3\u30A6\u30A9\u30FC\u30EB\u304B\u3089\u300C\u529B\u3092\u5570\u3048\u308D\u300D\u3068\u58F0\u304C\u9CF4\u308A\u97FF\u3044\u305F\u3002",
            // Day 12 – forging armor
            "\u9271\u51B6\u5C4B\u3067\u521D\u3081\u3066\u9271\u9244\u81EA\u4F5C\u306E\u30D6\u30EC\u30FC\u30B9\u30D7\u30EC\u30FC\u30C8\u3092\u935B\u3048\u305F\u3002\u6DF1\u591C\u306B\u53CA\u3076\u307E\u3067\u9271\u3092\u698E\u304D\u7D9A\u3051\u3001\u5C71\u8F2A\u306B\u6C17\u6301\u3061\u826F\u3044\u765D\u614B\u304C\u3067\u304D\u305F\u3002\u6B21\u306F\u9271\u9244\u306E\u5263\u306E\u935B\u9020\u306B\u6311\u6226\u3057\u305F\u3044\u3002",
            // Day 13 – speaking to Aela
            "\u30A8\u30FC\u30E9\u304C\u6CE3\u30FC\u30C8\u30E9\u30F3\u306E\u5C71\u307E\u306E\u30A6\u30A7\u30A2\u30A6\u30EB\u30D5\u306B\u3064\u3044\u3066\u8A71\u3057\u3066\u304F\u308C\u305F\u3002\u5F7C\u5973\u306E\u8A9E\u308A\u53E3\u306F\u7C21\u6F54\u3067\u3001\u96D1\u98F2\u6FF7\u304C\u6FA2\u308C\u3066\u3044\u305F\u3002\u6C11\u8A71\u306B\u306F\u5FC5\u305A\u771F\u5B9F\u304C\u542B\u307E\u308C\u308B\u3068\u5F7C\u5973\u306F\u8F9B\u3089\u3063\u305F\u8868\u60C5\u3067\u8A00\u3063\u305F\u3002",
            // Day 14 – civil war scouts
            "\u5185\u6226\u306E\u65A5\u5175\u9A0E\u99AC\u961F\u3068\u9053\u3067\u3059\u308C\u9055\u3063\u305F\u3002\u4E21\u8ECD\u3068\u3082\u305F\u3060\u306E\u65C5\u4EBA\u306E\u308F\u305F\u3057\u306B\u306F\u95A2\u5FC3\u3092\u793A\u3055\u306A\u304B\u3063\u305F\u3002\u6226\u4E71\u306F\u9060\u6182\u3067\u306F\u306A\u304F\u8FBA\u308A\u306E\u6751\u3005\u306B\u307E\u3067\u8FEB\u3063\u3066\u3044\u308B\u3068\u8033\u306B\u3057\u305F\u3002\u7D9A\u304F\u6CA5\u6765\u306E\u5927\u5275\u304C\u4E0D\u5B89\u3092\u8A98\u3046\u3002",
            // Day 15 – reflection
            "\u9577\u3044\u65CC\u306E\u5F8C\u3001\u6B66\u5177\u5C4B\u306B\u6A2A\u305F\u308F\u308A\u65E5\u8A18\u3092\u6E96\u3081\u305F\u3002\u5341\u4E94\u65E5\u9593\u306E\u5197\u9577\u306A\u65C5\u304C\u4E00\u518A\u306E\u672C\u306B\u3057\u307E\u308B\u306A\u3001\u3068\u601D\u3046\u3068\u5947\u5999\u306A\u6C17\u5206\u3060\u3002\u6B21\u306E\u51A0\u96EA\u5730\u306E\u5D26\u3067\u3082\u3001\u3053\u306E\u65E5\u8A18\u3092\u5FC3\u306E\u62E0\u308A\u6240\u306B\u3057\u3066\u5C71\u3092\u8089\u7C3F\u3059\u308B\u3060\u308D\u3046\u3002"
        };

        std::vector<SkyrimNetDiaries::DiaryEntry> entries;
        entries.reserve(15);

        for (int i = 0; i < 15; ++i) {
            SkyrimNetDiaries::DiaryEntry e;
            e.actor_uuid      = uuid;
            e.actor_name      = playerName;
            e.content         = kContent[i];
            e.entry_date      = baseTime - static_cast<double>(14 - i) * 86400.0;
            e.creation_time   = e.entry_date;
            e.location        = "\u30B9\u30AB\u30A4\u30EA\u30E0";  // スカイリム
            e.emotion         = "\u969C\u5BB3\u30C6\u30B9\u30C8";  // テスト用
            e.importance_score = 0.8;
            entries.push_back(std::move(e));
        }

        SKSE::log::info("[PapyrusAPI] Creating 15 Japanese test diary entries for actor {:08X} ({})",
            playerFormId, playerName);

        CreateAllVolumesForActor(uuid, playerName, playerFormId, "", std::move(entries), 1);
        return true;
    }

    bool RegisterFunctions(RE::BSScript::IVirtualMachine* a_vm) {
        if (!a_vm) {
            SKSE::log::error("Failed to register Papyrus functions - VM is null");
            return false;
        }

        a_vm->RegisterFunction("ApplyStolenDiaryEffect", "PhysicalDiaryAPI", ApplyStolenDiaryEffect);
        a_vm->RegisterFunction("RemoveStolenDiaryEffect", "PhysicalDiaryAPI", RemoveStolenDiaryEffect);
        a_vm->RegisterFunction("UpdateDiaryForActor", "SkyrimNetDiaries_Native", UpdateDiaryForActorWrapper);
        a_vm->RegisterFunction("GetStolenFaction",    "SkyrimNetDiaries_Native", GetStolenFaction);
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
        a_vm->RegisterFunction("GenerateJapaneseTestDiaries", "SkyrimNetDiaries_MCM", MCM_GenerateJapaneseTestDiaries);

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
