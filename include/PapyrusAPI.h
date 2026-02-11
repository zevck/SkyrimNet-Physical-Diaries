#pragma once

#include "PCH.h"

namespace PapyrusAPI {
    // Register all native Papyrus functions
    void Register();
    
    // Apply the SNPD_DiaryStolen magic effect to an actor
    // Returns true on success, false on failure
    bool ApplyStolenDiaryEffect(RE::StaticFunctionTag*, RE::Actor* akActor);
    
    // Remove the SNPD_DiaryStolen magic effect from an actor
    // Returns true on success, false on failure
    bool RemoveStolenDiaryEffect(RE::StaticFunctionTag*, RE::Actor* akActor);
}
