#pragma once

namespace DiaryTheftHandler {
    // Register the event handler for diary theft detection
    void Register();
    
    // Verify ESP setup on startup (called automatically by Register)
    void VerifyESPSetup();
    
    // Clear the stolen diary marker from an actor (called when they generate a new diary entry)
    // Returns true if the marker was present and removed, false otherwise
    bool ClearStolenDiaryMarker(RE::Actor* actor);
}
