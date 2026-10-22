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
        // Skyrim.esm LItemMiscVendorMiscItems75: the general-goods merchants' misc stock (with the Roll of Paper): a chest
        // that stocks it is a general store.
        constexpr RE::FormID kVendorMiscItems = 0x09AF0A;

        // Vanilla's generic world containers (hundreds in houses) stock the misc list too: a merchant using one would put
        // journals in all of them.  Same walk as Ink & Quill's and Physical Letters': fix all three.
        bool IsGenericContainer(const RE::TESObjectCONT* chest) {
            static constexpr std::pair<RE::FormID, std::string_view> kGeneric[] = { { 0x024CA4, "Skyrim.esm" }, { 0x09AF19, "Skyrim.esm" },
                                                                                   { 0x03C361, "Dragonborn.esm" } };
            auto* data = RE::TESDataHandler::GetSingleton();
            for (const auto& [id, plugin] : kGeneric) {
                if (data && data->LookupForm(id, plugin) == chest) return true;  // Cupboard01, PersonalChestSmall(_NoRespawn)
            }
            return false;
        }

        // `list` as one more entry of `chest`, without extra data: the engine writes out of bounds rolling a leveled entry
        // whose health isn't 1.0, and CommonLib's AddObjectToContainer gives it 100 (docs/EDITING.md#blank-journals).
        bool AddToChest(RE::TESObjectCONT* chest, RE::TESLevItem* list) {
            if (!chest->AddObjectToContainer(list, 1, nullptr)) return false;
            for (std::uint32_t i = 0; i < chest->numContainerObjects; ++i) {
                auto* entry = chest->containerObjects[i];
                if (!entry || entry->obj != list || !entry->itemExtra) continue;
                delete entry->itemExtra;  // the game's heap (TES_HEAP_REDEFINE_NEW)
                entry->itemExtra = nullptr;
            }
            return true;
        }

        // Every general-goods merchant's chest (a vendor faction's, stocking vanilla's misc list) gets `ours` as its own
        // entry, in memory: every restock has a journal, and no record is overridden.
        void AddToMerchants(RE::TESLevItem* ours) {
            auto* data = RE::TESDataHandler::GetSingleton();
            auto* generalGoods = RE::TESForm::LookupByID<RE::TESLevItem>(kVendorMiscItems);
            if (!data || !generalGoods) {
                SKSE::log::warn("[BlankJournals] LItemMiscVendorMiscItems75 not found: merchants won't sell them");
                return;
            }
            int stocked = 0;
            for (auto* faction : data->GetFormArray<RE::TESFaction>()) {
                if (!faction || !faction->IsVendor() || !faction->vendorData.merchantContainer) continue;
                auto* base = faction->vendorData.merchantContainer->GetBaseObject();
                auto* chest = base ? base->As<RE::TESObjectCONT>() : nullptr;
                // Not a general store, or already stocked (several merchants can share a chest).
                if (!chest || chest->GetObjectCount(generalGoods) == 0 || chest->GetObjectCount(ours) > 0) continue;
                if (IsGenericContainer(chest)) {
                    SKSE::log::warn("[BlankJournals] Vendor faction {:08X}'s chest is a generic world container ({:08X}): not stocked there",
                                    faction->GetFormID(), chest->GetFormID());
                    continue;
                }
                if (!AddToChest(chest, ours)) continue;
                ++stocked;
                SKSE::log::info("[BlankJournals] Stocked chest {:08X} '{}' (vendor faction {:08X})", chest->GetFormID(),
                                chest->GetFormEditorID() ? chest->GetFormEditorID() : "", faction->GetFormID());
            }
            SKSE::log::info("[BlankJournals] Always stocked by {} general-goods merchant chest(s)", stocked);
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

    std::vector<RE::FormID> Forms() {
        std::vector<RE::FormID> forms;
        for (const auto& [form, look] : g_looks) forms.push_back(form);
        return forms;
    }

    std::string LookOf(RE::FormID bookFormId) {
        const auto it = g_looks.find(bookFormId);
        return it == g_looks.end() ? std::string{} : it->second;
    }

}
