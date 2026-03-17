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
    
    // Get the theft status of an actor's diary as JSON string
    // Returns JSON like: {"stolen": true, "chronicled": false} or {"stolen": false}
    RE::BSFixedString GetDiaryTheftStatus(RE::StaticFunctionTag*, RE::Actor* akActor);
    
    // Check if an actor's diary is currently stolen (simple boolean check)
    // Returns "true" if stolen and not yet resolved, "false" otherwise
    RE::BSFixedString IsDiaryStolen(RE::StaticFunctionTag*, RE::Actor* akActor);
    
    // Record that an actor has chronicled their stolen diary (clears theft state)
    // Call this when an NPC writes a diary entry
    void SetTheftCleared(RE::StaticFunctionTag*, RE::Actor* akActor);

    // MCM debug test: read all volumes from an external DiaryDB file, log the full
    // book_text for each volume, and create test books in the player's inventory.
    // Hardcoded to C:\Users\zevic\OneDrive\Documents\kibech diary\diary.db
    bool MCM_TestBooksFromExternalDB(RE::StaticFunctionTag*);
}
