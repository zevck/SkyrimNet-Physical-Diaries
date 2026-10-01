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
#include <optional>

// NPCs writing diary entries on their own, once a game day, through SkyrimNet's own diary
// generation.  Optional and off by default.  See docs/NPC_DIARIES.md.
namespace SkyrimNetDiaries::NpcDiaries {

    // kDataLoaded: our records, the dialogue topics' localized text, the clock and the menu sink.
    void OnDataLoaded();

    // The dialogue's global follows [NpcDiaries] Enabled: after a load (saves keep globals) and MCM changes.
    void SyncEnabled();

    // The co-save: the game day the diaries last ran, and the NPCs asked to write daily.
    void Save(SKSE::SerializationInterface* a_intfc, std::uint32_t a_type);
    void Load(SKSE::SerializationInterface* a_intfc, std::uint32_t a_version);
    void Revert();

    // The MCM: SkyrimNet's own diary settings, "SkyrimNetDiaries" (enabled) and "SkyrimNetDayBoundary"
    // (respect_day_boundary), as last read, and changed through its Papyrus PatchConfig (saved to its file).
    std::optional<bool> GetSkyrimNetDiarySetting(std::string_view a_key);
    void RefreshSkyrimNetSettings();  // the MCM opens: read them again (answers before the page is drawn, normally)
    bool SetSkyrimNetDiarySetting(std::string_view a_key, bool a_value);

    // Papyrus (the dialogue's fragments): an NPC starts or stops writing every day.
    void DailyDiaryChanged(RE::Actor* a_actor, bool a_daily);

}  // namespace SkyrimNetDiaries::NpcDiaries
