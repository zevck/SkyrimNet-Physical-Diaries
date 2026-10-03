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
#include "Config.h"
#include "Localization.h"

#include <set>

// The player's journals: which one the new-entry key opens, and making a new one (docs/EDITING.md#new-entries,
// #reading-a-blank-journal).
namespace SkyrimNetDiaries::BookEditor {

    namespace {
        // Open a journal and begin a new entry once its menu is open.  Game thread.
        void OpenForNewEntry(RE::FormID bookFormId) {
            auto* book = RE::TESForm::LookupByID<RE::TESObjectBOOK>(bookFormId);
            if (!book) return;
            RE::BookMenu::OpenMenuFromBaseForm(book);
            BeginNewEntryOnOpen(bookFormId);
        }

        // The journal numbers SkyrimNet's entries name (untagged ones are journal 1's), or nullopt.
        std::optional<std::set<int>> TaggedJournals() {
            bool ok = false;
            const auto written = Database::GetDiaryEntries(0x14, kFetchAllEntries, 0.0, 0.0, &ok, VolumeKind::Written);
            if (!ok) return std::nullopt;
            std::set<int> numbers;
            for (const auto& entry : written) numbers.insert(std::max(JournalOf(entry), 1));
            return numbers;
        }

        RE::FormID MakeJournal(const JournalOwner& owner, int number, const std::string& look) {
            return BookManager::GetSingleton()->CreateEmptyVolume(owner.uuid, owner.name, number, 0x14,
                                                                  Database::GetTemplateNameByUUID(owner.uuid), look);
        }
    }

    std::optional<JournalOwner> PlayerJournalOwner() {
        const std::string uuid = Database::GetUUIDFromFormID(0x14);
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (uuid.empty() || !player) return std::nullopt;
        std::string name = Database::GetActorName(uuid);
        if (name.empty()) name = player->GetName();
        return JournalOwner{ uuid, std::move(name) };
    }

    RE::FormID StartJournal(const std::string& look) {
        const auto owner = PlayerJournalOwner();
        // After every journal SkyrimNet's entries name: a KEEP leaves entries of journals this save lacks.
        const auto tagged = owner ? TaggedJournals() : std::nullopt;
        if (!tagged) return 0;
        int number = tagged->empty() ? 0 : *tagged->rbegin();
        if (const auto* journals = BookManager::GetSingleton()->GetAllVolumesForActor(owner->uuid, VolumeKind::Written)) {
            for (const auto& journal : *journals) number = std::max(number, journal.volumeNumber);
        }
        const RE::FormID journal = MakeJournal(*owner, ++number, look);
        if (journal != 0) SKSE::log::info("[BookEditor] Made journal {} (0x{:X})", number, journal);
        return journal;
    }

    void RestoreLostJournals() {
        auto* manager = BookManager::GetSingleton();
        const auto looks = manager->TakeLostJournalLooks();
        const auto owner = PlayerJournalOwner();
        auto numbers = owner ? TaggedJournals() : std::nullopt;
        if (!numbers) return;
        if (const auto* journals = manager->GetAllVolumesForActor(owner->uuid, VolumeKind::Written)) {
            for (const auto& journal : *journals) numbers->erase(journal.volumeNumber);
        }
        int restored = 0;
        for (const int number : *numbers) {
            const auto look = looks.find(number);
            const RE::FormID book = MakeJournal(*owner, number, look != looks.end() ? look->second : std::string());
            auto* vol = book ? manager->GetBookForFormID(book) : nullptr;
            if (!vol) {
                SKSE::log::error("[Restore] Journal {}: has entries but couldn't be made again", number);
                continue;
            }
            bool ok = false;
            const auto live = manager->GetLiveEntries(*vol, 0x14, &ok);
            if (ok) manager->SetVolumeText(*vol, live);
            SKSE::log::info("[Restore] Journal {}: not in this save, {} entries in SkyrimNet: made again (0x{:X}, {})",
                            number, live.size(), book, look != looks.end() ? "its look" : "a new look");
            ++restored;
        }
        if (restored > 0) Notify(Localization::GetSingleton()->GetJournalRestored());
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
        const auto owner = PlayerJournalOwner();
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!owner || !player) return;
        const auto* journals = BookManager::GetSingleton()->GetAllVolumesForActor(owner->uuid, VolumeKind::Written);
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

} // namespace SkyrimNetDiaries::BookEditor
