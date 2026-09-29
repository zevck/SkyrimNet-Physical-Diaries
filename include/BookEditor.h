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

// The player writing in their own diary in the book menu.  See docs/EDITING.md.
namespace SkyrimNetDiaries::BookEditor {

    // Once, at kDataLoaded: the book-menu and keyboard sinks and the close hook.
    void Register();

    // A session ended (load or new game): drop the edit state; writes still in SkyrimNet
    // finish without touching the new session.
    void Reset();

} // namespace SkyrimNetDiaries::BookEditor
