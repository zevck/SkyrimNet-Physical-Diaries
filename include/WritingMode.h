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
#include "InkAndQuillAPI.h"

// The player writes in their journals through Ink & Quill, when it's installed and its writing is on.
// See docs/EDITING.md#writing-mode.
namespace SkyrimNetDiaries::WritingMode {

    // kPostLoad: finds Ink & Quill's API.
    void Connect();

    // Ink & Quill is installed, new enough, and its book.swf is the one the game loads.  kDataLoaded or later.
    bool IsOn();

    // Ink & Quill's API, or nullptr.
    const IQ_API* API();

}
