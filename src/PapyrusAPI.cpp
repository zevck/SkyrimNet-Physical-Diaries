#include "PapyrusAPI.h"
#include "BookManager.h"
#include "Config.h"

// Forward declarations from main.cpp (defined in global namespace)
extern void UpdateDiaryForActorInternal(RE::FormID formId);
extern int  ResetAllDiariesInternal();

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
            SKSE::log::info("{} already has SNPD_DiaryStorageSpell", akActor->GetName());
            return true; // Not an error, just already applied
        }

        // Add the spell (ability) to the actor
        akActor->AddSpell(spell);
        
        SKSE::log::info("Applied SNPD_DiaryStorageSpell to {} (FormID: 0x{:X})", 
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
            SKSE::log::info("{} doesn't have SNPD_DiaryStorageSpell (already removed or never applied)", akActor->GetName());
            return true; // Not an error, just not present
        }

        // Remove the spell (ability) from the actor
        akActor->RemoveSpell(spell);
        
        SKSE::log::info("Removed SNPD_DiaryStorageSpell from {} (FormID: 0x{:X}) - diary was returned", 
                       akActor->GetName(), akActor->GetFormID());
        return true;
    }
    
    void UpdateDiaryForActorWrapper(RE::StaticFunctionTag*, std::int32_t formId) {
        SKSE::log::info("[PapyrusAPI] UpdateDiaryForActor called with FormID 0x{:X}", formId);
        ::UpdateDiaryForActorInternal(static_cast<RE::FormID>(formId));
    }

    // -------------------------------------------------------------------------
    // MCM Maintenance natives
    // -------------------------------------------------------------------------

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

    bool RegisterFunctions(RE::BSScript::IVirtualMachine* a_vm) {
        if (!a_vm) {
            SKSE::log::error("Failed to register Papyrus functions - VM is null");
            return false;
        }

        a_vm->RegisterFunction("ApplyStolenDiaryEffect", "PhysicalDiaryAPI", ApplyStolenDiaryEffect);
        a_vm->RegisterFunction("RemoveStolenDiaryEffect", "PhysicalDiaryAPI", RemoveStolenDiaryEffect);
        a_vm->RegisterFunction("UpdateDiaryForActor", "SkyrimNetDiaries_Native", UpdateDiaryForActorWrapper);

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
