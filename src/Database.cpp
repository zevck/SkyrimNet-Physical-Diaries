/*
 * SkyrimNet Physical Diaries - a Skyrim SKSE plugin that turns SkyrimNet NPC
 * diary entries into books you can find and read in the world.
 * Copyright (C) 2026 Zevick
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

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
        return PublicIsMemorySystemReady();
    }

    std::vector<DiaryEntry> Database::ParseDiaryJSON(const std::string& jsonResponse, bool* ok) {
        std::vector<DiaryEntry> entries;
        if (ok) *ok = true;

        if (jsonResponse.empty()) {  // SkyrimNet always answers with a JSON array
            if (ok) *ok = false;
            return entries;
        }
        if (jsonResponse == "[]") {
            return entries;
        }

        try {
            auto jsonArray = json::parse(jsonResponse);

            if (!jsonArray.is_array()) {
                SKSE::log::error("Expected JSON array from API, got: {}", jsonResponse.substr(0, 100));
                if (ok) *ok = false;
                return entries;
            }

            for (const auto& item : jsonArray) {
                DiaryEntry entry;

                if (item.contains("id") && item["id"].is_number_integer()) {
                    entry.id = item["id"].get<int>();
                }

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

                if (item.contains("tags") && item["tags"].is_array()) {
                    for (const auto& tag : item["tags"]) {
                        if (tag.is_string()) entry.tags.push_back(tag.get<std::string>());
                    }
                }

                entries.push_back(entry);
            }

            SKSE::log::debug("Parsed {} diary entries from API JSON", entries.size());

        } catch (const json::exception& e) {
            if (ok) *ok = false;
            SKSE::log::error("JSON parsing error: {}", e.what());
            SKSE::log::error("JSON response (first 500 chars): {}", jsonResponse.substr(0, 500));
        }

        return entries;
    }

    std::vector<DiaryEntry> Database::GetDiaryEntries(uint32_t formId, int limit, double startTime, double endTime, bool* ok) {
        if (ok) *ok = false;  // until the query has succeeded
        try {
            if (!api_initialized_ && !InitializeAPI()) {
                SKSE::log::error("API not initialized - cannot get diary entries");
                return {};
            }

            SKSE::log::debug("Calling PublicGetDiaryEntries(formId=0x{:X}, limit={}, startTime={:.2f}, endTime={:.2f})",
                            formId, limit, startTime, endTime);

            std::string jsonResponse = PublicGetDiaryEntries(formId, limit, startTime, endTime);

            bool parsed = true;
            auto entries = ParseDiaryJSON(jsonResponse, &parsed);
            if (ok) *ok = parsed;

            // Oldest first, matching the volume-splitting order in CreateAllVolumesForActor.
            std::sort(entries.begin(), entries.end(), EntryOlder);

            // Client-side enforcement of time bounds.
            if (startTime > 0.0) {
                entries.erase(std::remove_if(entries.begin(), entries.end(),
                    [startTime](const DiaryEntry& e) { return e.entry_date < startTime; }), entries.end());
            }
            if (endTime > 0.0) {
                entries.erase(std::remove_if(entries.begin(), entries.end(),
                    [endTime](const DiaryEntry& e) { return e.entry_date > endTime; }), entries.end());
            }

            SKSE::log::debug("Retrieved {} diary entries for FormID 0x{:X}", entries.size(), formId);

            return entries;
        } catch (const std::exception& e) {
            SKSE::log::error("GetDiaryEntries exception: {}", e.what());
        } catch (...) {
            SKSE::log::error("GetDiaryEntries: unknown exception");
        }
        return {};
    }

    std::vector<DiaryEntry> Database::GetVolumeEntries(uint32_t formId, const VolumeBounds& bounds, bool* ok) {
        // Fetch the whole range: SkyrimNet's limit keeps the newest entries, which would
        // drop the volume's oldest ones.
        auto entries = GetDiaryEntries(formId, kFetchAllEntries, bounds.startTime, bounds.endTime, ok);

        // Exclude the previous volume's entries on a shared boundary date: exactly
        // prevCountAtBoundary of them (entry_date <= startTime and creation_time <=
        // prevLastCreationTime), in sorted order, so two identical entries are never
        // both removed.
        if (bounds.prevLastCreationTime > 0.0 && bounds.prevCountAtBoundary > 0) {
            int toRemove = bounds.prevCountAtBoundary;
            auto it = entries.begin();
            while (it != entries.end() && toRemove > 0) {
                if (it->entry_date <= bounds.startTime && it->creation_time <= bounds.prevLastCreationTime) {
                    it = entries.erase(it);
                    --toRemove;
                } else {
                    ++it;
                }
            }
        }

        // The mirror of the removal above: on the date the
        // next volume starts, keep only the nextPrevCountAtBoundary entries it recorded
        // as belonging here; the rest are the next volume's.
        if (bounds.endTime > 0.0 && bounds.nextPrevCountAtBoundary > 0) {
            int keep = bounds.nextPrevCountAtBoundary;
            for (auto it = entries.begin(); it != entries.end();) {
                if (it->entry_date < bounds.nextStartTime) {
                    ++it;
                } else if (keep > 0 && it->creation_time <= bounds.nextPrevLastCreationTime) {
                    --keep;
                    ++it;
                } else {
                    it = entries.erase(it);
                }
            }
        }
        return entries;
    }

    bool Database::RegisterDecorator(const char* name, const char* description,
                                     std::function<std::string(RE::Actor*)> callback) {
        if (!api_initialized_ && !InitializeAPI()) return false;
        if (!PublicRegisterDecorator) {
            SKSE::log::error("SkyrimNet's API has no decorator registration (needs API v5+) — '{}' not registered", name);
            return false;
        }
        if (!PublicRegisterDecorator(name, description, std::move(callback))) {
            SKSE::log::error("SkyrimNet refused decorator '{}' (name already taken?)", name);
            return false;
        }
        return true;
    }

    double Database::GetPlayerLastEventTime() {
        try {
            if (!api_initialized_ && !InitializeAPI()) return 0.0;
            if (!PublicGetRecentEvents) return 0.0;
            // Same query as SkyrimNet's own continuity check: the player's latest event.
            const auto events = json::parse(PublicGetRecentEvents(0x14, 1, ""), nullptr, false);
            if (!events.is_array() || events.empty()) return 0.0;
            const auto& latest = events.front();
            if (!latest.contains("gameTime") || !latest["gameTime"].is_number()) return 0.0;
            return latest["gameTime"].get<double>();
        } catch (const std::exception& e) {
            SKSE::log::error("GetPlayerLastEventTime exception: {}", e.what());
            return 0.0;
        }
    }

    bool Database::UpdateDiaryEntry(int entryId, const std::string& content, const std::string& tagsCSV) {
        try {
            if (!api_initialized_ && !InitializeAPI()) return false;
            if (!PublicUpdateDiaryEntry) {
                SKSE::log::warn("SkyrimNet has no PublicUpdateDiaryEntry (needs public API v11)");
                return false;
            }
            return PublicUpdateDiaryEntry(entryId, content.c_str(), tagsCSV.c_str());
        } catch (const std::exception& e) {
            SKSE::log::error("PublicUpdateDiaryEntry({}) threw: {}", entryId, e.what());
            return false;
        }
    }

    bool Database::DeleteDiaryEntry(int entryId) {
        try {
            if (!api_initialized_ && !InitializeAPI()) return false;
            if (!PublicDeleteDiaryEntry) {
                SKSE::log::warn("SkyrimNet has no PublicDeleteDiaryEntry (needs public API v11)");
                return false;
            }
            return PublicDeleteDiaryEntry(entryId);
        } catch (const std::exception& e) {
            SKSE::log::error("PublicDeleteDiaryEntry({}) threw: {}", entryId, e.what());
            return false;
        }
    }

    std::string Database::GetBioTemplateName(uint32_t formId) {
        try {
            if (!api_initialized_ && !InitializeAPI()) {
                return "";
            }

            std::string templateName = PublicGetBioTemplateName(formId);

            if (!templateName.empty()) {
                SKSE::log::debug("Bio template for 0x{:X}: {}", formId, templateName);
            }

            return templateName;
        } catch (const std::exception& e) {
            SKSE::log::error("GetBioTemplateName exception: {}", e.what());
        } catch (...) {
            SKSE::log::error("GetBioTemplateName: unknown exception");
        }
        return "";
    }

    // UUID ↔ FormID conversion
    std::string Database::GetUUIDFromFormID(uint32_t formId) {
        try {
            if (!api_initialized_ && !InitializeAPI()) {
                return "";
            }

            if (!PublicFormIDToUUID) {
                return "";
            }

            // "" when SkyrimNet doesn't know the actor (it returns 0).
            const uint64_t uuid = PublicFormIDToUUID(formId);
            return uuid == 0 ? std::string{} : std::to_string(uuid);
        } catch (const std::exception& e) {
            SKSE::log::error("GetUUIDFromFormID exception: {}", e.what());
        } catch (...) {
            SKSE::log::error("GetUUIDFromFormID: unknown exception");
        }
        return "";
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
