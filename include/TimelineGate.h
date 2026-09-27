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

// Holds the post-load sync back until SkyrimNet has settled its timeline.
//
// Loading a save older than SkyrimNet's history makes SkyrimNet ask the player to
// keep or clear the "future" history, and on Clear it deletes those diary entries.
// SkyrimNet's public API has no way to ask whether that decision is still pending,
// so SNPD watches for the prompt itself: a MinHook detour on
// MessageBoxData::QueueMessage recognises SkyrimNet's skynet_DeleteHistoryMessage
// and wraps its callback to see which button was pressed.
// See docs/VOLUMES_AND_SYNC.md.
namespace SkyrimNetDiaries::TimelineGate {

    // SKSEPlugin_Load: install the QueueMessage hook.
    void Install();

    // kDataLoaded: look up SkyrimNet's prompt text (needs an EditorID provider).
    void OnDataLoaded();

    // kPreLoadGame: forget the previous load's prompt and wait state.
    void Reset();

    // Polled on the game thread once SkyrimNet's database is ready.  True once the
    // timeline is settled: nothing from the future, the player chose Keep, or the
    // player chose Clear and SkyrimNet has finished deleting.
    bool IsSettled();

} // namespace SkyrimNetDiaries::TimelineGate
