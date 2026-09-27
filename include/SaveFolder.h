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

// The loaded character's SkyrimNet save folder ("SkyrimNet-<id>").  DiaryDB is
// keyed by it.  See docs/DATABASE.md.
namespace SkyrimNetDiaries::SaveFolder {

    const std::string& Get();
    void Set(std::string folder);
    void Clear();

    // Parses SkyrimNet.log for the last "Using save ID: " line, checks that the
    // matching SkyrimNet .db exists, and caches the result.  Returns the cached
    // value if one is already set, "" on failure.
    std::string DetectFromLog();

} // namespace SkyrimNetDiaries::SaveFolder
