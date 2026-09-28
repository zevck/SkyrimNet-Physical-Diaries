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

#include "BookManager.h"
#include "ActorLookup.h"
#include "BookCreation.h"
#include "BookText.h"
#include "Database.h"
#include "DiaryDB.h"
#include "DynamicForms.h"
#include "Localization.h"
#include <algorithm>
#include <map>
#include <mutex>

namespace SkyrimNetDiaries {

    BookManager* BookManager::GetSingleton() {
        static BookManager singleton;
        return &singleton;
    }

    namespace {
        // In-memory volume <-> DiaryDB row.  Used by RegisterBook, FlushToDB and
        // LoadFromDB so the three can't drift apart.
        DiaryDB::VolumeRow ToRow(const DiaryBookData& d) {
            DiaryDB::VolumeRow r;
            r.actorUuid                  = d.actorUuid;
            r.actorName                  = d.actorName;
            r.actorFormId                = static_cast<std::uint32_t>(d.actorFormId);
            r.bookFormId                 = static_cast<std::uint32_t>(d.bookFormId);
            r.volumeNumber               = d.volumeNumber;
            r.startTime                  = d.startTime;
            r.endTime                    = d.endTime;
            r.journalTemplate            = d.journalTemplate;
            r.bioTemplateName            = d.bioTemplateName;
            r.lastKnownEntryCount        = d.lastKnownEntryCount;
            r.prevVolumeLastCreationTime = d.prevVolumeLastCreationTime;
            r.prevVolumeCountAtBoundary  = d.prevVolumeCountAtBoundary;
            r.bookText                   = d.cachedBookText;  // "" keeps the stored text
            return r;
        }

        DiaryBookData FromRow(const DiaryDB::VolumeRow& r) {
            DiaryBookData d;
            d.actorUuid                  = r.actorUuid;
            d.actorName                  = r.actorName;
            d.actorFormId                = static_cast<RE::FormID>(r.actorFormId);
            d.bookFormId                 = static_cast<RE::FormID>(r.bookFormId);
            d.volumeNumber               = r.volumeNumber;
            d.startTime                  = r.startTime;
            d.endTime                    = r.endTime;
            d.journalTemplate            = r.journalTemplate;
            d.bioTemplateName            = r.bioTemplateName;
            d.lastKnownEntryCount        = r.lastKnownEntryCount;
            d.prevVolumeLastCreationTime = r.prevVolumeLastCreationTime;
            d.prevVolumeCountAtBoundary  = r.prevVolumeCountAtBoundary;
            d.cachedBookText             = r.bookText;
            return d;
        }
    }

    namespace {
        // Karliah, Gallus and Mercer Frey (Skyrim.esm NPC_ records) get the Nightingale
        // journal.  Matched by the actor's base form, never by name: names are
        // localized, and SkyrimNet display names can differ from the engine's.
        bool IsNightingale(RE::FormID actorFormId) {
            static constexpr RE::FormID kNightingaleNPCs[] = {
                0x0001B07F,  // Karliah
                0x0001BB5D,  // Gallus
                0x0001B07C,  // MercerFrey
            };
            auto* actor = RE::TESForm::LookupByID<RE::Actor>(actorFormId);
            auto* base = actor ? actor->GetActorBase() : nullptr;
            return base && std::find(std::begin(kNightingaleNPCs), std::end(kNightingaleNPCs),
                                     base->GetFormID()) != std::end(kNightingaleNPCs);
        }
    }

    std::string BookManager::SelectJournalTemplate(const std::string& actorUuid, const std::string& actorName,
                                                   RE::FormID actorFormId) {
        // Check if we already selected a template for this actor
        // An empty name (a legacy row that only held last_known_game_time) is no choice.
        auto it = actorTemplates_.find(actorUuid);
        if (it != actorTemplates_.end() && !it->second.empty()) {
            SKSE::log::debug("Using cached journal template for {}: {}", actorName, it->second);
            return it->second;
        }

        std::string selectedTemplate;

        if (IsNightingale(actorFormId)) {
            selectedTemplate = kNightingaleTemplate;
        } else {
            // Pick a variant from the actor's UUID so the choice is stable across reloads.
            selectedTemplate = kJournalTemplates[std::hash<std::string>{}(actorUuid) % std::size(kJournalTemplates)];
        }

        SKSE::log::debug("Selected journal template for {}: {}", actorName, selectedTemplate);
        actorTemplates_[actorUuid] = selectedTemplate;
        DiaryDB::GetSingleton()->UpsertActorTemplate(actorUuid, selectedTemplate);
        return selectedTemplate;
    }

