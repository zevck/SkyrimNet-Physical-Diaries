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

#include "EditorInternal.h"
#include "ActorLookup.h"
#include "BlankJournals.h"
#include "BookTextHook.h"
#include "Config.h"
#include "Localization.h"

// The player's journals: which one the new-entry key opens, and blank journals becoming journals
// (docs/EDITING.md#new-entries, #reading-a-blank-journal).
namespace SkyrimNetDiaries::BookEditor {

    namespace {
        // Open a book and start a new entry once its text is in (BookMenuAdvanceMovie).  Game thread.
        void OpenForNewEntry(RE::FormID bookFormId) {
            auto* book = RE::TESForm::LookupByID<RE::TESObjectBOOK>(bookFormId);
            if (!book) return;
            g_newEntryOnOpen = bookFormId;
            RE::BookMenu::OpenMenuFromBaseForm(book);
        }

        // A new, empty journal beside the player's others, in `look` (a template EditorID), in their
        // inventory.  Its book's FormID, or 0.
        RE::FormID StartJournal(const std::string& look) {
            const std::string uuid = Database::GetUUIDFromFormID(0x14);
            auto* player = RE::PlayerCharacter::GetSingleton();
            if (uuid.empty() || !player) return 0;
            // After every journal SkyrimNet's entries name: a KEEP leaves entries of journals this save lacks.
            bool ok = false;
            const auto written = Database::GetDiaryEntries(0x14, kFetchAllEntries, 0.0, 0.0, &ok, VolumeKind::Written);
            if (!ok) return 0;
            int number = 0;
            for (const auto& entry : written) number = std::max(number, JournalOf(entry));
            auto* manager = BookManager::GetSingleton();
            if (const auto* journals = manager->GetAllVolumesForActor(uuid, VolumeKind::Written)) {
                for (const auto& journal : *journals) number = std::max(number, journal.volumeNumber);
            }
            ++number;
            std::string name = Database::GetActorName(uuid);
            if (name.empty()) name = player->GetName();
            const RE::FormID journal = manager->CreateEmptyVolume(uuid, name, number, 0x14,
                                                                  Database::GetTemplateNameByUUID(uuid), look);
            if (journal != 0) SKSE::log::info("[BookEditor] Made journal {} (0x{:X})", number, journal);
            return journal;
        }

        // Points the open book menu at another book: the engine's globals for the base form and the
        // item's extra data list (cleared).  VR's list address is inferred (docs/DEVELOPMENT.md).
        void SetBookMenuBook(RE::TESObjectBOOK* a_book) {
            static REL::Relocation<RE::ExtraDataList**> extraList{ REL::VariantID(519294, 405834, 0x30111F8) };
            static REL::Relocation<RE::TESObjectBOOK**> book{ REL::VariantID(519295, 405835, 0x3011200) };
            *extraList = nullptr;
            *book = a_book;
        }
    }

    bool JournalFull(const DiaryBookData& vol) {
        const auto pending = g_pending.find(vol.bookFormId);
        const std::size_t count = pending != g_pending.end() && !pending->second.stale
                                      ? pending->second.entries.size()
                                      : static_cast<std::size_t>(std::max(vol.lastKnownEntryCount, 0));
        return count >= static_cast<std::size_t>(Config::GetSingleton()->GetEntriesPerVolume());
    }

    // The journal last written in if carried and not full, else the newest carried one with room.
    void NewEntryFromPlay() {
        auto* loc = Localization::GetSingleton();
        if (!Database::CanWriteDiaries()) {
            Notify(loc->GetEditNeedsSkyrimNet());
            return;
        }
        const std::string uuid = Database::GetUUIDFromFormID(0x14);
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (uuid.empty() || !player) return;
        const auto* journals = BookManager::GetSingleton()->GetAllVolumesForActor(uuid, VolumeKind::Written);
        if (!journals || journals->empty()) {
            Notify(loc->GetEditNoJournal());
            return;
        }
        const auto carried = [player](const DiaryBookData& vol) {
            return CountInInventory(player, RE::TESForm::LookupByID<RE::TESBoundObject>(vol.bookFormId)) > 0;
        };
        const DiaryBookData* pick = nullptr;
        bool carriesOne = false;
        for (auto vol = journals->rbegin(); vol != journals->rend(); ++vol) {
            if (!carried(*vol)) continue;
            carriesOne = true;
            if (JournalFull(*vol)) continue;
            if (!pick || vol->bookFormId == g_lastJournal) pick = &*vol;
        }
        if (!pick) {
            Notify(carriesOne ? loc->GetEditJournalFull() : loc->GetEditJournalNotCarried());
            return;
        }
        OpenForNewEntry(pick->bookFormId);
    }

    // Only from the player's own inventory: in the world, a container, a shop or the gift menu, or
    // without what writing needs, a blank journal is just an empty book.
    void NoteBlankJournal() {
        auto* book = RE::BookMenu::GetTargetForm();
        auto* player = RE::PlayerCharacter::GetSingleton();
        auto* ui = RE::UI::GetSingleton();
        if (!book || !player || !ui || BlankJournals::LookOf(book->GetFormID()).empty()) return;
        if (RE::BookMenu::GetTargetReference() || ui->IsMenuOpen(RE::ContainerMenu::MENU_NAME) ||
            ui->IsMenuOpen(RE::BarterMenu::MENU_NAME) || ui->IsMenuOpen(RE::GiftMenu::MENU_NAME) ||
            CountInInventory(player, book) <= 0) {
            return;
        }
        auto* loc = Localization::GetSingleton();
        if (!Database::CanWriteDiaries()) {
            Notify(loc->GetEditNeedsSkyrimNet());
            return;
        }
        if (!ui->GameIsPaused()) {
            // The conversion runs as a UI task, on the game thread only while the menu pauses it.
            Notify(loc->GetEditNeedsPause());
            return;
        }
        g_blankOnOpen = book->GetFormID();
    }

    // Without closing the book (the two share the model): the new journal, the blank used up, the
    // menu pointed at the journal and showing its pages.
    void ConvertBlankJournal() {
        auto* blank = RE::BookMenu::GetTargetForm();
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!blank || !player) return;
        auto* loc = Localization::GetSingleton();
        const std::string look = BlankJournals::LookOf(blank->GetFormID());
        const RE::FormID next = StartJournal(look);
        auto* journal = RE::TESForm::LookupByID<RE::TESObjectBOOK>(next);
        if (!journal) {
            SKSE::log::error("[BookEditor] Couldn't start a journal from blank journal 0x{:X}", blank->GetFormID());
            Notify(loc->GetBlankJournalFailed());
            return;
        }
        player->RemoveItem(blank, 1, RE::ITEM_REMOVE_REASON::kRemove, nullptr, nullptr);
        SetBookMenuBook(journal);
        if (const auto* vol = BookManager::GetSingleton()->GetBookForFormID(next); vol && BookMovie()) {
            RE::GFxValue text;
            const std::string forMenu = BookTextHook::ForBookMenu(vol->cachedBookText);
            text.SetString(forMenu.c_str());
            BookMovie()->Invoke("_root.BookMenu_mc.ReplaceBookText", nullptr, &text, 1);
        }
        SKSE::log::info("[BookEditor] Blank journal 0x{:X} became journal 0x{:X} ({})", blank->GetFormID(), next, look);
        g_lastJournal = next;
        Notify(loc->GetEditStartedVolume());
    }

} // namespace SkyrimNetDiaries::BookEditor
