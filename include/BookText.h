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
#include "Database.h"  // DiaryEntry

// Renders diary entries into the font-tagged book text BookTextHook injects.
// See docs/BOOK_TEXT.md.
namespace SkyrimNetDiaries {

    // Page separator in rendered text.  Page 0 is blank, page 1 the title, and
    // every later page one entry; the inter-plugin API splits on this.
    inline constexpr std::string_view kPageBreak = "[pagebreak]\n\n";

    // One volume: blank page, title page with date range, then one page per entry.
    // Entries outside [startTime, endTime] are skipped (0 = unbounded).  An empty
    // list renders the "all entries removed" page, marked with kEmptySentinel.
    std::string FormatDiaryEntries(const std::vector<DiaryEntry>& entries,
                                   const std::string& actorName,
                                   double startTime, double endTime,
                                   int maxEntries = 10);

} // namespace SkyrimNetDiaries