    DiaryBookData* BookManager::GetBookForActor(const std::string& actorUuid) {
        auto it = books_.find(actorUuid);
        if (it == books_.end() || it->second.empty()) {
            return nullptr;
        }
        // Return the latest (last) volume
        return &it->second.back();
    }

    std::vector<DiaryBookData>* BookManager::GetAllVolumesForActor(const std::string& actorUuid) {
        auto it = books_.find(actorUuid);
        if (it == books_.end()) {
            return nullptr;
        }
        return &it->second;
    }

    DiaryBookData* BookManager::GetBookForFormID(RE::FormID formId) {
        const auto entry = formIndex_.find(formId);
        if (entry == formIndex_.end()) return nullptr;
        const auto actor = books_.find(entry->second.first);
        if (actor == books_.end()) return nullptr;
        for (auto& book : actor->second) {
            if (book.volumeNumber == entry->second.second && book.bookFormId == formId) {
                return &book;
            }
        }
        return nullptr;
    }

    RE::FormID BookManager::FindBookByDescription(const RE::TESDescription* description) const {
        std::lock_guard lock{ snapshotMutex_ };
        const auto entry = descriptionIndex_.find(description);
        return entry == descriptionIndex_.end() ? 0 : entry->second;
    }

    std::string BookManager::TextSnapshotLocked(RE::FormID bookFormId) const {
        const auto entry = textSnapshot_.find(bookFormId);
        return entry == textSnapshot_.end() ? std::string{} : entry->second;
    }

    std::string BookManager::GetBookTextSnapshot(RE::FormID bookFormId) const {
        std::lock_guard lock{ snapshotMutex_ };
        return TextSnapshotLocked(bookFormId);
    }

    std::string BookManager::GetBookTextSnapshot(const RE::TESDescription* description) const {
        std::lock_guard lock{ snapshotMutex_ };
        const auto entry = descriptionIndex_.find(description);
        return entry == descriptionIndex_.end() ? std::string{} : TextSnapshotLocked(entry->second);
    }

    void BookManager::SetTextSnapshot(RE::FormID bookFormId, const std::string& text) {
        const auto* book = RE::TESForm::LookupByID<RE::TESObjectBOOK>(bookFormId);
        std::lock_guard lock{ snapshotMutex_ };
        if (book) descriptionIndex_[static_cast<const RE::TESDescription*>(book)] = bookFormId;
        textSnapshot_[bookFormId] = text;
    }

    void BookManager::ShowRemovedPage(RE::FormID bookFormId, const std::string& actorUuid) {
        SetTextSnapshot(bookFormId, FormatDiaryEntries({}, Database::GetActorName(actorUuid)));
    }

    void BookManager::RetireBook(RE::FormID bookFormId, const std::string& actorUuid) {
        DynamicForms::Retire(bookFormId);
        ShowRemovedPage(bookFormId, actorUuid);
    }

    void BookManager::IndexBook(const DiaryBookData& vol) {
        formIndex_[vol.bookFormId] = { vol.actorUuid, vol.volumeNumber };
        SetTextSnapshot(vol.bookFormId, vol.cachedBookText);
    }

    void BookManager::UnindexBook(RE::FormID bookFormId) {
        formIndex_.erase(bookFormId);
        std::lock_guard lock{ snapshotMutex_ };
        std::erase_if(descriptionIndex_, [bookFormId](const auto& entry) { return entry.second == bookFormId; });
        textSnapshot_.erase(bookFormId);
    }

