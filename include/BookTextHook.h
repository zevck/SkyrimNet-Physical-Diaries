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

// Hooks TESDescription::GetDescription to give our diary books their text: the
// book menu and every other reader ask for it there.  RELOCATION_ID(14399, 14552)
// is (SE, AE); VR reuses the SE id via the VR Address Library.
namespace SkyrimNetDiaries::BookTextHook
{
    void Install();

    // A book text as the book menu gets it: Win-1251 when it contains Cyrillic (Scaleform's
    // pagination needs one byte per character), else unchanged.
    std::string ForBookMenu(const std::string& text);
} // namespace SkyrimNetDiaries::BookTextHook
