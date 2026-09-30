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
#include <MinHook.h>

namespace SkyrimNetDiaries {

    // MinHook detour on `target`, trampoline in `*original`: copes with other plugins hooking the same
    // function.  Logs the outcome; `onFailure` says what stops working.
    inline bool InstallDetour(std::uintptr_t target, void* detour, void** original,
                              std::string_view what, std::string_view ids, std::string_view onFailure) {
        const auto init = MH_Initialize();
        if (init != MH_OK && init != MH_ERROR_ALREADY_INITIALIZED) {
            SKSE::log::error("MH_Initialize failed ({}) — {}", MH_StatusToString(init), onFailure);
            return false;
        }
        auto* targetPtr = reinterpret_cast<void*>(target);
        auto status = MH_CreateHook(targetPtr, detour, original);
        if (status == MH_OK) {
            status = MH_EnableHook(targetPtr);
        }
        if (status != MH_OK) {
            SKSE::log::error("{} hook failed ({}) — {}", what, MH_StatusToString(status), onFailure);
            return false;
        }
        SKSE::log::info("Installed {} hook (RELOCATION_ID {})", what, ids);
        return true;
    }

} // namespace SkyrimNetDiaries
