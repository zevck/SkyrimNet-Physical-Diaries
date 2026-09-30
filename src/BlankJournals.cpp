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

#include "BlankJournals.h"
#include "BookManager.h"
#include "Localization.h"
#include "WritingMode.h"

namespace SkyrimNetDiaries::BlankJournals {

    namespace {

        // The three main looks, then the Nightingales' (crafted only, once the player is one),
        // each with the template book whose look it has.
        constexpr const char* kBooks[] = { "SNPD_BlankJournal1", "SNPD_BlankJournal2", "SNPD_BlankJournal3",
                                           "SNPD_BlankJournalN" };
        constexpr const char* kLooks[] = { kJournalTemplates[0], kJournalTemplates[1], kJournalTemplates[2],
                                           kNightingaleTemplate };
        static_assert(std::size(kBooks) == std::size(kLooks));

        std::unordered_map<RE::FormID, std::string> g_looks;  // blank journal → its look; set at kDataLoaded
        constexpr const char* kRecipes[] = { "SNPD_RecipeBlankJournal1", "SNPD_RecipeBlankJournal2",
                                             "SNPD_RecipeBlankJournal3", "SNPD_RecipeBlankJournalN" };
        // Picks one of the three main looks.
        constexpr const char* kList = "SNPD_LItemBlankJournal";
        // Skyrim.esm LItemMiscVendorMiscItems75: the general-goods merchants' misc stock (with
        // the Roll of Paper).  Also rolled by Cupboard01 and PersonalChestSmall.
        constexpr RE::FormID kVendorMiscItems = 0x09AF0A;

        // Adds `ours` (level 1, count 1) to the vendor list in memory, so no plugin overrides the
        // vanilla record and nothing conflicts with mods that do.
        void AddToMerchants(RE::TESLevItem* ours) {
            auto* vendor = RE::TESForm::LookupByID<RE::TESLevItem>(kVendorMiscItems);
            if (!vendor) {
                SKSE::log::warn("[BlankJournals] LItemMiscVendorMiscItems75 not found: merchants won't sell them");
                return;
            }
            auto& entries = vendor->entries;
            const std::size_t count = vendor->numEntries;
            if (entries.size() != count || count >= 255) {
                SKSE::log::warn("[BlankJournals] LItemMiscVendorMiscItems75 has {} entries ({} counted): not added",
                                entries.size(), count);
                return;
            }
            for (const auto& entry : entries) {
                if (entry.form == ours) return;
            }
            // Entries are kept in level order; ours goes after the other level-1 entries.
            std::size_t at = count;
            for (std::size_t i = 0; i < count; ++i) {
                if (entries[i].level > 1) {
                    at = i;
                    break;
                }
            }
            entries.resize(count + 1);
            for (std::size_t i = count; i > at; --i) entries[i] = entries[i - 1];
            entries[at] = RE::LEVELED_OBJECT{ .form = ours, .count = 1, .level = 1, .pad0C = 0, .itemExtra = nullptr };
            vendor->numEntries = static_cast<std::uint8_t>(count + 1);
            SKSE::log::info("[BlankJournals] Added to general-goods merchants' stock ({} entries)", count + 1);
        }

    }

    void OnDataLoaded() {
        if (!WritingMode::IsOn()) {
            // Useless without writing: off the tanning rack (merchants never get them).
            int hidden = 0;
            for (const char* id : kRecipes) {
                if (auto* recipe = RE::TESForm::LookupByEditorID<RE::BGSConstructibleObject>(id)) {
                    recipe->benchKeyword = nullptr;
                    ++hidden;
                }
            }
            SKSE::log::info("[BlankJournals] Writing is off: {} recipe(s) hidden", hidden);
            return;
        }
        const std::string& name = Localization::GetSingleton()->GetBlankJournalName();
        for (std::size_t i = 0; i < std::size(kBooks); ++i) {
            if (auto* book = RE::TESForm::LookupByEditorID<RE::TESObjectBOOK>(kBooks[i])) {
                book->SetFullName(name.c_str());
                g_looks[book->GetFormID()] = kLooks[i];
            } else {
                SKSE::log::warn("[BlankJournals] {} not found", kBooks[i]);
            }
        }
        if (auto* list = RE::TESForm::LookupByEditorID<RE::TESLevItem>(kList)) {
            AddToMerchants(list);
        } else {
            SKSE::log::warn("[BlankJournals] {} not found: merchants won't sell them", kList);
        }
    }

    std::string LookOf(RE::FormID bookFormId) {
        const auto it = g_looks.find(bookFormId);
        return it == g_looks.end() ? std::string{} : it->second;
    }

}
