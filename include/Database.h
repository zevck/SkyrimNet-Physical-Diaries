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
        int id = 0;                    // SkyrimNet's entry id (0 if the response had none)
        std::string actor_uuid;
        std::string actor_name;
        std::string content;
        double entry_date = 0.0;
        double creation_time = 0.0;
        std::vector<std::string> tags;
    };

    // Tag SNPD adds to an entry the player edited in their diary: shown exactly as written.
    inline constexpr std::string_view kPlayerWrittenTag = "snpd_player_written";

    inline bool IsPlayerWritten(const DiaryEntry& entry) {
        return std::ranges::find(entry.tags, kPlayerWrittenTag) != entry.tags.end();
    }

    // Where a volume's entry range starts and ends (docs/VOLUMES_AND_SYNC.md#volume-boundaries).
    // The prev* fields are the volume's own boundary data; the next* fields are the
    // next volume's, when there is one.
    struct VolumeBounds {
        double startTime = 0.0;
        double endTime = 0.0;                  // 0 = open-ended
        double prevLastCreationTime = 0.0;
        int    prevCountAtBoundary = 0;
        double nextStartTime = 0.0;
        double nextPrevLastCreationTime = 0.0;
        int    nextPrevCountAtBoundary = 0;
    };

    // Chronological order: entry_date, then creation_time (real-world write time)
    // to break ties between entries dated the same in-game moment.
    inline bool EntryOlder(const DiaryEntry& a, const DiaryEntry& b) {
        if (a.entry_date != b.entry_date) return a.entry_date < b.entry_date;
        return a.creation_time < b.creation_time;
    }

    inline constexpr double kSecondsPerGameDay = 86400.0;

    // Current game time in entry_date units (game seconds): what diary dates are
    // compared against.
    inline double CurrentGameTimeSeconds() {
        auto* calendar = RE::Calendar::GetSingleton();
        return calendar ? calendar->GetCurrentGameTime() * kSecondsPerGameDay : 0.0;
    }

    // True if `date` (entry_date units) is later than game time `now`, with one game
    // second of slack for rounding: "written after the loaded save".
    inline bool DatedAfter(double date, double now) {
        return date > now + 1.0;
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
        // An empty result can mean "no entries" or "the query failed"; pass `ok` to
        // tell them apart (false: SkyrimNet unavailable, an exception, bad JSON).
        static std::vector<DiaryEntry> GetDiaryEntries(uint32_t formId, int limit = kFetchAllEntries,
                                                        double startTime = 0.0, double endTime = 0.0,
                                                        bool* ok = nullptr);

        // A volume's entries, oldest first: the whole [startTime, endTime] range minus
        // the previous volume's boundary entries and, on a date shared with the next
        // volume, the entries that volume owns.  Volume sizes come only from these
        // stored boundaries, never from the current EntriesPerVolume setting.
        static std::vector<DiaryEntry> GetVolumeEntries(uint32_t formId, const VolumeBounds& bounds,
                                                         bool* ok = nullptr);

        // Game time of the player's most recent SkyrimNet event, in entry_date units
        // (0 when there is none).  SkyrimNet asks its keep/clear question on load
        // exactly when this is later than the current game time.
        static double GetPlayerLastEventTime();

        // Registers a native prompt decorator (SkyrimNet public API v5+).  The callback
        // runs on SkyrimNet's worker threads.  False if unavailable or refused.
        static bool RegisterDecorator(const char* name, const char* description,
                                      std::function<std::string(RE::Actor*)> callback);

        // Any thread but the game thread (SkyrimNet re-embeds the entry's memory): replace an
        // entry's text and tags.  False when SkyrimNet refuses (its keep/clear timeline check
        // is pending), the entry is gone, the API is older than v11, or the call failed.
        static bool UpdateDiaryEntry(int entryId, const std::string& content, const std::string& tagsCSV);

        // Get bio template name for an actor FormID
        static std::string GetBioTemplateName(uint32_t formId);

        // UUID ↔ FormID conversion (using PublicAPI).  GetUUIDFromFormID returns ""
        // for an actor SkyrimNet doesn't know.
        static std::string GetUUIDFromFormID(uint32_t formId);
        static uint32_t GetFormIDForUUID(const std::string& uuid);

        // Actor name lookup by UUID
        static std::string GetActorName(const std::string& uuid);

        // Get bio template name by UUID (converts UUID → FormID → GetBioTemplateName)
        static std::string GetTemplateNameByUUID(const std::string& uuid);

    private:
        // Parse JSON response from PublicGetDiaryEntries into DiaryEntry structures
        // `ok` (if given) is set false when the response isn't a valid JSON array.
        static std::vector<DiaryEntry> ParseDiaryJSON(const std::string& jsonResponse, bool* ok = nullptr);

        // Track if API has been initialized
        static inline bool api_initialized_ = false;
    };

} // namespace SkyrimNetDiaries