    void BookManager::ClearIndexes() {
        formIndex_.clear();
        std::lock_guard lock{ snapshotMutex_ };
        descriptionIndex_.clear();
        textSnapshot_.clear();
    }

    DiaryBookData& BookManager::RegisterBook(DiaryBookData data) {
        // Pre-warm the runtime cache so RefreshVolumeOnOpen never needs the UUID roundtrip.
        if (data.actorFormId != 0) {
            data.cachedActorFormId = data.actorFormId;
        }
        auto& volumes = books_[data.actorUuid];
        auto& registered = volumes.emplace_back(std::move(data));
        IndexBook(registered);
        SKSE::log::info("Registered book for actor {}: FormID 0x{:X}, Volume {} (template: {}, subfolder: {})",
                       registered.actorUuid, registered.bookFormId, registered.volumeNumber,
                       registered.journalTemplate, registered.bioTemplateName);

        // Persist the row (the caller writes the text next, via SetVolumeText).
        DiaryDB::GetSingleton()->UpsertVolume(ToRow(registered));
        return registered;
    }

    void BookManager::UpdateBookEndTime(const std::string& actorUuid, int volumeNumber, double endTime) {
        auto it = books_.find(actorUuid);
        if (it != books_.end()) {
            for (auto& book : it->second) {
                if (book.volumeNumber == volumeNumber) {
                    book.endTime = endTime;
                    SKSE::log::debug("Updated book endTime for {} volume {} (FormID 0x{:X}) to {}",
                                   actorUuid, volumeNumber, book.bookFormId, endTime);
                    DiaryDB::GetSingleton()->UpdateEndTime(actorUuid, volumeNumber, endTime);
                    return;
                }
            }
        }
    }

    std::vector<DiaryEntry> BookManager::GetLiveEntries(const DiaryBookData& vol, RE::FormID actorFormId,
                                                        double endTime, bool* ok) {
        VolumeBounds bounds{
            .startTime = vol.volumeNumber == 1 ? 0.0 : vol.startTime,
            .endTime = endTime,
            .prevLastCreationTime = vol.prevVolumeLastCreationTime,
            .prevCountAtBoundary = vol.prevVolumeCountAtBoundary,
        };
        if (const auto* volumes = GetAllVolumesForActor(vol.actorUuid)) {
            for (const auto& next : *volumes) {
                if (next.volumeNumber == vol.volumeNumber + 1) {
                    bounds.nextStartTime = next.startTime;
                    bounds.nextPrevLastCreationTime = next.prevVolumeLastCreationTime;
                    bounds.nextPrevCountAtBoundary = next.prevVolumeCountAtBoundary;
                    break;
                }
            }
        }
        return Database::GetVolumeEntries(actorFormId, bounds, ok);
    }

    void BookManager::SetVolumeText(DiaryBookData& vol, const std::vector<DiaryEntry>& entries) {
        std::string text = FormatDiaryEntries(entries, vol.actorName);
        const int count = static_cast<int>(entries.size());
        DiaryDB::GetSingleton()->UpdateBookText(vol.actorUuid, vol.volumeNumber, text, count);
        SKSE::log::debug("{} volume {}: text set from {} entries (was {})",
                         vol.actorName, vol.volumeNumber, count, vol.lastKnownEntryCount);
        vol.cachedBookText = std::move(text);
        vol.lastKnownEntryCount = count;
        SetTextSnapshot(vol.bookFormId, vol.cachedBookText);
    }

    void BookManager::UnregisterVolumesFrom(const std::string& actorUuid, int fromVolume) {
        auto it = books_.find(actorUuid);
        if (it == books_.end()) return;
        auto& volumes = it->second;
        auto* db = DiaryDB::GetSingleton();
        int removed = 0;
        for (auto vol = volumes.begin(); vol != volumes.end();) {
            if (vol->volumeNumber >= fromVolume) {
                db->DeleteVolume(actorUuid, vol->volumeNumber);
                UnindexBook(vol->bookFormId);
                RetireBook(vol->bookFormId, actorUuid);
                vol = volumes.erase(vol);
                ++removed;
            } else {
                ++vol;
            }
        }
        SKSE::log::info("Unregistered {} volume(s) from {} for {}", removed, fromVolume, actorUuid);
    }

