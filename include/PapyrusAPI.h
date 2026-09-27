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

namespace PapyrusAPI {
    // Register all native Papyrus functions
    void Register();
    
    // Get the theft status of an actor's diary as JSON string
    // Returns JSON like: {"stolen": true, "chronicled": false} or {"stolen": false}
    RE::BSFixedString GetDiaryTheftStatus(RE::StaticFunctionTag*, RE::Actor* akActor);
    
    // Check if an actor's diary is currently stolen (simple boolean check)
    // Returns "true" if stolen and not yet resolved, "false" otherwise
    RE::BSFixedString IsDiaryStolen(RE::StaticFunctionTag*, RE::Actor* akActor);
    
    // Record that an actor has chronicled their stolen diary (clears theft state)
    // Call this when an NPC writes a diary entry
    void SetTheftCleared(RE::StaticFunctionTag*, RE::Actor* akActor);
}
