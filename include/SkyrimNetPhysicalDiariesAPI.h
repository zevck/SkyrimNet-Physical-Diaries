#pragma once
#include <cstdint>
#include <cstring>

// =============================================================================
// SkyrimNet Physical Diaries - Inter-Plugin API
// =============================================================================
//
// Allows other SKSE plugins to resolve diary book file paths.
//
// USAGE — detecting and reading one of our diary books:
//
//   Step 1: Cheap heuristic pre-filter (avoids an API call for every book)
//     a) Book display name matches "*'s Diary*"
//     b) Book has no vanilla text (book->itemCardDescription is empty)
//     If both are true, proceed to Step 2.
//
//   Step 2: API check (definitive)
//     Fill in an SNPDBookQuery with the book's FormID and dispatch it to us.
//     If isDiaryBook == true in the response, filePath holds the absolute path.
//
//   Example:
//
//     SNPDBookQuery query{};
//     query.apiVersion = SNPD_API_VERSION;
//     query.bookFormId = book->GetFormID();
//
//     auto* messaging = SKSE::GetMessagingInterface();
//     messaging->Dispatch(SNPD_QUERY_BOOK, &query, sizeof(query),
//                         "SkyrimNetPhysicalDiaries");
//
//     if (query.isDiaryBook) {
//         // query.filePath contains the absolute path to the .txt file
//         std::ifstream f(query.filePath);
//         // ... read diary text ...
//     }
//
// NOTE: Dispatch is synchronous — the response is filled in before it returns.
// NOTE: filePath is only valid when isDiaryBook == true.
// =============================================================================

namespace SkyrimNetPhysicalDiaries_API
{
    constexpr const char* PluginName    = "SkyrimNetPhysicalDiaries";
    constexpr std::uint32_t SNPD_API_VERSION = 1;

    // Message type sent via SKSE::GetMessagingInterface()->Dispatch()
    constexpr std::uint32_t SNPD_QUERY_BOOK = 'SNPD';

    // Request/response struct — allocate on the stack before calling Dispatch.
    // The handler fills in the response fields in-place; Dispatch is synchronous.
    struct SNPDBookQuery
    {
        // ── Request (caller fills in) ──────────────────────────────────────
        std::uint32_t apiVersion = SNPD_API_VERSION; // must be SNPD_API_VERSION
        std::uint32_t bookFormId = 0;                // FormID of the book to query

        // ── Response (filled by SkyrimNetPhysicalDiaries) ─────────────────
        bool isDiaryBook = false;  // true  → this FormID is one of our diaries
                                   // false → not ours; filePath is empty

        // Absolute path to the .txt file that holds the diary's rendered text.
        // Null-terminated. Only valid when isDiaryBook == true.
        // Max length: 511 characters + null terminator.
        char filePath[512] = {};
    };

} // namespace SkyrimNetPhysicalDiaries_API
