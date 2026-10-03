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

// The player writing in their own journals, through Ink & Quill.  See docs/EDITING.md.
namespace SkyrimNetDiaries::BookEditor {

    // Once, at kDataLoaded with writing on (after BlankJournals::OnDataLoaded): Ink & Quill's owner for the
    // journals and the blank journals, the new-entry key.
    void Register();

    // SNPD's keys that act while writing (tear-out, new entry), given to Ink & Quill.  At Register and on MCM changes.
    // False: Ink & Quill refused one, which then won't act while writing (docs/CONFIG_AND_MCM.md).
    bool RegisterKeys();

    // A session ended (load or new game): drop the edit state; writes still in SkyrimNet
    // finish without touching the new session.
    void Reset();

    // Post-load, after ReconcileWithTimeline: a journal with entries in SkyrimNet but no book in this save (made
    // after it, then a KEEP) is made again in the player's inventory (docs/EDITING.md#diaries-and-journals).
    void RestoreLostJournals();

    // Game thread: the player's edits to this book are still being written to SkyrimNet, which has the old text.
    bool HasPendingWrites(RE::FormID a_bookFormId);

} // namespace SkyrimNetDiaries::BookEditor
