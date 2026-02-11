#include "PapyrusAPI.h"

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

    bool RegisterFunctions(RE::BSScript::IVirtualMachine* a_vm) {
        if (!a_vm) {
            SKSE::log::error("Failed to register Papyrus functions - VM is null");
            return false;
        }

        a_vm->RegisterFunction("ApplyStolenDiaryEffect", "PhysicalDiaryAPI", ApplyStolenDiaryEffect);
        a_vm->RegisterFunction("RemoveStolenDiaryEffect", "PhysicalDiaryAPI", RemoveStolenDiaryEffect);

        SKSE::log::info("Registered Physical Diary Papyrus API functions: ApplyStolenDiaryEffect, RemoveStolenDiaryEffect");
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
