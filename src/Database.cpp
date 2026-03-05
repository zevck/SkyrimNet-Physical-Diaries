#include "Database.h"
#include "SkyrimNetPublicAPI.h"
#include <algorithm>
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

        SKSE::log::debug("Calling PublicGetDiaryEntries(formId=0x{:X}, limit={}, startTime={:.2f}, endTime={:.2f})",
                        formId, limit, startTime, endTime);

        std::string jsonResponse = PublicGetDiaryEntries(formId, limit, startTime, endTime);
        
        auto entries = ParseDiaryJSON(jsonResponse);

        // Ensure oldest-first order regardless of what the API returns.
        std::sort(entries.begin(), entries.end(),
                  [](const DiaryEntry& a, const DiaryEntry& b) { return a.entry_date < b.entry_date; });

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

} // namespace SkyrimNetDiaries
