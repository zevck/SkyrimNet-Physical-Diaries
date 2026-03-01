#include "Database.h"
#include "SkyrimNetPublicAPI.h"
#include <sstream>

using json = nlohmann::json;

namespace SkyrimNetDiaries {

    bool Database::InitializeAPI() {
        if (api_initialized_) {
            return true;
        }

        SKSE::log::info("Initializing SkyrimNet Public API...");
        
        if (!FindFunctions()) {
            SKSE::log::error("Failed to load SkyrimNet.dll - API not available");
            return false;
        }

        if (!PublicGetVersion) {
            SKSE::log::error("PublicGetVersion not available");
            return false;
        }

        int version = PublicGetVersion();
        SKSE::log::info("SkyrimNet API version: {}", version);

        if (version < 4) {
            SKSE::log::error("SkyrimNet API v4+ required for diary support (found v{})", version);
            return false;
        }

        if (!PublicGetDiaryEntries || !PublicIsMemorySystemReady || !PublicGetBioTemplateName) {
            SKSE::log::error("Required API functions not available");
            return false;
        }

        api_initialized_ = true;
        SKSE::log::info("✓ SkyrimNet API initialized successfully");
        return true;
    }

    bool Database::IsMemorySystemReady() {
        if (!api_initialized_ && !InitializeAPI()) {
            return false;
        }

        if (!PublicIsMemorySystemReady) {
            return false;
        }

        return PublicIsMemorySystemReady();
    }

    std::vector<DiaryEntry> Database::ParseDiaryJSON(const std::string& jsonResponse) {
        std::vector<DiaryEntry> entries;

        if (jsonResponse.empty() || jsonResponse == "[]") {
            return entries;
        }

        try {
            auto jsonArray = json::parse(jsonResponse);

            if (!jsonArray.is_array()) {
                SKSE::log::error("Expected JSON array from API, got: {}", jsonResponse.substr(0, 100));
                return entries;
            }

            for (const auto& item : jsonArray) {
                DiaryEntry entry;
                
                // Required fields
                if (item.contains("actor_uuid") && item["actor_uuid"].is_number()) {
                    entry.actor_uuid = std::to_string(item["actor_uuid"].get<uint64_t>());
                }
                
                if (item.contains("actor_name") && item["actor_name"].is_string()) {
                    entry.actor_name = item["actor_name"].get<std::string>();
                }
                
                if (item.contains("content") && item["content"].is_string()) {
                    entry.content = item["content"].get<std::string>();
                }
                
                if (item.contains("entry_date") && item["entry_date"].is_number()) {
                    entry.entry_date = item["entry_date"].get<double>();
                }
                
                if (item.contains("creation_time") && item["creation_time"].is_number()) {
                    entry.creation_time = item["creation_time"].get<double>();
                }
                
                // Optional fields
                if (item.contains("location") && item["location"].is_string()) {
                    entry.location = item["location"].get<std::string>();
                }
                
                if (item.contains("emotion") && item["emotion"].is_string()) {
                    entry.emotion = item["emotion"].get<std::string>();
                }
                
                if (item.contains("importance_score") && item["importance_score"].is_number()) {
                    entry.importance_score = item["importance_score"].get<double>();
                }

                entries.push_back(entry);
            }

            SKSE::log::info("Parsed {} diary entries from API JSON", entries.size());

        } catch (const json::exception& e) {
            SKSE::log::error("JSON parsing error: {}", e.what());
            SKSE::log::error("JSON response (first 500 chars): {}", jsonResponse.substr(0, 500));
        }

        return entries;
    }

    std::vector<DiaryEntry> Database::GetDiaryEntries(uint32_t formId, int limit, double startTime, double endTime) {
        if (!api_initialized_ && !InitializeAPI()) {
            SKSE::log::error("API not initialized - cannot get diary entries");
            return {};
        }

        if (!PublicGetDiaryEntries) {
            SKSE::log::error("PublicGetDiaryEntries not available");
            return {};
        }

        SKSE::log::info("Calling PublicGetDiaryEntries(formId=0x{:X}, limit={}, startTime={:.2f}, endTime={:.2f})",
                       formId, limit, startTime, endTime);

        std::string jsonResponse = PublicGetDiaryEntries(formId, limit, startTime, endTime);
        
        auto entries = ParseDiaryJSON(jsonResponse);
        
        SKSE::log::info("Retrieved {} diary entries for FormID 0x{:X}", entries.size(), formId);
        
        return entries;
    }

    std::vector<DiaryEntry> Database::GetAllDiaryEntries(int limit) {
        // Use formId=0 to get entries from ALL actors
        return GetDiaryEntries(0, limit, 0.0, 0.0);
    }

    std::string Database::GetBioTemplateName(uint32_t formId) {
        if (!api_initialized_ && !InitializeAPI()) {
            return "";
        }

        if (!PublicGetBioTemplateName) {
            return "";
        }

        std::string templateName = PublicGetBioTemplateName(formId);
        
        if (!templateName.empty()) {
            SKSE::log::info("Bio template for 0x{:X}: {}", formId, templateName);
        }
        
        return templateName;
    }

