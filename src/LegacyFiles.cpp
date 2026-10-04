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

#include "LegacyFiles.h"
#include "PluginPaths.h"

#include <algorithm>
#include <filesystem>
#include <vector>

// Copies only: MO2's virtual files don't move or delete well.  The player removes the old ones.
namespace SkyrimNetDiaries::LegacyFiles {

    namespace {
        namespace fs = std::filesystem;

        constexpr std::string_view kOldName = "SkyrimNetPhysicalDiaries";
        constexpr std::string_view kDbFile = "diary.db";
    }

    bool OldDllPresent() {
        std::error_code ec;
        return fs::exists(PluginPaths::PluginsDir() / (std::string(kOldName) + ".dll"), ec);
    }

    void CopySettings() {
        std::error_code ec;
        const auto from = PluginPaths::PluginsDir() / (std::string(kOldName) + ".ini");
        const auto to = PluginPaths::IniPath();
        if (!fs::exists(from, ec) || fs::exists(to, ec)) return;
        if (fs::copy_file(from, to, ec)) {
            SKSE::log::info("[LegacyFiles] Settings copied from {}.ini", kOldName);
        } else {
            SKSE::log::error("[LegacyFiles] Couldn't copy {}.ini: {}", kOldName, ec.message());
        }
    }

    bool CopyDiaryDb(const std::string& saveFolder) {
        std::error_code ec;
        const auto from = PluginPaths::PluginsDir() / kOldName / saveFolder;
        const auto to = PluginPaths::DataDir() / saveFolder;
        if (fs::exists(to / kDbFile, ec) || !fs::exists(from / kDbFile, ec)) return true;
        // SQLite's -wal and -shm first, diary.db last: a copy that fails partway leaves no diary.db.
        std::vector<fs::path> files;
        for (const auto& entry : fs::directory_iterator(from, ec)) {
            if (entry.is_regular_file(ec)) files.push_back(entry.path());
        }
        std::ranges::stable_partition(files, [](const fs::path& p) { return p.filename() != kDbFile; });
        fs::create_directories(to, ec);
        for (const auto& file : files) {
            fs::copy_file(file, to / file.filename(), fs::copy_options::overwrite_existing, ec);
            if (ec) {
                SKSE::log::error("[LegacyFiles] Couldn't copy '{}': {}", file.string(), ec.message());
                return false;
            }
        }
        SKSE::log::info("[LegacyFiles] Copied {} from {}", saveFolder, kOldName);
        return true;
    }

}
