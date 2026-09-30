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
#include "Database.h"  // VolumeKind

// Diary book forms: engine-persisted runtime books (DynamicForms), one per volume.
// BookManager::CreateDiaryBook is also defined in BookCreation.cpp.  See
// docs/BOOK_FORMS.md.
namespace SkyrimNetDiaries {

    // A volume's key in its co-save record: "<actor UUID>|v<volume number>" for a diary,
    // "<actor UUID>|j<volume number>" for the player's journal.
    std::string VolumeKey(const std::string& actorUuid, VolumeKind kind, int volumeNumber);
    // Splits a VolumeKey.  False if `key` isn't one.
    bool ParseVolumeKey(const std::string& key, std::string& actorUuid, VolumeKind& kind, int& volumeNumber);

    // Gives a diary form SNPD's look: book type, models, bounds, sounds, keywords and
    // item card from its template, weight, value, no flags, and its name.  Returns
    // true if the name had to change.  The save keeps none of this, so it runs at
    // creation and on every load.
    bool ConfigureDiaryForm(RE::TESObjectBOOK* book, const RE::TESObjectBOOK* templateBook,
                            const std::string& name);

    // Load callback, after DynamicForms::Load: fills in this save's diary books from
    // their co-save records, so they look right before DiaryDB is open.
    void ConfigureLoadedBooks();

    // Retired books stay in the save but must never be seen (docs/BOOK_FORMS.md#retirement).
    // Game thread: removes their copies from the loaded cells, owners and merchant chests.
    void SweepRetiredBooks();
    // Once at plugin load: removes them from references as their cells attach.
    void RegisterRetiredBookSweeper();

} // namespace SkyrimNetDiaries
