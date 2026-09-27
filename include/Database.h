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

    class Database {
    public:
        Database() = default;
        ~Database() = default;

        // Initialize the SkyrimNet API
        static bool InitializeAPI();
        
        // Check if SkyrimNet memory system is ready
        static bool IsMemorySystemReady();
        
        // Query diary entries for a specific actor FormID (using API).
        // prevVolumeLastCreationTime: when > 0, entries with creation_time <= this value are excluded
        // (prevents the last entry of the previous volume from appearing in this volume when they
        // share the same entry_date at the boundary).
        static std::vector<DiaryEntry> GetDiaryEntries(uint32_t formId, int limit = 10000,
                                                        double startTime = 0.0, double endTime = 0.0,
                                                        double prevVolumeLastCreationTime = 0.0,
                                                        int prevVolumeCountAtBoundary = 0);
        
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
