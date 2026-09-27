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

namespace SkyrimNetDiaries::DiaryTheftHandler {
    // Register the event handler for diary theft detection
    void Register();

    // The NPC wrote about their missing diary (or a mod said so): clears every stolen
    // volume.
    void ClearStolenVolumes(const std::string& actorUuid);

    // Registers the snpd_diary_stolen decorator with SkyrimNet.  SkyrimNet clears
    // decorator registrations on every load, so call this on every kPostLoadGame.
    void RegisterStolenDecorator();

    // kPostLoadGame: drops thefts recorded after the loaded save's game time (they
    // belong to a timeline the player has left).  See docs/THEFT.md.
    void ReconcileAfterLoad();
} // namespace SkyrimNetDiaries::DiaryTheftHandler
