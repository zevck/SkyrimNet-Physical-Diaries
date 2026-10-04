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

#include <string>

// 1.x's file names (SkyrimNetPhysicalDiaries.*), renamed in 2.0.  See docs/ARCHITECTURE.md#names.
namespace SkyrimNetDiaries::LegacyFiles {

    // 1.x's DLL is still installed: it would run beside this one, on the same files.
    bool OldDllPresent();

    // 1.x's INI copied to the new name if there's none yet.  Before the config loads.
    void CopySettings();

    // This save's DiaryDB copied from 1.x's folder if the new one has none.  False: it's there but couldn't be.
    bool CopyDiaryDb(const std::string& saveFolder);

}
