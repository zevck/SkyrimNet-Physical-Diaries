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

namespace DiaryTheftHandler {
    // Register the event handler for diary theft detection
    void Register();

    // Registers the snpd_diary_stolen decorator with SkyrimNet.  SkyrimNet clears
    // decorator registrations on every load, so call this on every kPostLoadGame.
    void RegisterStolenDecorator();

    // kPostLoadGame: if this save is earlier in game time than the last session,
    // clears each actor's stolen volumes (the theft belongs to an abandoned
    // timeline), then records the current game time.  See docs/THEFT.md.
    void ReconcileAfterLoad();
}
