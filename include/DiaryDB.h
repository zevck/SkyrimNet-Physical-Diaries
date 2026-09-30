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
#include "Database.h"  // VolumeKind

// Forward-declare sqlite3 to avoid pulling the full header into every TU.
struct sqlite3;

namespace SkyrimNetDiaries {

    // DiaryDB: per-character SQLite store for volume metadata and rendered text, keyed to the SkyrimNet
    // save folder so it survives save reverts.  Location and schema: docs/DATABASE.md.

    class DiaryDB {
    public:
        static DiaryDB* GetSingleton();

        // Open (or create) the DB for the save folder.  The same folder again is a no-op; a different
        // folder closes the old connection first.
        bool Open(const std::string& saveFolder);
        void Close();
        bool IsOpen() const { return db_ != nullptr; }

        // ── Volume operations ─────────────────────────────────────────────────

        struct VolumeRow {
            std::string  actorUuid;
            std::string  actorName;
            std::uint32_t actorFormId                  = 0;  // Actor's FormID at creation; a hint, checked against the UUID before use
            std::uint32_t bookFormId                   = 0;
            int           volumeNumber                 = 1;
            VolumeKind    kind                         = VolumeKind::Generated;
            double        startTime                    = 0.0;
            double        endTime                      = 0.0;
            std::string  journalTemplate;
            std::string  bioTemplateName;
            int           lastKnownEntryCount          = 0;
            double        prevVolumeLastCreationTime   = 0.0;
            int           prevVolumeCountAtBoundary    = 0;
            std::string  bookText;   // rendered font-tagged text ready for injection
        };

        // Insert-or-replace a full volume row.  An empty bookText keeps the stored book_text
        // (a metadata-only upsert doesn't wipe the render).
        bool UpsertVolume(const VolumeRow& row);

        // Update only the rendered text and entry count (called after formatting).
        bool UpdateBookText(const std::string& actorUuid, VolumeKind kind, int volumeNumber,
                            const std::string& text, int entryCount);

        // Update only end_time (a volume's last entry moved: update, seal or deletion).
        bool UpdateEndTime(const std::string& actorUuid, VolumeKind kind, int volumeNumber,
                           double endTime);

        // Delete a single volume row.
        bool DeleteVolume(const std::string& actorUuid, VolumeKind kind, int volumeNumber);

        // Delete all volumes of both kinds (and the template) for an actor.
        bool DeleteActor(const std::string& actorUuid);

        // Return all volume rows ordered by (actor_uuid, kind, volume_number).
        std::vector<VolumeRow> LoadAllVolumes();

        // ── Actor-template operations ─────────────────────────────────────────

        std::unordered_map<std::string, std::string> LoadActorTemplates();

        // ── Theft tracking ────────────────────────────────────────────────────

        // Track which specific volumes are stolen (not just a timestamp)
        bool AddStolenVolume(const std::string& actorUuid, int volumeNumber, double gameTime);
        bool RemoveStolenVolume(const std::string& actorUuid, int volumeNumber);
        // Every actor with at least one stolen volume.
        std::vector<std::string> LoadStolenActorUuids();
        bool ClearAllStolenVolumes(const std::string& actorUuid);
        // Drops thefts recorded after `gameTime`: after loading an earlier save they
        // happened in a timeline the player has left.  Returns how many were removed.
        int RemoveStolenVolumesAfter(double gameTime);

        bool UpsertActorTemplate(const std::string& uuid,
                                 const std::string& templateName);

        // ── Blood (the red ranges of the player's entries, BookText.h MarkBlood) ─

        // Stores an entry's ranges for exactly this content, and whether it was begun in blood
        // (its heading red); neither removes the row.
        bool SetBlood(int entryId, const std::string& ranges, const std::string& content, bool heading);
        bool DeleteBlood(int entryId);
        struct Blood {
            std::string ranges;  // "" unless stored for this content (else they'd colour the wrong words)
            bool heading = false;
        };
        Blood GetBlood(int entryId, const std::string& content);

    private:
        DiaryDB() = default;
        DiaryDB(const DiaryDB&) = delete;
        DiaryDB& operator=(const DiaryDB&) = delete;

        bool EnsureSchema();
        bool Exec(const char* sql);
        bool HasColumn(const char* table, const char* column);

        sqlite3*    db_          = nullptr;
        std::string openFolder_;  // folder name the DB was opened for
    };

} // namespace SkyrimNetDiaries
