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

    // Brings one actor's volumes up to date with SkyrimNet: first-time creation, updating the open volume, sealing
    // a full one and overflowing into new volumes.  Game thread only.
    void UpdateDiaryForActorInternal(RE::FormID formId);

    // The player's diary doesn't grow: [Diary] PlayerDiaryBooks is off
    // (docs/EDITING.md#diaries-and-journals).  True only for the player (0x14).
    bool PlayerDiaryFrozen(RE::FormID actorFormId);

    // False from kPreLoadGame until the post-load sync has run.  Diary events wait meanwhile: before DiaryDB is
    // loaded every actor would look new and get duplicate volumes.
    void SetPostLoadSyncReady(bool ready);
    bool IsPostLoadSyncReady();

    // For the rest of this session diary events do nothing: the post-load sync couldn't load this save's volumes,
    // so every NPC would get a second set of books (docs/ARCHITECTURE.md#startup-and-load-sequence).
    void PauseDiaryBooks();

    // Runs handler(formId) on the game thread once the post-load sync has run.  Dropped if another load starts
    // first (that load's recovery and catch-up scans pick the entry up).
    void DeferUntilSyncReady(RE::FormID formId, void (*handler)(RE::FormID));

    // Post-load, right after LoadFromDB: diary volumes still split by date (last_id -1) are re-cut by entry id,
    // each keeping its size (docs/DATABASE.md#schema-changes).
    void MigrateVolumesToIds();

    // Post-load, after TimelineGate and LoadFromDB: re-renders or drops volumes showing entries dated after the
    // save that SkyrimNet cleared (docs/VOLUMES_AND_SYNC.md#reconciling-reconcilewithtimeline).
    void ReconcileWithTimeline();

    // kPostLoadGame (revert + KEEP): queues an update for actors whose latest volume misses entries SkyrimNet has.
    // Skips skipUuids and adds the ones it queues, so the catch-up scan skips them too.
    void QueueNewEntryRecovery(std::unordered_set<std::string>& skipUuids);

    // kPostLoadGame: pages through all diary entries to find actors that have
    // none of our volumes yet, then creates theirs, one actor per task.
    void QueueBatchCatchUpScan(std::unordered_set<std::string> skipUuids = {});

    // MCM Reset: retires every diary book and clears BookManager and DiaryDB tracking (never SkyrimNet's entries).
    // Returns the number of actors cleared, or -1 on exception.
    int ResetAllDiariesInternal();

} // namespace SkyrimNetDiaries
