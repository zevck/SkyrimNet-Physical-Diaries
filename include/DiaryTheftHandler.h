#pragma once

namespace DiaryTheftHandler {
    // Register the event handler for diary theft detection
    void Register();
    
    // Verify ESP setup on startup (called automatically by Register)
    void VerifyESPSetup();
}
