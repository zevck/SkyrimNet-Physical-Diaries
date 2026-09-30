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
#include <cstdint>
#include <cstring>

// SNPD inter-plugin API: Dispatch a query struct to PluginName from the game thread (state is read without a lock).
// Filled in before Dispatch returns.  Send it for any book; never filter by (localized) title.  See docs/PAPYRUS_AND_API.md.

namespace SkyrimNetPhysicalDiaries_API
{
    constexpr const char* PluginName    = "SkyrimNetPhysicalDiaries";
    constexpr std::uint32_t SNPD_API_VERSION = 3;

    // Message types sent via SKSE::GetMessagingInterface()->Dispatch()
    constexpr std::uint32_t SNPD_QUERY_BOOK  = 'SNPD'; // query full volume text + metadata
    constexpr std::uint32_t SNPD_QUERY_ENTRY = 'SNPE'; // query a single diary entry by index

    // ── Buffer size constants ──────────────────────────────────────────────────
    constexpr std::size_t SNPD_MAX_BOOK_TEXT    = 65536;  // SNPDBookQuery::text capacity
    constexpr std::size_t SNPD_MAX_ENTRY_TEXT   = 8192;   // SNPDEntryQuery::content capacity
    constexpr std::size_t SNPD_MAX_ALL_ENTRIES  = 262144; // SNPDAllEntriesQuery::content capacity

    // ── Result codes ──────────────────────────────────────────────────────────
    enum class SNPDResultCode : std::int32_t {
        Success         = 0, // Query completed successfully
        NotADiary       = 1, // FormID is not one of our diary books
        IndexOutOfRange = 2, // entryIndex was out of range
        NoEntries       = 3, // Volume is registered but has no readable entries
    };

    // ── SNPD_QUERY_BOOK: fill bookFormId and Dispatch; isDiaryBook / resultCode say whether it is one of ours.
    // API v2 replaced filePath[512] with text; v3 added entryCount, volumeNumber and totalVolumes.
    struct SNPDBookQuery
    {
        // ── Request (caller fills in) ──────────────────────────────────────
        std::uint32_t apiVersion = SNPD_API_VERSION; // must be SNPD_API_VERSION
        std::uint32_t bookFormId = 0;                // FormID of the book to query

        // ── Response (filled by SkyrimNetPhysicalDiaries) ─────────────────
        SNPDResultCode resultCode = SNPDResultCode::Success; // see SNPDResultCode
        bool isDiaryBook = false;  // true  → this FormID is one of our diaries
                                   // false → not ours (resultCode NotADiary; counts and text zeroed)

        // Number of diary entries in this volume.
        // Use with SNPD_QUERY_ENTRY to iterate or select entries individually.
        std::int32_t entryCount   = 0;

        // Which volume this book is (1-based). Useful for display ("Volume 2").
        std::int32_t volumeNumber = 0;

        // Total number of volumes this actor has written.
        std::int32_t totalVolumes = 0;

        // SNPD's rendered copy (refreshed when the book opens; font-tagged, &<> plain), null-terminated, at most
        // SNPD_MAX_BOOK_TEXT - 1 chars.  Empty when the volume has no readable entries; valid only if isDiaryBook.
        char text[SNPD_MAX_BOOK_TEXT] = {};
    };

    // ── SNPD_QUERY_ENTRY: one entry of a volume by 0-based index, without processing the whole volume.
    // SNPD_QUERY_BOOK gives entryCount.
    struct SNPDEntryQuery
    {
        // ── Request (caller fills in) ──────────────────────────────────────
        std::uint32_t apiVersion = SNPD_API_VERSION; // must be SNPD_API_VERSION
        std::uint32_t bookFormId = 0;                // FormID of the volume to query

        // 0-based index of the entry to fetch.
        // Pass -1 to get the most recent (last) entry.
        std::int32_t  entryIndex = -1;

        // ── Response (filled by SkyrimNetPhysicalDiaries) ─────────────────
        SNPDResultCode resultCode    = SNPDResultCode::Success; // see SNPDResultCode
        bool          isValid        = false; // false → index out of range or volume not found
        std::int32_t  totalEntries   = 0;     // total entries in this volume
        std::int32_t  returnedIndex  = -1;    // actual 0-based index returned

        // Entry text (font-tagged, same as vanilla books) — ready for display or TTS.
        // Max length: SNPD_MAX_ENTRY_TEXT - 1 characters + null terminator.
        char content[SNPD_MAX_ENTRY_TEXT]  = {};
    };

    // ── SNPD_QUERY_ALL_ENTRIES: every entry, null-terminated back to back, oldest first (next: p += strlen(p) + 1).
    // Entries that don't fit are skipped, counted in truncatedCount.  ~256 KB: heap-allocate (std::make_unique).
    struct SNPDAllEntriesQuery
    {
        // ── Request (caller fills in) ──────────────────────────────────────
        std::uint32_t apiVersion = SNPD_API_VERSION;
        std::uint32_t bookFormId = 0;

        // ── Response (filled by SkyrimNetPhysicalDiaries) ─────────────────
        SNPDResultCode resultCode    = SNPDResultCode::Success; // see SNPDResultCode
        bool          isValid        = false; // false → volume not found
        std::int32_t  entryCount     = 0;     // number of strings packed in content[]
        std::int32_t  truncatedCount = 0;     // entries that didn't fit (0 = complete)

        // Entry text (font-tagged, as in SNPD_QUERY_ENTRY), packed; sized for ~50 entries of ~4 KB plus headroom.
        // Capacity: SNPD_MAX_ALL_ENTRIES - 1 bytes of text + a final '\0' guard.
        char content[SNPD_MAX_ALL_ENTRIES] = {};
    };

    constexpr std::uint32_t SNPD_QUERY_ALL_ENTRIES = 'SNPA';

} // namespace SkyrimNetPhysicalDiaries_API
