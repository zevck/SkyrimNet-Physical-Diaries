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

// DPF book creation: the serial create queue, DPFCreateCallback and the FormID
// claim table.  BookManager::CreateDiaryBook and SanitizeLoadedBookForms are also
// defined in BookCreation.cpp.  See docs/BOOK_FORMS.md.
namespace SkyrimNetDiaries {

    // Records that formId belongs to actorUuid, so a recycled DPF FormID can't be
    // handed to a second actor.
    void ClaimBookFormId(RE::FormID formId, const std::string& actorUuid);

    // Claims are per-session; cleared from BookManager::ClearActorCache on each load.
    void ClearBookFormIdClaims();

    // Gives a diary form SNPD's look: book type, model and item card from its
    // template, weight, value, no flags, and its name.  Returns true if the name had
    // to change.  Used at creation and on every load, because DPF can restore a form
    // with data (a name) from an earlier owner of the same FormID.
    bool ConfigureDiaryForm(RE::TESObjectBOOK* book, const RE::TESObjectBOOK* templateBook,
                            const std::string& name);

    // True while any of the actor's volumes are queued or being created, i.e. not
    // yet in books_.  Anything that decides "which volumes does this actor have"
    // must wait for these, or it creates the same volumes twice.
    bool HasPendingCreations(const std::string& actorUuid);

    // kPreLoadGame and MCM Reset: drops queued creations and discards results of
    // any Create() still in flight, so they can't register into the new state.
    void CancelPendingCreations();

} // namespace SkyrimNetDiaries
