#pragma once

#include "PCH.h"

namespace SkyrimNetDiaries {

    struct DiaryEntry {
        std::string actor_uuid;
        std::string actor_name;
        std::string content;
        double entry_date;
        double creation_time;
        std::string location;
        std::string emotion;
        double importance_score;
    };

    class Database {
    public:
        Database() = default;
        ~Database();

        bool Open(const std::string& dbPath, bool readOnly = true);
        void Close();

        // Query diary entries for a specific actor UUID
        std::vector<DiaryEntry> GetDiaryEntries(const std::string& actorUUID, int limit = 100, double startTime = 0.0, double endTime = 0.0);
        
        // Get actor name from UUID
        std::string GetActorName(const std::string& actorUUID);

        // Get all diary entries (for testing)
        std::vector<DiaryEntry> GetAllDiaryEntries(int limit = 10);

        // Log contents of counters table
        void LogCountersTable();

        // Get the player's UUID from the database
        std::string GetPlayerUUID();

        // Get the player's name from the database
        std::string GetPlayerName();

        // Check if database contains a specific player UUID
        bool HasPlayerUUID(const std::string& playerUuid);

        // Get all unique actor UUIDs that have diary entries
        std::vector<std::string> GetAllActorUUIDs();
        
        // Get UUID by bio_template_name
        std::string GetUUIDByTemplateName(const std::string& templateName);
        
        // Get bio_template_name by UUID
        std::string GetTemplateNameByUUID(const std::string& actorUUID);

        // Get actors with new diary entries since a given time
        std::vector<std::pair<std::string, std::string>> GetActorUUIDsWithNewEntries(double sinceTime);

        // Get FormID for an actor UUID
        RE::FormID GetFormIDForUUID(const std::string& actorUUID);
        
        // Enable WAL mode for concurrent access (call once per database)
        bool EnableWALMode();

    private:
        sqlite3* db_ = nullptr;
        bool is_open_ = false;
    };

} // namespace SkyrimNetDiaries
