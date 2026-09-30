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

#include "WritingTools.h"
#include "ActorLookup.h"
#include <array>

namespace SkyrimNetDiaries::WritingTools {

    namespace {

        // Writing sessions a full inkwell lasts.
        constexpr int kUses = 10;

        RE::BGSListForm* g_quills = nullptr;    // SNPD_Quills
        RE::BGSListForm* g_inkwells = nullptr;  // SNPD_Inkwells: full ones
        // g_partly[n]: the inkwell with n uses left (SNPD_Inkwell<n>), 1 to kUses - 1.
        std::array<RE::TESBoundObject*, kUses> g_partly{};

        bool Carries(RE::PlayerCharacter* player, RE::TESBoundObject* item) {
            return item && CountInInventory(player, item) > 0;
        }

        // A full inkwell the player carries: any form in SNPD_Inkwells.
        RE::TESBoundObject* FullInkwell(RE::PlayerCharacter* player) {
            if (!g_inkwells) return nullptr;
            RE::TESBoundObject* found = nullptr;
            g_inkwells->ForEachForm([&](RE::TESForm* form) {
                auto* item = form ? form->As<RE::TESBoundObject>() : nullptr;
                if (Carries(player, item)) {
                    found = item;
                    return RE::BSContainer::ForEachResult::kStop;
                }
                return RE::BSContainer::ForEachResult::kContinue;
            });
            return found;
        }

    }

    void OnDataLoaded() {
        g_quills = RE::TESForm::LookupByEditorID<RE::BGSListForm>("SNPD_Quills");
        g_inkwells = RE::TESForm::LookupByEditorID<RE::BGSListForm>("SNPD_Inkwells");
        // The clones' name is the ESP's English one: use the game's, in the game's language.
        const auto* vanilla = RE::TESForm::LookupByID<RE::TESObjectMISC>(0x04C3C6);  // Inkwell01
        const char* name = vanilla ? vanilla->GetName() : nullptr;
        int found = 0;
        for (int uses = 1; uses < kUses; ++uses) {
            auto* inkwell = RE::TESForm::LookupByEditorID<RE::TESObjectMISC>(std::format("SNPD_Inkwell{}", uses));
            g_partly[uses] = inkwell;
            if (!inkwell) continue;
            ++found;
            if (name && *name) inkwell->SetFullName(name);
        }
        if (!g_quills || !g_inkwells || found != kUses - 1) {
            SKSE::log::error("[WritingTools] The ESP's quills, inkwells or partly used inkwells are missing ({} of {}): "
                             "nobody can write",
                             found, kUses - 1);
        }
    }

    bool HasQuill() {
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player || !g_quills) return false;
        bool found = false;
        g_quills->ForEachForm([&](RE::TESForm* form) {
            found = form && Carries(player, form->As<RE::TESBoundObject>());
            return found ? RE::BSContainer::ForEachResult::kStop : RE::BSContainer::ForEachResult::kContinue;
        });
        return found;
    }

    Ink UseInk() {
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player) return Ink::None;
        // The emptiest first, so there's only ever one partly used inkwell.
        int uses = 0;
        RE::TESBoundObject* inkwell = nullptr;
        for (int n = 1; n < kUses && !inkwell; ++n) {
            if (Carries(player, g_partly[n])) {
                inkwell = g_partly[n];
                uses = n;
            }
        }
        if (!inkwell) {
            inkwell = FullInkwell(player);
            uses = kUses;
        }
        if (!inkwell) return Ink::None;

        player->RemoveItem(inkwell, 1, RE::ITEM_REMOVE_REASON::kRemove, nullptr, nullptr);
        if (uses == 1) {
            SKSE::log::info("[WritingTools] An inkwell ran dry");
            return Ink::RanDry;
        }
        auto* next = g_partly[uses - 1];
        if (!next) {
            SKSE::log::error("[WritingTools] No inkwell with {} uses left in the ESP: the inkwell is gone", uses - 1);
            return Ink::RanDry;
        }
        player->AddObjectToContainer(next, nullptr, 1, nullptr);
        SKSE::log::info("[WritingTools] Used ink: an inkwell has {} uses left", uses - 1);
        return Ink::Used;
    }

}
