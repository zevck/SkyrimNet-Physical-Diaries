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

#include "WritingMode.h"
#include <charconv>
#include <fstream>
#include <iterator>

namespace SkyrimNetDiaries::WritingMode {

    namespace {

        // BookMenu.as WRITING_INTERFACE: "BOOKMENU_WRITING_INTERFACE=<n>".  Mod-neutral: the
        // Physical Letters mod ships the same book.swf.  A SWF only adds calls, so any version
        // from kMinInterface up will do; raise kMinInterface when the plugin needs a newer call.
        constexpr std::string_view kMarker = "BOOKMENU_WRITING_INTERFACE=";
        constexpr int kMinInterface = 1;

        bool g_on = false;

    }

    void Detect() {
        g_on = false;
        std::ifstream file("Data/Interface/book.swf", std::ios::binary);
        if (!file) {
            SKSE::log::info("[WritingMode] Off: no loose Interface/book.swf (the game's own, or one in a BSA)");
            return;
        }
        const std::string data{ std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>() };
        if (data.size() < 3 || data.compare(0, 3, "FWS") != 0) {
            // CWS/ZWS: compressed, so not ours (ours ships uncompressed) or it was re-saved.
            SKSE::log::info("[WritingMode] Off: Interface/book.swf has no writing (compressed, {} bytes)", data.size());
            return;
        }
        const auto at = data.find(kMarker);
        if (at == std::string::npos) {
            SKSE::log::info("[WritingMode] Off: Interface/book.swf is another mod's, without writing");
            return;
        }
        int version = 0;
        const char* first = data.data() + at + kMarker.size();
        std::from_chars(first, data.data() + data.size(), version);
        if (version < kMinInterface) {
            SKSE::log::error("[WritingMode] Off: Interface/book.swf is too old (interface {}, this plugin needs {} or "
                             "newer). Reinstall SNPD.", version, kMinInterface);
            return;
        }
        g_on = true;
        SKSE::log::info("[WritingMode] On: Interface/book.swf has writing (interface {})", version);
    }

    bool IsOn() { return g_on; }

}
