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
#include <unordered_set>

// Entry -> volume sync: turns SkyrimNet diary entries into volumes, keeps them
// current, and recovers after loads.  See docs/VOLUMES_AND_SYNC.md.
namespace SkyrimNetDiaries {

    // Brings one actor's volumes up to date with SkyrimNet: first-time creation,
    // updating the open volume, sealing a full one and overflowing into new
    // volumes.  Game thread only.
    void UpdateDiaryForActorInternal(RE::FormID formId);

    // kPostLoadGame (revert + KEEP): queues an update for every actor whose latest
    // volume is missing entries SkyrimNet still has.
    void QueueSealedVolumeRecovery(const std::unordered_set<std::string>& skipUuids = {});

    // kPostLoadGame: pages through all diary entries to find actors that have
    // none of our volumes yet, then creates theirs, one actor per task.
    void QueueBatchCatchUpScan(std::unordered_set<std::string> skipUuids = {});

    // MCM Reset: removes every tracked book from loaded inventories and clears
    // BookManager and DiaryDB tracking (never SkyrimNet's entries).  Returns the
    // number of actors cleared, or -1 on exception.
    int ResetAllDiariesInternal();

} // namespace SkyrimNetDiaries
