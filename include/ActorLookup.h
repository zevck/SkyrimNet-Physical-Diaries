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

namespace SkyrimNetDiaries {

    // The NPC owning a volume: player special case, UUID -> live FormID, then the stored FormID only if it
    // maps back to the same UUID.  Hits cached by UUID per session.  See docs/BOOK_FORMS.md.
    RE::Actor* FindActorForBook(RE::FormID targetFormID,
                                const std::string& actorName,
                                const std::string& bioTemplate,
                                const std::string& actorUuid);

    // How many of `item` the reference holds (0 if none).
    std::int32_t CountInInventory(RE::TESObjectREFR* ref, const RE::TESBoundObject* item);

    // Clears the UUID -> actor cache.  Called on each load.
    void ClearActorLookupCache();

} // namespace SkyrimNetDiaries
