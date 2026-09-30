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

    // The player's diary doesn't grow: [Diary] PlayerDiaryBooks is off
    // (docs/EDITING.md#diaries-and-journals).  True only for the player (0x14).
    bool PlayerDiaryFrozen(RE::FormID actorFormId);

    // False from kPreLoadGame until the post-load sync has run.  While false, diary
    // events wait (DeferUntilSyncReady): DiaryDB isn't loaded yet, so every actor
    // would look new and get duplicate volumes.
    void SetPostLoadSyncReady(bool ready);
    bool IsPostLoadSyncReady();

    // For the rest of this session, diary events create and update nothing.  Used
    // when the post-load sync couldn't load this save's volumes (no save folder,
    // SkyrimNet never ready): with nothing loaded, every NPC would look new and get a
    // second set of books.  Cleared when the session ends.
    void PauseDiaryBooks();

    // Runs handler(formId) on the game thread once the post-load sync has run,
    // polling every 500 ms.  Dropped if another load starts first (that load's
    // recovery and catch-up scans pick the entry up).
    void DeferUntilSyncReady(RE::FormID formId, void (*handler)(RE::FormID));

    // Post-load, once SkyrimNet has settled its timeline (TimelineGate): checks every
    // volume that ends after the loaded save's game time against the entries
    // SkyrimNet still has.  After Clear those entries are gone, so the volume is
    // re-rendered with its end moved back, or dropped (its book taken back from the
    // NPC and retired) when nothing is left.  After Keep they are all there and
    // nothing changes.  Run after LoadFromDB.
    void ReconcileWithTimeline();

    // kPostLoadGame (revert + KEEP): queues an update for every actor whose latest
    // volume is missing entries SkyrimNet still has.  Skips actors in skipUuids and
    // adds the ones it queues, so the catch-up scan can skip them too.
    void QueueNewEntryRecovery(std::unordered_set<std::string>& skipUuids);

    // kPostLoadGame: pages through all diary entries to find actors that have
    // none of our volumes yet, then creates theirs, one actor per task.
    void QueueBatchCatchUpScan(std::unordered_set<std::string> skipUuids = {});

    // MCM Reset: retires every diary book (removed from the loaded cells; copies
    // elsewhere show the "all entries removed" page) and clears BookManager and DiaryDB
    // tracking (never SkyrimNet's entries).  Returns the number of actors cleared,
    // or -1 on exception.
    int ResetAllDiariesInternal();

} // namespace SkyrimNetDiaries
