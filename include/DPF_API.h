#pragma once

#include "RE/Skyrim.h"
#include "SKSE/SKSE.h"

// Direct C++ API for Dynamic Persistent Forms
// This allows us to call DPF functions directly without going through Papyrus VM

namespace DPF {
    // Create a persistent form based on a template
    // Returns the new persistent form, or nullptr on failure
    RE::TESForm* Create(RE::TESForm* baseTemplate);
    
    // Track an existing form to make it persistent
    void Track(RE::TESForm* form);
    
    // Dispose a persistent form (removes from DPF tracking and marks for deletion)
    // Call this to prevent DPF from serializing forms that might be in an invalid state
    void Dispose(RE::TESForm* form);
}
