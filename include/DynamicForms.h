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

// Runtime forms the engine saves by itself, with no plugin file.  A form made by
// its type's factory gets an 0xFF FormID from the engine; a change record puts it
// in the save, and on load the engine recreates it before inventories resolve.
// The save keeps only the form's flags, so the owner re-applies everything else
// each session, from the Record this module writes to the co-save per form.
//
// A form is never removed from the save once created.  Freed 0xFF FormIDs are
// reused at once (by leveled NPC bases, among others), and a world copy of the form
// in any cell, loaded or not, keeps its base's raw FormID: if the form were
// dropped, that copy would come back with an unrelated base and crash the game.
// A form the owner no longer needs is retired instead: kept, and flagged.
//
// Knows nothing about SkyrimNet Physical Diaries, so it can move into other mods.
// Physical Letters has a copy: keep the two in step (docs/BOOK_FORMS.md, "Reuse in
// other mods").  Thread-safe.  See docs/BOOK_FORMS.md.
namespace DynamicForms {

    // What a form is, in its owner's terms.
    struct Record {
        RE::FormID   formId = 0;
        RE::FormType formType = RE::FormType::None;
        std::string  key;               // the owner's identity for the form
        std::string  templateEditorId;  // the form it takes its look from
        std::string  displayName;
        bool         retired = false;   // no longer used by the owner; kept (see above)
    };

    // Marks a new form persistent: clears kTemporary and adds a change record.
    void MakePersistent(RE::TESForm* a_form);

    // A new persistent form of T, or nullptr if the factory fails.  Track it
    // straight away: an untracked form is still saved, with nothing to fill it in.
    template <class T>
    T* Create()
    {
        auto* factory = RE::IFormFactory::GetConcreteFormFactoryByType<T>();
        T* form = factory ? factory->Create() : nullptr;
        if (form) MakePersistent(form);
        return form;
    }

    // Adds a form's record, or replaces it (by FormID).
    void Track(Record a_record);

    // Flags a tracked form as no longer used.  It stays in the save, so copies of it
    // anywhere stay valid; the owner decides what a retired form shows.
    void Retire(RE::FormID a_formId);

    // Every tracked form's record, retired ones included.
    std::vector<Record> Tracked();

    // Cheap checks for event sinks that run for every reference: whether any
    // tracked form is retired, and whether this one is.
    bool AnyRetired();
    bool IsRetired(RE::FormID a_formId);

    // Co-save: one record of type a_type holding every tracked form.
    void Save(SKSE::SerializationInterface* a_intfc, std::uint32_t a_type);
    // Reads a record written by Save and tracks the forms that still exist with
    // their saved type (the engine renumbers ours if the save gave the ID to
    // another form).  Call from the load callback; the forms are empty shells
    // until the owner fills them in.
    void Load(SKSE::SerializationInterface* a_intfc, std::uint32_t a_version);
    // A new game or a load: forgets the tracked forms.
    void Revert();

    // kPostLoadGame: world copies in loaded cells had their 3D built while the form
    // was still an empty shell, so they are invisible.  Rebuilds it.
    void RebuildLoadedWorldCopies();

} // namespace DynamicForms
