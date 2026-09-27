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
        // GetDiaryEntries to exclude boundary entries that share an entry_date with
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

        // True once this volume has been included in a Skyrim .ess save file.
        // Set at kSaveGame.  When false the NPC's inventory state is not
        // authoritative (reload-without-save) and QueueInventoryCheck may re-add.
        bool persistedInSave = false;
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

        // Initialize with template book Editor IDs from ESP
        // baseTemplate is the default, others are for variety (pass empty strings to disable variety)
        void Initialize();

        // Queues creation of one volume's book (async: DPF creates the form, then the
        // volume is registered and the book added to the NPC).  `entries` are the
        // volume's entries, oldest first.  Defined in BookCreation.cpp.
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

        // Get all books (for checking volume numbers)
        const std::unordered_map<std::string, std::vector<DiaryBookData>>& GetAllBooks() const { return books_; }
        std::unordered_map<std::string, std::vector<DiaryBookData>>& GetAllBooksRef() { return books_; }

        // Track a book creation
        void RegisterBook(const std::string& actorUuid, const std::string& actorName,
                         RE::FormID bookFormId, double startTime, double endTime, int volumeNumber,
                         const std::string& journalTemplate = "",
                         const std::string& bioTemplateName = "",
                         double prevVolumeLastCreationTime = 0.0,
                         int prevVolumeCountAtBoundary = 0,
                         RE::FormID actorFormId = 0);

        // Update a volume's endTime (after an update, a seal or a deletion), in memory and DiaryDB
        void UpdateBookEndTime(const std::string& actorUuid, int volumeNumber, double endTime);

        // Renders `entries` into the volume (bounded by its startTime/endTime), writes the
        // text and entry count to DiaryDB, and updates cachedBookText / lastKnownEntryCount.
        void SetVolumeText(DiaryBookData& vol, const std::vector<DiaryEntry>& entries);

        // Unregister a book (when it becomes obsolete due to deletions)
        void UnregisterBook(const std::string& actorUuid);

        // Drops the actor's volumes numbered fromVolume and up (memory and DiaryDB).
        void UnregisterVolumesFrom(const std::string& actorUuid, int fromVolume);

        // Re-render every tracked volume from SkyrimNet (MCM "Regenerate texts" and font changes)
        void RegenerateAllDiaryTexts();

        // Load all volumes from DiaryDB into books_ / actorTemplates_.
        // Validates DPF form IDs — volumes with invalid forms are removed from the
        // DB so UpdateDiaryForActorInternal will recreate them.  Returns UUIDs of
        // actors whose latest volume was invalid (useful for logging).
        std::vector<std::string> LoadFromDB();

        // Clears the actor reference cache so the next inventory-add does a fresh
        // lookup.  Must be called on each kPostLoadGame to avoid stale pointers.
        static void ClearActorCache();

        // Clears the invalid sourceFiles pointer DPF leaves on some clones (VR),
        // across every loaded book form.  Call at kPostLoadGame, before anything
        // reads book descriptions.  Defined in BookCreation.cpp.
        static void SanitizeLoadedBookForms();

        // For every volume currently in books_, queues a game-thread task that checks
        // whether the owning NPC has the book in their inventory and re-adds it if not.
        // Call this after LoadFromDB to recover books whose DPF forms survived an
        // in-session reload but whose inventory entries did not.
        void QueueInventoryCheck();

        // Write every in-memory book and actor template to DiaryDB.
        // Safe to call when DB is not open (no-op in that case).
        // Used at save time to flush books created before the DB was opened
        // (e.g. first session on a brand-new save).
        void FlushToDB();

        // Called by BookTextHook immediately before a diary volume's text is injected
        // into the book UI.  Queries live entry count, reformats if it changed, and
        // updates the sealed endTime if entries were deleted.
        void RefreshVolumeOnOpen(DiaryBookData* vol);

        // Serialization (co-save — legacy; data now lives in DiaryDB)
        void Save(SKSE::SerializationInterface* a_intfc);
        void Load(SKSE::SerializationInterface* a_intfc, std::uint32_t version = 1);
        void Revert();

    private:
        BookManager() = default;
        BookManager(const BookManager&) = delete;
        BookManager& operator=(const BookManager&) = delete;

        // Select appropriate journal template for an actor
        std::string SelectJournalTemplate(const std::string& actorUuid, const std::string& actorName,
                                          RE::FormID actorFormId);

        std::vector<std::string> journalTemplates_;  // variants picked from by UUID hash
        std::string nightingaleTemplate_;            // Karliah, Gallus, Mercer Frey
        std::unordered_map<std::string, std::string> actorTemplates_;  // UUID → template choice (persists across volumes)
        std::unordered_map<std::string, std::vector<DiaryBookData>> books_; // UUID → all volumes
    };

} // namespace SkyrimNetDiaries
