#pragma once

#include "PCH.h"
#include "Database.h"  // DiaryEntry

// Renders diary entries into the font-tagged book text BookTextHook injects.
// See docs/BOOK_TEXT.md.
namespace SkyrimNetDiaries {

    // One volume: blank page, title page with date range, then one page per entry.
    // Entries outside [startTime, endTime] are skipped (0 = unbounded).  An empty
    // list renders the "all entries removed" page, marked with kEmptySentinel.
    std::string FormatDiaryEntries(const std::vector<DiaryEntry>& entries,
                                   const std::string& actorName,
                                   double startTime, double endTime,
                                   int maxEntries = 10);

} // namespace SkyrimNetDiaries
