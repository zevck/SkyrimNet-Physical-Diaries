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
#include "Database.h"  // For DiaryEntry struct
#include "Config.h"    // For configuration settings
#include "DiaryDB.h"   // Persistent SQLite store
#include <mutex>

namespace SkyrimNetDiaries {

    struct DiaryBookData {
        std::string actorUuid;
        std::string actorName;
        RE::FormID bookFormId;         // The actual book FormID (unique identifier)
        double startTime;              // Game seconds (entry_date units): first entry's date (0 for volume 1)
        double endTime;                // Game seconds: the last included entry's date
        int volumeNumber = 1;
        std::string journalTemplate;   // Which template book was used (for consistent appearance)
        std::string bioTemplateName;   // Actor-specific subfolder key (e.g. "lydia_3a2") — unique per NPC
        int lastKnownEntryCount = 0;   // Track expected entry count for deletion detection

        // The creation_time of the last entry in the previous volume.  Used by
        // GetVolumeEntries to exclude boundary entries that share an entry_date with
        // this volume's first entry but logically belong to the previous volume.
        double prevVolumeLastCreationTime = 0.0;
        int prevVolumeCountAtBoundary = 0;   // how many prev-vol entries share the boundary date/CT

        // Actor FormID stored at creation time.  Only a hint: a load-order change can
        // make it point at someone else, so it is used only after SkyrimNet maps it
        // back to actorUuid.
        RE::FormID actorFormId = 0;

        // Runtime-only: the actor's FormID resolved from actorUuid this session (0 =
        // not resolved yet).  Never loaded from DiaryDB.
        RE::FormID cachedActorFormId = 0;
        std::string cachedBookText;
    };

    // Template books in the ESP, found by EditorID.  Everyone gets one of the
    // journal variants (by UUID hash); the Nightingale NPCs get their own.
    inline constexpr const char* kJournalTemplates[] = {
        "SkyrimNetDiaryTemplate", "SkyrimNetDiaryTemplate2", "SkyrimNetDiaryTemplate3",
    };
    inline constexpr const char* kNightingaleTemplate = "SkyrimNetDiaryTemplateN";

    class BookManager {
    public:
        static BookManager* GetSingleton();

        // Creates one volume's book, registers the volume and gives the book to the
        // NPC.  `entries` are the volume's entries, oldest first.  Game thread.
        // Defined in BookCreation.cpp.
        void CreateDiaryBook(const std::string& actorUuid, const std::string& actorName,
                             double startTime, int volumeNumber, RE::FormID targetActorFormID,
                             const std::vector<DiaryEntry>& entries, const std::string& bioTemplateName,
                             double prevVolumeLastCreationTime, int prevVolumeCountAtBoundary);

        // Get latest volume for an actor by UUID
        DiaryBookData* GetBookForActor(const std::string& actorUuid);

        // Get all volumes for an actor by UUID
        std::vector<DiaryBookData>* GetAllVolumesForActor(const std::string& actorUuid);

        // Get book data by FormID (searches all actors)
        DiaryBookData* GetBookForFormID(RE::FormID formId);

        // Any thread (never touch books_ off the game thread): the book that owns this
        // description component (0 if not ours), and a copy of a book's rendered text
        // ("" if not ours).  Books with no volume have the "all entries removed" page.
        RE::FormID FindBookByDescription(const RE::TESDescription* description) const;
        std::string GetBookTextSnapshot(RE::FormID bookFormId) const;
        std::string GetBookTextSnapshot(const RE::TESDescription* description) const;

        // A book whose volume is gone for certain (Reset, a Clear): retired, so it stays
        // in the save but is swept out of sight (see BookCreation.h).  Game thread.
        void RetireBook(RE::FormID bookFormId, const std::string& actorUuid);

        // Get all books (for checking volume numbers)
        const std::unordered_map<std::string, std::vector<DiaryBookData>>& GetAllBooks() const { return books_; }
        std::unordered_map<std::string, std::vector<DiaryBookData>>& GetAllBooksRef() { return books_; }

        // Tracks a newly created volume (memory and DiaryDB).  Returns it as stored.
        DiaryBookData& RegisterBook(DiaryBookData data);

