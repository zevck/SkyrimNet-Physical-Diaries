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

// =============================================================================
// SkyrimNet Physical Diaries - Inter-Plugin API
// =============================================================================
//
// Allows other SKSE plugins to query diary book content and metadata.
//
// Three query types are available:
//   SNPD_QUERY_BOOK        — full rendered volume text + metadata
//   SNPD_QUERY_ENTRY       — single entry by index (font-tagged)
//   SNPD_QUERY_ALL_ENTRIES — all entries for a volume in one call (font-tagged)
//
// All queries are synchronous — the response is filled before Dispatch returns.
// Dispatch from the game thread: the handler reads SNPD's in-memory state
// without locking.
//
// Text is SNPD's rendered copy of the diary (refreshed whenever the book is
// opened), not a live SkyrimNet query.  Entry text is the diary prose wrapped in
// <font> tags; "&", "<" and ">" inside the prose are returned as plain characters.
//
// USAGE — detecting and reading one of our diary books:
//
//   Send the query for any book: it is an in-memory lookup.  Don't pre-filter
//   by name, since book titles are localized.  isDiaryBook / resultCode say
//   whether the FormID is one of our diary volumes.
//
//   Example:
//
//     using namespace SkyrimNetPhysicalDiaries_API;
//
//     SNPDBookQuery query{};
//     query.apiVersion = SNPD_API_VERSION;
//     query.bookFormId = book->GetFormID();
//
//     SKSE::GetMessagingInterface()->Dispatch(
//         SNPD_QUERY_BOOK, &query, sizeof(query), PluginName);
//
//     if (query.isDiaryBook) {
//         // query.text holds the full rendered volume text (font-tagged)
//         // query.entryCount, volumeNumber, totalVolumes hold metadata
//         // query.resultCode == SNPDResultCode::Success
//         //                  or SNPDResultCode::NoEntries (entries removed)
//     }
//
// =============================================================================

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

    // ── SNPD_QUERY_BOOK ───────────────────────────────────────────────────────
    //
    // Allocate SNPDBookQuery on the stack, fill bookFormId, Dispatch to us.
    // Dispatch is synchronous — response is filled before it returns.
    //
    // API v2: filePath[512] replaced by text[65536] (rendered diary text).
    // API v3: added entryCount, volumeNumber, totalVolumes metadata fields.
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

        // Null-terminated rendered diary text (font-tagged, ready for display).
        // Empty (text[0]=='\0') when the volume has no readable entries.
        // Only valid when isDiaryBook == true.
        // Max length: SNPD_MAX_BOOK_TEXT - 1 characters + null terminator.
        char text[SNPD_MAX_BOOK_TEXT] = {};
    };

    // ── SNPD_QUERY_ENTRY ──────────────────────────────────────────────────────
    //
    // Fetch one diary entry by index from a known volume.
    // Use SNPD_QUERY_BOOK first to learn entryCount, then request individual
    // entries by 0-based index, without processing an entire volume.
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

    // ── SNPD_QUERY_ALL_ENTRIES ───────────────────────────────────────────────
    //
    // Fetch all diary entries for a volume in one call.
    // Content strings are packed null-terminated, back-to-back, in
    // chronological order. Iterate with:
    //
    //   const char* p = query.content;
    //   for (int i = 0; i < query.entryCount; i++) {
    //       // use p as a C-string
    //       p += strlen(p) + 1;
    //   }
    //
    // If the total content exceeds the buffer, an entry that doesn't fit is
    // skipped (a later, shorter one may still be packed) and truncatedCount holds
    // how many were skipped.  A volume whose entries were all removed returns
    // resultCode Success, isValid true and entryCount 0.
    //
    // NOTE: sizeof(SNPDAllEntriesQuery) ~= 256 KB. Always heap-allocate:
    //   auto query = std::make_unique<SNPDAllEntriesQuery>();
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

        // Entry text (font-tagged, as in SNPD_QUERY_ENTRY), packed null-terminated strings.
        // Sized for worst case: 50 entries × ~4 KB average = ~200 KB, plus headroom.
        // Total capacity: SNPD_MAX_ALL_ENTRIES - 1 bytes of text + final '\0' guard.
        char content[SNPD_MAX_ALL_ENTRIES] = {};
    };

    constexpr std::uint32_t SNPD_QUERY_ALL_ENTRIES = 'SNPA';

} // namespace SkyrimNetPhysicalDiaries_API
