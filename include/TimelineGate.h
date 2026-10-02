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

// Holds the post-load sync back until SkyrimNet's keep/clear check after a load has resolved (its public API v11
// timeline state).  See docs/VOLUMES_AND_SYNC.md#waiting-for-the-decision-timelinegate.
namespace SkyrimNetDiaries::TimelineGate {

    // kPreLoadGame: forget the previous load's state.
    void Reset();

    // Polled on the game thread once SkyrimNet's database is ready.  True once the check has resolved: nothing
    // from the future, the player chose Keep, or chose Clear and SkyrimNet has finished deleting.
    bool IsSettled();

    // What happened to SkyrimNet's history on this load, for logging:
    // "no prompt", "unanswered", "Keep" or "Clear".
    std::string_view Outcome();

} // namespace SkyrimNetDiaries::TimelineGate
