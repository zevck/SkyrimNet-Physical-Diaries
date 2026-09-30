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

// Quill and ink: writing needs a quill, and each writing session uses one dip of ink.  A
// partly used inkwell is its own item, one per uses left (the ESP's SNPD_Inkwell1-9, clones of
// the vanilla inkwell), swapped for the next one down at each use; Description Framework, if
// installed, shows which it is.  See docs/EDITING.md#quill-and-ink.
namespace SkyrimNetDiaries::WritingTools {

    // kDataLoaded, writing on: finds the ESP's quills, inkwells and partly used inkwells, and
    // gives those the game's own name for an inkwell.
    void OnDataLoaded();

    // The player carries a quill.
    bool HasQuill();

    enum class Ink { None, Used, RanDry };

    // Game thread: one use of ink from the player's emptiest inkwell, swapped at once for the
    // next one down.  Ink::RanDry: that was its last use, and it's gone.  Ink::None: no inkwell.
    Ink UseInk();

}
