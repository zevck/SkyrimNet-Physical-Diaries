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

    // Chronological order: entry_date, then creation_time (real-world write time)
    // to break ties between entries dated the same in-game moment.
    inline bool EntryOlder(const DiaryEntry& a, const DiaryEntry& b) {
        if (a.entry_date != b.entry_date) return a.entry_date < b.entry_date;
        return a.creation_time < b.creation_time;
    }

    // Limit that means "every entry" for GetDiaryEntries.
    inline constexpr int kFetchAllEntries = 10000;

    class Database {
    public:
        Database() = default;
        ~Database() = default;

        // Initialize the SkyrimNet API
        static bool InitializeAPI();
        
        // Check if SkyrimNet memory system is ready
        static bool IsMemorySystemReady();
        
        // Diary entries for a FormID (0 = every actor) within [startTime, endTime]
        // (0 = unbounded), returned oldest first.
        //
        // `limit` is applied by SkyrimNet to the NEWEST entries, so a limited query
        // returns the latest `limit` entries in the range, not the first.  Use
        // GetVolumeEntries whenever "the first N entries" matters.
        //
        // prevVolumeLastCreationTime / prevVolumeCountAtBoundary remove the previous
        // volume's entries that share this volume's first entry_date (see
        // docs/VOLUMES_AND_SYNC.md#volume-boundaries).
        static std::vector<DiaryEntry> GetDiaryEntries(uint32_t formId, int limit = kFetchAllEntries,
                                                        double startTime = 0.0, double endTime = 0.0,
                                                        double prevVolumeLastCreationTime = 0.0,
                                                        int prevVolumeCountAtBoundary = 0);

        // A volume's entries, oldest first: the whole [startTime, endTime] range minus
        // the previous volume's boundary entries, then capped to the first maxEntries
        // (0 = no cap).
        static std::vector<DiaryEntry> GetVolumeEntries(uint32_t formId, double startTime, double endTime,
                                                         double prevVolumeLastCreationTime,
                                                         int prevVolumeCountAtBoundary,
                                                         int maxEntries = 0);
        
        // Get bio template name for an actor FormID
        static std::string GetBioTemplateName(uint32_t formId);
        
        // UUID ↔ FormID conversion (using PublicAPI)
        static std::string GetUUIDFromFormID(uint32_t formId);
        static uint32_t GetFormIDForUUID(const std::string& uuid);
        
        // Actor name lookup by UUID
        static std::string GetActorName(const std::string& uuid);
        
        // Get bio template name by UUID (converts UUID → FormID → GetBioTemplateName)
        static std::string GetTemplateNameByUUID(const std::string& uuid);

    private:
        // Parse JSON response from PublicGetDiaryEntries into DiaryEntry structures
        static std::vector<DiaryEntry> ParseDiaryJSON(const std::string& jsonResponse);
        
        // Track if API has been initialized
        static inline bool api_initialized_ = false;
    };

} // namespace SkyrimNetDiaries
