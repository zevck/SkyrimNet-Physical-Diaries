#include "PCH.h"
#include "SkyrimNetAPITest.h"
#include "Database.h"

namespace SkyrimNetAPITest {

    bool InitializeAPI() {
        SKSE::log::info("=== Testing SkyrimNet Public API ===");
        
        // Use Database class to initialize API
        if (!SkyrimNetDiaries::Database::InitializeAPI()) {
            SKSE::log::error("Failed to initialize SkyrimNet API");
            return false;
        }

        SKSE::log::info("SkyrimNet API initialized successfully");
        return true;
    }

    void TestMemorySystem() {
        SKSE::log::info("--- Testing Memory System ---");
        
        bool ready = SkyrimNetDiaries::Database::IsMemorySystemReady();
        SKSE::log::info("Memory system ready: {}", ready);
    }

    void TestUUIDResolution() {
        SKSE::log::info("--- Testing UUID Resolution ---");
        
        // Test with player FormID
        uint32_t playerFormID = 0x14;
        std::string playerUUID = SkyrimNetDiaries::Database::GetUUIDFromFormID(playerFormID);
        SKSE::log::info("Player FormID 0x{:X} -> UUID: {}", playerFormID, playerUUID);
        
        if (playerUUID != 0) {
            uint32_t resolvedFormID = PublicUUIDToFormID(playerUUID);
            SKSE::log::info("UUID {} -> FormID: 0x{:X}", playerUUID, resolvedFormID);
            
            std::string actorName = PublicGetActorNameByUUID(playerUUID);
            SKSE::log::info("UUID {} -> Name: {}", playerUUID, actorName);
        }
    }

    void TestBioTemplate(uint32_t formId) {
        SKSE::log::info("--- Testing Bio Template ---");
        
        if (!PublicGetBioTemplateName) {
            SKSE::log::warn("PublicGetBioTemplateName not available");
            return;
        }

        std::string templateName = PublicGetBioTemplateName(formId);
        SKSE::log::info("Actor 0x{:X} bio template: {}", formId, templateName.empty() ? "(none)" : templateName);
    }

    void TestMemoriesQuery(uint32_t formId, int maxCount) {
        SKSE::log::info("--- Testing Memories Query ---");
        
        if (!PublicGetMemoriesForActor) {
            SKSE::log::warn("PublicGetMemoriesForActor not available");
            return;
        }

        std::string memories = PublicGetMemoriesForActor(formId, maxCount, "");
        SKSE::log::info("Memories for actor 0x{:X} (max {}): {}", formId, maxCount, memories);
    }

    void TestRecentEvents(uint32_t formId, int maxCount) {
        SKSE::log::info("--- Testing Recent Events ---");
        
        if (!PublicGetRecentEvents) {
            SKSE::log::warn("PublicGetRecentEvents not available");
            return;
        }

        std::string events = PublicGetRecentEvents(formId, maxCount, "");
        SKSE::log::info("Recent events for actor 0x{:X} (max {}): {}", formId, maxCount, events);
    }

    void TestDialogueQuery(uint32_t formId, int maxExchanges) {
        SKSE::log::info("--- Testing Dialogue Query ---");
        
        if (!PublicGetRecentDialogue) {
            SKSE::log::warn("PublicGetRecentDialogue not available");
            return;
        }

        std::string dialogue = PublicGetRecentDialogue(formId, maxExchanges);
        SKSE::log::info("Recent dialogue for actor 0x{:X} (max {}): {}", formId, maxExchanges, dialogue);
    }

    void TestLatestDialogue() {
        SKSE::log::info("--- Testing Latest Dialogue Info ---");
        
        if (!PublicGetLatestDialogueInfo) {
            SKSE::log::warn("PublicGetLatestDialogueInfo not available");
            return;
        }

        std::string info = PublicGetLatestDialogueInfo();
        SKSE::log::info("Latest dialogue info: {}", info);
    }

