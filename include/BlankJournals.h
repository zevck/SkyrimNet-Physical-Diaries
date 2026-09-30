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

// Blank journals: the ESP's "Blank Journal" books (one per look), sold by general-goods merchants and crafted at a
// tanning rack; the Nightingale look only crafted, by a Nightingale.  See docs/EDITING.md#blank-journals.
namespace SkyrimNetDiaries::BlankJournals {

    // kDataLoaded, after WritingMode::Detect.  Writing on: names the books in the game's
    // language and adds them to merchant stock.  Writing off: hides their recipes.
    void OnDataLoaded();

    // The journal look (template book EditorID) of a blank journal, or "" if `bookFormId` isn't one.
    std::string LookOf(RE::FormID bookFormId);

}
