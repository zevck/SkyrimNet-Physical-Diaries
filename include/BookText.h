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

    // One volume of exactly the entries given, titled by kind; none: the sentinel's "all entries removed" page.
    // `marked`: Ink & Quill's marked text instead, never for the book menu (docs/EDITING.md#marked-text).
    std::string FormatDiaryEntries(const std::vector<DiaryEntry>& entries, const std::string& actorName,
                                   VolumeKind kind = VolumeKind::Generated, bool marked = false);

    // Blood text: stored as byte ranges in DiaryDB, marked inline with these private-use characters
    // while editing and rendering.  See docs/EDITING.md#writing-in-blood.
    inline constexpr std::string_view kBloodOpen = "\xEE\x80\x80";   // U+E000
    inline constexpr std::string_view kBloodClose = "\xEE\x80\x81";  // U+E001
    // Dried blood on the lit book page (vanilla ink is black); BookMenu.as BLOOD_COLOR too.
    inline constexpr std::string_view kBloodColor = "#2B0202";
    // The content with the markers around each range.
    std::string MarkBlood(const std::string& content, const std::string& ranges);
    // Marked text: the content (no markers) and its ranges.
    std::pair<std::string, std::string> SplitBlood(std::string_view marked);
    // In Ink & Quill's marked text, what the player can't change is between these (U+E002, U+E003).
    inline constexpr std::string_view kLockOpen = "\xEE\x80\x82";   // U+E002
    inline constexpr std::string_view kLockClose = "\xEE\x80\x83";  // U+E003

    // Plain-text pieces of that layout for the book editor (no markup, not escaped).
    // An entry's text as its page shows it (the same cleanup as FormatDiaryEntries), blood marked.
    std::string EditableEntryText(const DiaryEntry& entry);

    // Text's paragraphs (split at "\n\n"; empty ones are blank lines and kept, but not at the end): an entry's
    // run in the editor, and its pages.
    std::vector<std::string_view> Paragraphs(std::string_view text);
    // Its date as a heading shows it, whether headings are on or not.
    std::string EntryDate(const DiaryEntry& entry);
    // The title page's date range ("" for no entries).
    std::string TitlePageDates(const std::vector<DiaryEntry>& entries);

} // namespace SkyrimNetDiaries
