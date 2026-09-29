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

// DEV HARNESS for the in-book editor: the edit mode in swf/book's BookMenu.as,
// driven from here (ported from the `letters` branch).  F3 arms edit mode; the next
// book or note opened enters it.  Keys are turned into characters with the active
// keyboard layout and fed to the SWF.  Not tied to diaries or storage yet.
namespace SkyrimNetDiaries::BookEditor {

    // Once, at kDataLoaded: the book-menu and keyboard sinks.
    void Register();

} // namespace SkyrimNetDiaries::BookEditor
