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

#include <filesystem>
#include <string>
#include <string_view>

// Where SNPD's files live: the INI and the data folder (Locales, DiaryDB).  See docs/ARCHITECTURE.md#names.
namespace SkyrimNetDiaries::PluginPaths {

    inline constexpr std::string_view kName = "PhysicalDiaries";

    inline std::filesystem::path PluginsDir() {
        return std::filesystem::current_path() / "Data" / "SKSE" / "Plugins";
    }
    inline std::filesystem::path DataDir() { return PluginsDir() / kName; }
    inline std::filesystem::path IniPath() { return PluginsDir() / (std::string(kName) + ".ini"); }

}