    void BookManager::RegenerateAllDiaryTexts() {
        SKSE::log::info("[BookManager] Regenerating text for {} actors' diaries from API", books_.size());

        try {
            int totalRegenerated = 0;
            for (auto& [uuid, volumes] : books_) {
                uint32_t actorFormId = SkyrimNetDiaries::Database::GetFormIDForUUID(uuid);
                if (actorFormId == 0) continue;

                for (auto& bookData : volumes) {
                    // Keep cached FormID warm so the next open can skip the UUID lookup.
                    bookData.cachedActorFormId = actorFormId;

                    std::string bookTitle = Localization::GetSingleton()->FormatBookName(bookData.actorName, bookData.volumeNumber);

                    SKSE::log::debug("[Regen] '{}' vol={} startTime={:.2f} endTime={:.2f}",
                                    bookTitle, bookData.volumeNumber, bookData.startTime, bookData.endTime);

                    bool queryOk = false;
                    auto volumeEntries = GetLiveEntries(bookData, actorFormId, bookData.endTime, &queryOk);
                    if (!queryOk) {
                        SKSE::log::warn("[Regen] '{}': couldn't read entries from SkyrimNet — keeping its text", bookTitle);
                        continue;
                    }

                    SKSE::log::debug("[Regen] '{}' {} entries; first={:.2f} last={:.2f}",
                                    bookTitle, volumeEntries.size(),
                                    volumeEntries.empty() ? 0.0 : volumeEntries.front().entry_date,
                                    volumeEntries.empty() ? 0.0 : volumeEntries.back().entry_date);

                    SetVolumeText(bookData, volumeEntries);

                    totalRegenerated++;
                }
            }

            SKSE::log::info("[BookManager] Regenerated {} diary volumes from API", totalRegenerated);

        } catch (const std::exception& e) {
            SKSE::log::error("[BookManager] Exception regenerating diary texts: {}", e.what());
        } catch (...) {
            SKSE::log::error("[BookManager] Unknown exception regenerating diary texts");
        }
    }

    void BookManager::Revert() {
        // Clear in-memory maps only; DiaryDB on disk is intentionally preserved
        // so that volume metadata survives the revert and loads correctly.
        books_.clear();
        ClearIndexes();
        unclaimed_.clear();
        actorTemplates_.clear();
        SKSE::log::debug("[BookManager] Revert: in-memory data cleared (DiaryDB preserved on disk)");
    }

    void BookManager::FlushToDB() {
        auto* db = DiaryDB::GetSingleton();
        if (!db->IsOpen()) return;

        int volumesFlushed = 0;
        for (const auto& [uuid, volumes] : books_) {
            for (const auto& data : volumes) {
                db->UpsertVolume(ToRow(data));
                ++volumesFlushed;
            }
        }
        for (const auto& [uuid, tmpl] : actorTemplates_) {
            db->UpsertActorTemplate(uuid, tmpl);
        }
        SKSE::log::debug("[BookManager] FlushToDB: wrote {} volumes, {} actor templates",
                        volumesFlushed, actorTemplates_.size());
    }

    void BookManager::ClearActorCache() {
        ClearActorLookupCache();
        SKSE::log::debug("[BookManager] Actor cache cleared");
    }

