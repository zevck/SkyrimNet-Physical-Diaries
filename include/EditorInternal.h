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
#include "BookManager.h"
#include "Database.h"
#include <unordered_map>
#include <unordered_set>

// Shared by the book editor's sources: BookEditor.cpp, EditorWrites.cpp, EditorJournals.cpp.
// Game thread: Ink & Quill's callbacks are UI tasks, safe for game state paused or not.  docs/ARCHITECTURE.md#threading
namespace SkyrimNetDiaries::BookEditor {

    // An entry being edited, its run as given or last saved, and whether its last save failed.
    struct EditedEntry {
        DiaryEntry entry;
        std::string savedBody;
        bool unsaved = false;

        // Added in the editor and not in SkyrimNet yet (no id): entry.localKey names it.
        bool IsNew() const { return entry.id == 0 && entry.localKey != 0; }
    };

    inline std::vector<EditedEntry> g_edit;  // the session's entries: entry k is Ink & Quill's run k
    inline RE::FormID g_bookFormId = 0;      // the journal being edited

    // An entry's key in the pending-write bookkeeping: its id, or its local key (negated).
    inline int EntryKey(const DiaryEntry& entry) { return entry.id != 0 ? entry.id : -entry.localKey; }

    // The entries as the book shows them: a new entry that was never given text isn't one.
    std::vector<DiaryEntry> EditedEntries(int skip = -1);

    // ---- Sessions (BookEditor.cpp) ----

    void Notify(const std::string& text);

    // ---- Writes to SkyrimNet (EditorWrites.cpp) ----

    // Writes SkyrimNet hasn't finished, per journal: editing it again starts from `entries`.
    // `unsaved`: refused updates, written again next save; `stale`: a delete failed.
    struct PendingWrites {
        int count = 0;
        std::vector<DiaryEntry> entries;
        std::unordered_set<int> unsaved;
        bool stale = false;
    };
    inline std::unordered_map<RE::FormID, PendingWrites> g_pending;
    inline std::uint32_t g_generation = 0;         // bumped when a session ends (Reset)
    inline std::unordered_set<int> g_addQueued;    // local keys whose add is in the write queue

    struct WriteJob {
        enum class Kind { Update, Delete, Add };
        Kind kind = Kind::Update;
        std::uint32_t generation = 0;
        RE::FormID bookFormId = 0;
        int entryId = 0;   // 0: a new entry, found by localKey once its add has run
        int localKey = 0;
        double entryDate = 0.0;  // Add only
        std::string text;
        std::string tagsCSV;
        std::string blood;          // Add, Update: the text's red ranges (DiaryDB, once SkyrimNet has it)
        bool bloodHeading = false;  // Add, Update: begun in blood (its heading red)
    };

    // Queue writes for the journal being edited, and show the edited entries in its book now.
    void QueueWrites(std::vector<WriteJob> jobs);

    // ---- Journals (EditorJournals.cpp) ----


    // Whose journals: the player's SkyrimNet UUID and name (SkyrimNet's, else the game's).
    struct JournalOwner {
        std::string uuid;
        std::string name;
    };
    std::optional<JournalOwner> PlayerJournalOwner();

    // A new, empty journal beside the player's others, in `look` (a template EditorID), in their inventory.
    // Its book's FormID, or 0.
    RE::FormID StartJournal(const std::string& look);

} // namespace SkyrimNetDiaries::BookEditor
