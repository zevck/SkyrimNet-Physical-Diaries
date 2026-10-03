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

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

namespace SkyrimNetDiaries::WritingMode {

    namespace {
        const IQ_API* g_api = nullptr;
    }

    void Connect() {
        auto* module = GetModuleHandleA("InkAndQuill.dll");
        const auto get = module ? reinterpret_cast<IQ_GetAPI_t>(GetProcAddress(module, "IQ_GetAPI")) : nullptr;
        g_api = get ? get(IQ_API_VERSION) : nullptr;
        if (g_api) {
            SKSE::log::info("[WritingMode] Ink & Quill found (API {})", g_api->version);
        } else if (module) {
            SKSE::log::info("[WritingMode] Off: Ink & Quill is too old (needs API {})", IQ_API_VERSION);
        } else {
            SKSE::log::info("[WritingMode] Off: Ink & Quill isn't installed");
        }
    }

    bool IsOn() { return g_api && g_api->IsWritingOn(); }

    const IQ_API* API() { return g_api; }

}