    std::vector<std::string> BookManager::LoadFromDB() {
        auto* db = DiaryDB::GetSingleton();

        // Always clear in-memory state on each game load, regardless of whether the
        // DB is open.  If we skip the clear when the DB is closed (e.g. save-folder
        // detection failed on a previous load), stale books_ entries from the prior
        // session survive and cause the catch-up scan to think every actor already
        // has books — so nothing gets recreated after a reload-without-save.
        books_.clear();
        ClearIndexes();
        unclaimed_.clear();
        actorTemplates_.clear();

        if (!db->IsOpen()) {
            SKSE::log::warn("[BookManager] LoadFromDB: DiaryDB not open — skipping");
            return {};
        }

        // This save's live books by volume, from its co-save records: the record, not
        // the row, says which form is the volume in this save (DiaryDB is shared by
        // every save of the character).  Retired books match no row.
        std::map<std::pair<std::string, int>, std::vector<DynamicForms::Record>> saved;
        for (auto& record : DynamicForms::Tracked()) {
            std::string uuid;
            int volume = 0;
            if (!ParseVolumeKey(record.key, uuid, volume)) continue;
            if (record.retired) {
                ShowRemovedPage(record.formId, uuid);
            } else {
                saved[{ uuid, volume }].push_back(std::move(record));
            }
        }

        auto rows = db->LoadAllVolumes();
        std::vector<std::string> invalidActors;
        auto* localization = Localization::GetSingleton();

        for (auto& row : rows) {
            // The save's book for this volume; with two, the one DiaryDB names.
            auto* candidates = [&]() -> std::vector<DynamicForms::Record>* {
                const auto match = saved.find({ row.actorUuid, row.volumeNumber });
                return match != saved.end() && !match->second.empty() ? &match->second : nullptr;
            }();
            auto chosen = candidates ? std::ranges::find(*candidates, static_cast<RE::FormID>(row.bookFormId),
                                                         &DynamicForms::Record::formId)
                                     : std::vector<DynamicForms::Record>::iterator{};
            if (candidates && chosen == candidates->end()) chosen = candidates->begin();
            auto* book = candidates ? RE::TESForm::LookupByID<RE::TESObjectBOOK>(chosen->formId) : nullptr;
            if (!book) {
                // Created after this save, or in another of the character's timelines:
                // the volume has no book here.  It is recreated from SkyrimNet's entries.
                SKSE::log::info("[LoadFromDB] {} vol {}: no book in this save — removing row and queuing recreation",
                                row.actorName, row.volumeNumber);
                db->DeleteVolume(row.actorUuid, row.volumeNumber);
                invalidActors.push_back(row.actorUuid);
                continue;
            }
            DynamicForms::Record record = std::move(*chosen);
            candidates->erase(chosen);

            DiaryBookData data = FromRow(row);
            if (data.bookFormId != record.formId) {
                SKSE::log::info("[LoadFromDB] {} vol {}: this save's book is 0x{:X} (DiaryDB had 0x{:X})",
                                data.actorName, data.volumeNumber, record.formId, data.bookFormId);
                data.bookFormId = record.formId;
                db->UpsertVolume(ToRow(data));
            }

            // DiaryDB is authoritative for the look: re-apply it (the actor's name or the
            // game's language may have changed since the save) and keep the record in step.
            if (!data.journalTemplate.empty()) record.templateEditorId = data.journalTemplate;
            record.displayName = localization->FormatBookName(data.actorName, data.volumeNumber);
            auto* templateBook = RE::TESForm::LookupByEditorID<RE::TESObjectBOOK>(record.templateEditorId);
            ConfigureDiaryForm(book, templateBook, record.displayName);
            DynamicForms::Track(std::move(record));

            IndexBook(data);
            books_[data.actorUuid].push_back(std::move(data));
        }

        // Books with no volume in DiaryDB are not proof of a Reset (DiaryDB may be new,
        // or from another branch of saves), so they are left where they are: unclaimed,
        // and reused if their volume is created again.
        for (const auto& [volume, records] : saved) {
            for (const auto& record : records) {
                SKSE::log::info("[LoadFromDB] Book 0x{:X} ('{}') has no volume in DiaryDB — kept unclaimed",
                                record.formId, record.key);
                unclaimed_.try_emplace(record.key, record.formId);
                ShowRemovedPage(record.formId, volume.first);
            }
        }
        SweepRetiredBooks();

        // Ensure each actor's volumes are in order.
        for (auto& [uuid, volumes] : books_) {
            std::sort(volumes.begin(), volumes.end(),
                [](const DiaryBookData& a, const DiaryBookData& b) {
                    return a.volumeNumber < b.volumeNumber;
                });
        }

        actorTemplates_ = db->LoadActorTemplates();

        SKSE::log::info("[LoadFromDB] Loaded {} actors ({} volume(s) queued for recreation)",
                        books_.size(), invalidActors.size());
        return invalidActors;
    }