    uint32_t Database::GetFormIDByName(const std::string& actorName) {
        if (!api_initialized_ && !InitializeAPI()) {
            return 0;
        }

        // Use engagement API to find actor by name
        if (!PublicGetActorEngagement) {
            return 0;
        }

        // Get top 100 actors and search for matching name
        std::string engagementJson = PublicGetActorEngagement(100, false, false, 86400.0, 604800.0);
        
        try {
            auto jsonArray = json::parse(engagementJson);
            
            if (jsonArray.is_array()) {
                for (const auto& item : jsonArray) {
                    if (item.contains("name") && item.contains("formId")) {
                        std::string name = item["name"].get<std::string>();
                        if (name == actorName) {
                            return item["formId"].get<uint32_t>();
                        }
                    }
                }
            }
        } catch (const json::exception& e) {
            SKSE::log::error("Error parsing engagement JSON: {}", e.what());
        }

        return 0;
    }

    // UUID ↔ FormID conversion
    std::string Database::GetUUIDFromFormID(uint32_t formId) {
        if (!api_initialized_ && !InitializeAPI()) {
            return "";
        }

        if (!PublicFormIDToUUID) {
            return "";
        }

        uint64_t uuid = PublicFormIDToUUID(formId);
        return std::to_string(uuid);
    }

    uint32_t Database::GetFormIDForUUID(const std::string& uuid) {
        if (!api_initialized_ && !InitializeAPI()) {
            return 0;
        }

        if (!PublicUUIDToFormID) {
            return 0;
        }

        try {
            uint64_t uuidNum = std::stoull(uuid);
            return PublicUUIDToFormID(uuidNum);
        } catch (...) {
            SKSE::log::error("Invalid UUID string: {}", uuid);
            return 0;
        }
    }

    // Actor name lookup by UUID
    std::string Database::GetActorName(const std::string& uuid) {
        if (!api_initialized_ && !InitializeAPI()) {
            return "";
        }

        if (!PublicGetActorNameByUUID) {
            return "";
        }

        try {
            uint64_t uuidNum = std::stoull(uuid);
            return PublicGetActorNameByUUID(uuidNum);
        } catch (...) {
            SKSE::log::error("Invalid UUID string: {}", uuid);
            return "";
        }
    }

    // Get bio template name by UUID
    std::string Database::GetTemplateNameByUUID(const std::string& uuid) {
        uint32_t formId = GetFormIDForUUID(uuid);
        if (formId == 0) {
            return "";
        }
        return GetBioTemplateName(formId);
    }

    // Get UUID by bio template name
    std::string Database::GetUUIDByTemplateName(const std::string& templateName) {
        if (!api_initialized_ && !InitializeAPI()) {
            return "";
        }

        // Strategy: Get all diary entries, find first entry with matching template name
        // This is not efficient but necessary without a direct API
        auto allEntries = GetAllDiaryEntries(1000);
        
        for (const auto& entry : allEntries) {
            // Get FormID for this entry's UUID
            uint32_t formId = GetFormIDForUUID(entry.actor_uuid);
            if (formId != 0) {
                std::string entryTemplate = GetBioTemplateName(formId);
                if (entryTemplate == templateName) {
                    SKSE::log::info("Found UUID for template '{}': {}", templateName, entry.actor_uuid);
                    return entry.actor_uuid;
                }
            }
        }

        SKSE::log::warn("No UUID found for template name: {}", templateName);
        return "";
    }

    // Query diary entries by UUID
    std::vector<DiaryEntry> Database::GetDiaryEntries(const std::string& uuid, int limit, double startTime) {
        uint32_t formId = GetFormIDForUUID(uuid);
        if (formId == 0) {
            SKSE::log::error("Cannot convert UUID {} to FormID", uuid);
            return {};
        }
        
        // Call FormID version (endTime = 0.0 means no end limit)
        return GetDiaryEntries(formId, limit, startTime, 0.0);
    }

    // Get actors with new diary entries since a timestamp
    std::unordered_map<std::string, std::string> Database::GetActorUUIDsWithNewEntries(double sinceTimestamp) {
        std::unordered_map<std::string, std::string> actorsWithNewEntries;

        if (!api_initialized_ && !InitializeAPI()) {
            return actorsWithNewEntries;
        }

        // Get all diary entries and filter by timestamp
        auto allEntries = GetAllDiaryEntries(10000);
        
        for (const auto& entry : allEntries) {
            // Check if entry is newer than timestamp
            if (entry.entry_date > sinceTimestamp) {
                // Add to map (UUID → actor name)
                actorsWithNewEntries[entry.actor_uuid] = entry.actor_name;
            }
        }

        SKSE::log::info("Found {} actors with entries since timestamp {:.2f}", 
                       actorsWithNewEntries.size(), sinceTimestamp);
        
        return actorsWithNewEntries;
    }

    // OPTIMIZED: Fetch all entries once and partition by actor
    std::unordered_map<std::string, std::vector<DiaryEntry>> Database::GetAllDiaryEntriesGroupedByActor(double sinceTimestamp) {
        std::unordered_map<std::string, std::vector<DiaryEntry>> entriesByActor;

        if (!api_initialized_ && !InitializeAPI()) {
            return entriesByActor;
        }

        // Fetch ALL diary entries from API in one call
        auto allEntries = GetAllDiaryEntries(10000);
        
        // Partition entries by actor UUID locally (avoids N API calls)
        for (const auto& entry : allEntries) {
            if (entry.entry_date > sinceTimestamp) {
                entriesByActor[entry.actor_uuid].push_back(entry);
            }
        }

        SKSE::log::info("Grouped {} diary entries across {} actors (since timestamp {:.2f})", 
                       allEntries.size(), entriesByActor.size(), sinceTimestamp);
        
        return entriesByActor;
    }

} // namespace SkyrimNetDiaries
