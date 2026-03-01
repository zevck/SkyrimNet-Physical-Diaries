#pragma once

#include "PCH.h"
#include <nlohmann/json.hpp>

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
        ~Database() = default;

        // Initialize the SkyrimNet API
        static bool InitializeAPI();
        
        // Check if SkyrimNet memory system is ready
        static bool IsMemorySystemReady();
        
        // Query diary entries for a specific actor FormID (using API)
        static std::vector<DiaryEntry> GetDiaryEntries(uint32_t formId, int limit = 10000, double startTime = 0.0, double endTime = 0.0);
        
        // Query diary entries by UUID (converts UUID → FormID → API call)
        static std::vector<DiaryEntry> GetDiaryEntries(const std::string& uuid, int limit = 10000, double startTime = 0.0);
        
        // Query ALL diary entries across all actors (using API with formId=0)
        static std::vector<DiaryEntry> GetAllDiaryEntries(int limit = 10000);
        
        // Get bio template name for an actor FormID
        static std::string GetBioTemplateName(uint32_t formId);
        
        // Get FormID from actor name (reverse lookup via engagement API)
        static uint32_t GetFormIDByName(const std::string& actorName);
        
        // UUID ↔ FormID conversion (using PublicAPI)
        static std::string GetUUIDFromFormID(uint32_t formId);
        static uint32_t GetFormIDForUUID(const std::string& uuid);
        
        // Actor name lookup by UUID
        static std::string GetActorName(const std::string& uuid);
        
        // Get bio template name by UUID (converts UUID → FormID → GetBioTemplateName)
        static std::string GetTemplateNameByUUID(const std::string& uuid);
        
        // Get UUID by bio template name (searches all actors)
        static std::string GetUUIDByTemplateName(const std::string& templateName);
        
        // Get actors with new diary entries since a timestamp
        // Returns map of UUID → actor name
        static std::unordered_map<std::string, std::string> GetActorUUIDsWithNewEntries(double sinceTimestamp);
        
        // OPTIMIZED: Get all diary entries grouped by actor UUID (fetches once, partitions locally)
        // Returns map of UUID → vector of diary entries
        // More efficient than calling GetDiaryEntries per actor when processing many actors
        static std::unordered_map<std::string, std::vector<DiaryEntry>> GetAllDiaryEntriesGroupedByActor(double sinceTimestamp = 0.0);

    private:
        // Parse JSON response from PublicGetDiaryEntries into DiaryEntry structures
        static std::vector<DiaryEntry> ParseDiaryJSON(const std::string& jsonResponse);
        
        // Track if API has been initialized
        static inline bool api_initialized_ = false;
    };

} // namespace SkyrimNetDiaries