        // Update a volume's endTime (after an update, a seal or a deletion), in memory and DiaryDB
        void UpdateBookEndTime(const std::string& actorUuid, int volumeNumber, double endTime);

        // SkyrimNet's entries for `vol`, oldest first, up to `endTime` (0 = open-ended).
        // The next volume's boundary data decides who owns entries on a date both share.
        // `ok` is false when the query failed (as opposed to returning no entries).
        // Game thread: the entries the volume's book shows now (what RefreshVolumeOnOpen
        // renders).  `ok` false: SkyrimNet couldn't be read or the actor isn't resolved.
        std::vector<DiaryEntry> GetShownEntries(DiaryBookData& vol, bool* ok);

        std::vector<DiaryEntry> GetLiveEntries(const DiaryBookData& vol, RE::FormID actorFormId,
                                               double endTime, bool* ok);

        // Renders `entries` (exactly these) into the volume, writes the
        // text and entry count to DiaryDB, and updates cachedBookText / lastKnownEntryCount.
        void SetVolumeText(DiaryBookData& vol, const std::vector<DiaryEntry>& entries);

        // Drops the actor's volumes numbered fromVolume and up (memory and DiaryDB) and
        // retires their books.
        void UnregisterVolumesFrom(const std::string& actorUuid, int fromVolume);

        // Re-render every tracked volume from SkyrimNet (MCM "Regenerate texts" and font changes)
        void RegenerateAllDiaryTexts();

        // Loads DiaryDB's volumes, matched against this save's books (docs/BOOK_FORMS.md#load).
        // Returns the UUIDs of actors with a volume to recreate.  Game thread.
        std::vector<std::string> LoadFromDB();

        // Clears the actor FormID cache.  Called when a session ends (kPreLoadGame,
        // kNewGame).
        static void ClearActorCache();

        // Write every in-memory book and actor template to DiaryDB.
        // Safe to call when DB is not open (no-op in that case).
        // Used at save time to flush books created before the DB was opened
        // (e.g. first session on a brand-new save).
        void FlushToDB();

        // Called by BookTextHook immediately before a diary volume's text is injected
        // into the book UI.  Queries live entry count, reformats if it changed, and
        // moves the volume's endTime back if entries were deleted.
        void RefreshVolumeOnOpen(DiaryBookData* vol);

        // New game or load: clears the in-memory volumes (DiaryDB stays).
        void Revert();

    private:
        BookManager() = default;
        BookManager(const BookManager&) = delete;
        BookManager& operator=(const BookManager&) = delete;

        // Select appropriate journal template for an actor
        std::string SelectJournalTemplate(const std::string& actorUuid, const std::string& actorName,
                                          RE::FormID actorFormId);

        std::unordered_map<std::string, std::string> actorTemplates_;  // UUID → template choice (persists across volumes)
        std::unordered_map<std::string, std::vector<DiaryBookData>> books_; // UUID → all volumes
        // Book FormID → (UUID, volume number): GetBookForFormID runs on every book open,
        // container change and description read.  Kept in step with every books_ edit.
        std::unordered_map<RE::FormID, std::pair<std::string, int>> formIndex_;
        // This save's books no volume claims, by volume key: reused if that volume is
        // created again (docs/BOOK_FORMS.md#load).
        std::unordered_map<std::string, RE::FormID> unclaimed_;
        // Read by the text hook on any thread, so under their own lock.
        mutable std::mutex snapshotMutex_;
        std::unordered_map<const RE::TESDescription*, RE::FormID> descriptionIndex_;
        std::unordered_map<RE::FormID, std::string> textSnapshot_;
        std::string TextSnapshotLocked(RE::FormID bookFormId) const;
        void SetTextSnapshot(RE::FormID bookFormId, const std::string& text);
        // The "all entries removed" page, for a book with no volume.
        void ShowRemovedPage(RE::FormID bookFormId, const std::string& actorUuid);

        // Adds a volume's book to the indexes and snapshot / removes it / clears them.
        void IndexBook(const DiaryBookData& vol);
        void UnindexBook(RE::FormID bookFormId);
        void ClearIndexes();
    };

} // namespace SkyrimNetDiaries