    void BookManager::RefreshVolumeOnOpen(DiaryBookData* vol) {
        if (!vol) return;

        // Backfill bioTemplateName (may be missing on old co-save sessions).
        if (vol->bioTemplateName.empty()) {
            vol->bioTemplateName = Database::GetTemplateNameByUUID(vol->actorUuid);
        }

        // Resolve the actor's FormID once per session from the UUID.  The stored
        // actorFormId is only a fallback, and only if SkyrimNet maps it back to this
        // UUID: after a load-order change it can belong to someone else, whose entries
        // the book would then show.
        if (vol->cachedActorFormId == 0) {
            RE::FormID live = Database::GetFormIDForUUID(vol->actorUuid);
            if (live == 0 && vol->actorFormId != 0 &&
                Database::GetUUIDFromFormID(vol->actorFormId) == vol->actorUuid) {
                live = vol->actorFormId;
            }
            vol->cachedActorFormId = live;
        }
        if (vol->cachedActorFormId == 0) return;

        // For the active (latest) volume use 0.0 so entries written after the
        // last update are visible even if UpdateDiaryForActorInternal hasn't run yet.
        // For an earlier volume (a newer one exists), vol->endTime is the upper cutoff.
        double queryEnd = 0.0;
        {
            auto* allVols = GetAllVolumesForActor(vol->actorUuid);
            if (allVols && !allVols->empty() &&
                allVols->back().volumeNumber != vol->volumeNumber) {
                // A newer volume exists: stop at this one's end.
                queryEnd = vol->endTime;
            }
        }

        bool queryOk = false;
        auto liveEntries = GetLiveEntries(*vol, vol->cachedActorFormId, queryEnd, &queryOk);
        if (!queryOk) {
            // Couldn't read SkyrimNet: keep what the book shows rather than treat the
            // failure as "every entry was deleted".
            SKSE::log::warn("[SNPD] {} vol {}: couldn't read entries from SkyrimNet — showing the cached text",
                            vol->actorName, vol->volumeNumber);
            return;
        }
        int liveCount = static_cast<int>(liveEntries.size());

        // Nothing changed and the cached text is current: done.  Text from before
        // font tags existed is re-rendered once.  Zero live entries is real (the query
        // succeeded): a volume whose entries were all deleted gets the "all entries
        // removed" page.
        bool textIsCurrentFormat = vol->cachedBookText.find("<font face='") != std::string::npos;
        if (liveCount == vol->lastKnownEntryCount && !vol->cachedBookText.empty() && textIsCurrentFormat) {
            return;
        }

        if (!textIsCurrentFormat && !vol->cachedBookText.empty()) {
            SKSE::log::info("[SNPD] {} vol {} has old-format text (no font tags) — regenerating from {} live entries",
                vol->actorName, vol->volumeNumber, liveCount);
        }

        // Entries were deleted — move endTime back so QueueNewEntryRecovery
        // doesn't probe beyond the now-missing entry's timestamp and spawn a duplicate volume.
        if (liveCount < vol->lastKnownEntryCount) {
            SKSE::log::info("[SNPD] {} vol {} shrank {} → {} entries on open",
                vol->actorName, vol->volumeNumber, vol->lastKnownEntryCount, liveCount);
            if (vol->endTime > 0.0 && !liveEntries.empty()) {
                double newEnd = liveEntries.back().entry_date;
                if (newEnd != vol->endTime) {
                    SKSE::log::debug("[SNPD]   endTime updated {:.2f} → {:.2f}", vol->endTime, newEnd);
                    DiaryDB::GetSingleton()->UpdateEndTime(vol->actorUuid, vol->volumeNumber, newEnd);
                    vol->endTime = newEnd;
                }
            }
        }

        SetVolumeText(*vol, liveEntries);
    }

} // namespace SkyrimNetDiaries
