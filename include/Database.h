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
#include <charconv>
#include <optional>
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
        int localKey = 0;              // SNPD only: a new entry the book editor added (0 = from SkyrimNet)
        std::string blood;             // SNPD only: the red ranges of a journal entry's content (BookText.h MarkBlood)
        bool bloodHeading = false;     // SNPD only: begun in blood, so its heading is red too
    };

    // Tag SNPD adds to an entry the player edited in their diary: shown exactly as written.
    inline constexpr std::string_view kPlayerWrittenTag = "snpd_player_written";

    // Tag SNPD adds to an entry with any text the player wrote in blood.
    inline constexpr std::string_view kBloodTag = "snpd_written_in_blood";

    inline bool IsPlayerWritten(const DiaryEntry& entry) {
        return std::ranges::find(entry.tags, kPlayerWrittenTag) != entry.tags.end();
    }

    // Tag naming the player's journal an entry is in, "snpd_journal_<volume number>", whatever its date
    // (docs/EDITING.md#diaries-and-journals).
    inline constexpr std::string_view kJournalTagPrefix = "snpd_journal_";

    // The journal an entry's tag names, or 0 (none: written before journals had tags, and
    // shown in journal 1).
    inline int JournalOf(const DiaryEntry& entry) {
        for (const auto& tag : entry.tags) {
            if (!tag.starts_with(kJournalTagPrefix)) continue;
            int number = 0;
            const auto digits = std::string_view(tag).substr(kJournalTagPrefix.size());
            if (std::from_chars(digits.data(), digits.data() + digits.size(), number).ec == std::errc{}) return number;
        }
        return 0;
    }

    // Puts the entry in journal `number` (replacing any other journal tag).
    inline void SetJournal(DiaryEntry& entry, int number) {
        std::erase_if(entry.tags, [](const std::string& tag) { return tag.starts_with(kJournalTagPrefix); });
        entry.tags.push_back(std::format("{}{}", kJournalTagPrefix, number));
    }

    // Generated: SkyrimNet's entries (a diary).  Written: the player's tagged entries (a journal).
    // docs/EDITING.md#diaries-and-journals
    enum class VolumeKind : int { Generated = 0, Written = 1 };

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

    // SkyrimNet public API version SNPD needs: PublicQueryDiaryEntries, the diary edits, the timeline state.
    inline constexpr int kRequiredApiVersion = 11;

    class Database {
    public:
        Database() = default;
        ~Database() = default;

        // Initialize the SkyrimNet API
        static bool InitializeAPI();

        // Check if SkyrimNet memory system is ready
        static bool IsMemorySystemReady();

        // Entries for a FormID (0 = all) dated in [startTime, endTime], in date order.  `limit` keeps the NEWEST;
        // `ok` tells empty from failed.  Volumes use GetEntriesById.  docs/VOLUMES_AND_SYNC.md
        static std::vector<DiaryEntry> GetDiaryEntries(uint32_t formId, int limit = kFetchAllEntries,
                                                        double startTime = 0.0, double endTime = 0.0,
                                                        bool* ok = nullptr, VolumeKind kind = VolumeKind::Generated);

        // Entries in write order (SkyrimNet id), ids in (afterId, upToId] (upToId 0 = no end), the lowest `limit`.
        // The player's are one kind only.  `ok` tells empty from failed.  docs/VOLUMES_AND_SYNC.md#entries
        static std::vector<DiaryEntry> GetEntriesById(uint32_t formId, int afterId, int upToId = 0,
                                                      int limit = kFetchAllEntries, bool* ok = nullptr,
                                                      VolumeKind kind = VolumeKind::Generated);

        // SkyrimNet's public API version, once InitializeAPI has run (0: SkyrimNet not loaded).
        static int ApiVersion() { return api_version_; }

        // The player's latest SkyrimNet event, in entry_date units (0: none).  SkyrimNet asks its keep/clear
        // question on load exactly when this is later than the current game time.
        static double GetPlayerLastEventTime();

        // SkyrimNet's keep/clear check (TimelineState values), or nullopt before public API v11.
        static std::optional<int> GetTimelineState();

        // SkyrimNet's per-actor activity (memory importance, event counts) over two game-second windows, as its
        // JSON array; "" if unavailable.  docs/NPC_DIARIES.md#who-writes
        static std::string GetActorEngagement(double shortWindowSeconds, double mediumWindowSeconds);

        // Registers a native prompt decorator (SkyrimNet public API v5+).  The callback
        // runs on SkyrimNet's worker threads.  False if unavailable or refused.
        static bool RegisterDecorator(const char* name, const char* description,
                                      std::function<std::string(RE::Actor*)> callback);

        // True when SkyrimNet can add, save and delete diary entries (public API v11): the
        // book editor needs all three before the player writes anything.
        static bool CanWriteDiaries();

        // Not on the game thread (SkyrimNet embeds the memory).  The new id, or 0 when SkyrimNet refuses
        // (keep/clear timeline check pending) or the call failed.
        static int AddDiaryEntry(uint32_t formId, const std::string& content, double entryDate,
                                 const std::string& tagsCSV);

        // Not on the game thread (SkyrimNet re-embeds the memory).  False when SkyrimNet refuses (keep/clear
        // check pending), the entry is gone, the API is older than v11, or the call failed.
        static bool UpdateDiaryEntry(int entryId, const std::string& content, const std::string& tagsCSV);

        // Any thread: delete an entry and its memory.  False when SkyrimNet refuses (keep/clear check
        // pending), the entry is gone, the API is older than v11, or the call failed.
        static bool DeleteDiaryEntry(int entryId);

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
        static inline int api_version_ = 0;
    };

} // namespace SkyrimNetDiaries