    void TestActorEngagement() {
        SKSE::log::info("--- Testing Actor Engagement ---");
        
        if (!PublicGetActorEngagement) {
            SKSE::log::warn("PublicGetActorEngagement not available");
            return;
        }

        // Get top 5 actors, exclude player, show last 24h and 7d
        std::string engagement = PublicGetActorEngagement(5, true, true, 86400.0, 604800.0);
        SKSE::log::info("Actor engagement (top 5): {}", engagement);
    }

    void TestRelatedActors(uint32_t formId) {
        SKSE::log::info("--- Testing Related Actors ---");
        
        if (!PublicGetRelatedActors) {
            SKSE::log::warn("PublicGetRelatedActors not available");
            return;
        }

        std::string related = PublicGetRelatedActors(formId, 5, 86400.0, 604800.0);
        SKSE::log::info("Related actors for 0x{:X} (top 5): {}", formId, related);
    }

    void TestPlayerContext() {
        SKSE::log::info("--- Testing Player Context ---");
        
        if (!PublicGetPlayerContext) {
            SKSE::log::warn("PublicGetPlayerContext not available");
            return;
        }

        // Get context for last 24 game hours
        std::string context = PublicGetPlayerContext(24.0f);
        SKSE::log::info("Player context (24h): {}", context);
    }

    void TestDiaryQuery(uint32_t formId, int maxCount) {
        SKSE::log::info("--- Testing Diary Entries ---");
        
        if (!PublicGetDiaryEntries) {
            SKSE::log::warn("PublicGetDiaryEntries not available (requires API v4+)");
            return;
        }

        // Get all diary entries for the actor (or all actors if formId = 0)
        std::string entries = PublicGetDiaryEntries(formId, maxCount, 0.0, 0.0);
        
        if (formId == 0) {
            SKSE::log::info("Diary entries for ALL actors (max {}): {}", maxCount, entries);
        } else {
            SKSE::log::info("Diary entries for actor 0x{:X} (max {}): {}", formId, maxCount, entries);
        }

        // Test with time filtering - entries from last 24 hours
        auto now = std::chrono::system_clock::now();
        auto now_time = std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch()).count();
        auto day_ago = now_time - (24 * 3600);
        
        std::string recentEntries = PublicGetDiaryEntries(formId, maxCount, static_cast<double>(day_ago), static_cast<double>(now_time));
        SKSE::log::info("Recent diary entries (last 24h): {}", recentEntries);
    }

    void RunAllTests() {
        SKSE::log::info("======================================");
        SKSE::log::info("Running SkyrimNet API Tests");
        SKSE::log::info("======================================");

        if (!InitializeAPI()) {
            SKSE::log::error("API initialization failed - aborting tests");
            return;
        }

        TestMemorySystem();
        TestUUIDResolution();
        
        // Test with player
        uint32_t playerFormID = 0x14;
        TestBioTemplate(playerFormID);
        TestMemoriesQuery(playerFormID, 5);
        TestRecentEvents(playerFormID, 5);
        
        TestLatestDialogue();
        TestActorEngagement();
        TestRelatedActors(playerFormID);
        TestPlayerContext();
        TestDiaryQuery(playerFormID, 10);  // Test player's diary
        TestDiaryQuery(0, 20);              // Test all actors

        SKSE::log::info("======================================");
        SKSE::log::info("API Tests Complete");
        SKSE::log::info("======================================");
    }

    void TestSpecificActor(uint32_t formId) {
        SKSE::log::info("======================================");
        SKSE::log::info("Testing Actor 0x{:X}", formId);
        SKSE::log::info("======================================");

        if (!InitializeAPI()) {
            return;
        }

        TestUUIDResolution();
        TestBioTemplate(formId);
        TestMemoriesQuery(formId, 10);
        TestRecentEvents(formId, 10);
        TestDialogueQuery(formId, 5);
        TestRelatedActors(formId);
        TestDiaryQuery(formId, 15);

        SKSE::log::info("======================================");
    }

} // namespace SkyrimNetAPITest
