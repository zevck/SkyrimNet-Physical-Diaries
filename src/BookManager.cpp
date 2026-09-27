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
#include "Localization.h"
#include <algorithm>

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
            r.persistedInSave            = d.persistedInSave;
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
            d.persistedInSave            = r.persistedInSave;
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
        // Search all actors' volumes for matching FormID
        for (auto& [uuid, volumes] : books_) {
            for (auto& book : volumes) {
                if (book.bookFormId == formId) {
                    return &book;
                }
            }
        }
        return nullptr;
    }

    DiaryBookData& BookManager::RegisterBook(DiaryBookData data) {
        // Pre-warm the runtime cache so RefreshVolumeOnOpen never needs the UUID roundtrip.
        if (data.actorFormId != 0) {
            data.cachedActorFormId = data.actorFormId;
        }
        auto& volumes = books_[data.actorUuid];
        auto& registered = volumes.emplace_back(std::move(data));
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

    void BookManager::Save(SKSE::SerializationInterface* a_intfc) {
        // Volume data is now stored in DiaryDB (SQLite) — persists across reverts.
        // Write a sentinel so the co-save record stays well-formed.
        std::uint32_t sentinel = 0;
        a_intfc->WriteRecordData(&sentinel, sizeof(sentinel)); // totalVolumes = 0
        a_intfc->WriteRecordData(&sentinel, sizeof(sentinel)); // actorTemplateCount = 0
        SKSE::log::debug("[BookManager] Save: sentinel written (real data lives in DiaryDB)");
    }

    void BookManager::Load(SKSE::SerializationInterface* /*a_intfc*/, std::uint32_t /*version*/) {
        // Volume data is loaded from DiaryDB in LoadFromDB() (called from kPostLoadGame).
        // The sentinel written by Save() is intentionally ignored.
        SKSE::log::debug("[BookManager] Load: skipping co-save (real data loaded from DiaryDB)");
    }

    void BookManager::Revert() {
        // Clear in-memory maps only; DiaryDB on disk is intentionally preserved
        // so that volume metadata survives the revert and loads correctly.
        books_.clear();
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
        // FormID claims are per-session; clear alongside the actor cache so a fresh
        // load/regeneration starts with a clean claim table.
        ClearBookFormIdClaims();
        SKSE::log::debug("[BookManager] Actor cache + FormID claims cleared");
    }

    // Gives the NPC the book back if they don't hold it: a volume whose form is still
    // in memory but not in the NPC's inventory (a reload without saving).
    static void EnsureBookInInventory(RE::FormID bookFormId, RE::FormID targetFormID,
                                      const std::string& actorName, const std::string& bioTemplate,
                                      const std::string& bookName, const std::string& actorUuid) {
        auto* book = RE::TESForm::LookupByID<RE::TESObjectBOOK>(bookFormId);
        if (!book) {
            SKSE::log::warn("[EnsureInventory] Book 0x{:X} no longer valid — skipping '{}'", bookFormId, bookName);
            return;
        }

        RE::Actor* actor = FindActorForBook(targetFormID, actorName, bioTemplate, actorUuid);
        if (!actor) {
            SKSE::log::warn("[EnsureInventory] Could not find actor '{}' (UUID {}) for '{}'", actorName, actorUuid, bookName);
            return;
        }

        if (CountInInventory(actor, book) > 0) {
            SKSE::log::debug("[EnsureInventory] '{}' already in {}'s inventory", bookName, actorName);
            return;
        }

        actor->AddObjectToContainer(book, nullptr, 1, nullptr);
        SKSE::log::info("[EnsureInventory] Re-added '{}' to {}'s inventory after reload", bookName, actorName);
    }

    void BookManager::QueueInventoryCheck() {
        int queued = 0;
        int skipped = 0;
        for (const auto& [uuid, volumes] : books_) {
            uint32_t actorFormId = SkyrimNetDiaries::Database::GetFormIDForUUID(uuid);
            for (const auto& vol : volumes) {
                if (vol.persistedInSave) {
                    // This volume was committed to a .ess save — the loaded inventory
                    // state is authoritative.  Do not re-add (would duplicate taken books).
                    ++skipped;
                    continue;
                }
                RE::FormID bookFid   = vol.bookFormId;
                // Prefer the volume's stored actorFormId (set at creation time);
                // fall back to live UUID→FormID resolution if it wasn't persisted
                // on older DB rows.
                RE::FormID actorFid  = vol.actorFormId != 0
                                          ? vol.actorFormId
                                          : static_cast<RE::FormID>(actorFormId);
                std::string aName    = vol.actorName;
                std::string bio      = vol.bioTemplateName;
                std::string bName    = Localization::GetSingleton()->FormatBookName(vol.actorName, vol.volumeNumber);
                std::string aUuid    = uuid;
                SKSE::GetTaskInterface()->AddTask([bookFid, actorFid, aName, bio, bName, aUuid]() {
                    EnsureBookInInventory(bookFid, actorFid, aName, bio, bName, aUuid);
                });
                ++queued;
            }
        }
        if (queued > 0 || skipped > 0)
            SKSE::log::debug("[BookManager] Inventory check: {} volume(s) queued, {} skipped (already persisted)", queued, skipped);
    }

    std::vector<std::string> BookManager::LoadFromDB() {
        auto* db = DiaryDB::GetSingleton();

        // Always clear in-memory state on each game load, regardless of whether the
        // DB is open.  If we skip the clear when the DB is closed (e.g. save-folder
        // detection failed on a previous load), stale books_ entries from the prior
        // session survive and cause the catch-up scan to think every actor already
        // has books — so nothing gets recreated after a reload-without-save.
        books_.clear();
        actorTemplates_.clear();

        if (!db->IsOpen()) {
            SKSE::log::warn("[BookManager] LoadFromDB: DiaryDB not open — skipping");
            return {};
        }

        auto rows = db->LoadAllVolumes();
        std::vector<std::string> invalidActors;

        for (auto& row : rows) {
            // The book form must exist.  A volume created after the loaded save isn't
            // in it (DPF only restores forms the save contains): drop the row and let
            // the actor be recreated from SkyrimNet's entries.
            auto* form = RE::TESForm::LookupByID(static_cast<RE::FormID>(row.bookFormId));
            if (!form || form->GetFormType() != RE::FormType::Book) {
                SKSE::log::info("[LoadFromDB] {} vol {}: book 0x{:X} is gone{} — removing row and queuing recreation",
                                row.actorName, row.volumeNumber, row.bookFormId,
                                row.persistedInSave ? "" : " (never saved)");
                db->DeleteVolume(row.actorUuid, row.volumeNumber);
                invalidActors.push_back(row.actorUuid);
                continue;
            }
            if (!row.persistedInSave) {
                // Created this session and never saved (a reload without saving): the
                // form is still in memory; QueueInventoryCheck puts it back.
                SKSE::log::debug("[LoadFromDB] {} vol {} not saved yet, form 0x{:X} still valid",
                                 row.actorName, row.volumeNumber, row.bookFormId);
            }

            DiaryBookData data = FromRow(row);

            // Claim this loaded diary's FormID so a later creation can't be handed
            // the same ID (guards against a duplicate deleted record in DPF's pool
            // that happens to match an already-live loaded diary).
            ClaimBookFormId(static_cast<RE::FormID>(row.bookFormId), data.actorUuid);

            // DPF may have restored this form with another owner's data: a FormID it
            // handed out in an earlier session keeps that session's name in a save
            // made then.  DiaryDB is authoritative, so re-apply the volume's look.
            {
                const std::string bookName = Localization::GetSingleton()->FormatBookName(data.actorName, data.volumeNumber);
                auto* templateBook = RE::TESForm::LookupByEditorID<RE::TESObjectBOOK>(data.journalTemplate);
                const std::string previous = form->GetName();
                if (ConfigureDiaryForm(form->As<RE::TESObjectBOOK>(), templateBook, bookName)) {
                    SKSE::log::info("[LoadFromDB] Book 0x{:X} was named '{}' — renamed to '{}'",
                                    row.bookFormId, previous, bookName);
                }
            }

            books_[data.actorUuid].push_back(std::move(data));
        }

        // Ensure each actor's volumes are in order.
        for (auto& [uuid, volumes] : books_) {
            std::sort(volumes.begin(), volumes.end(),
                [](const DiaryBookData& a, const DiaryBookData& b) {
                    return a.volumeNumber < b.volumeNumber;
                });
        }

        actorTemplates_ = db->LoadActorTemplates();

        SKSE::log::info("[LoadFromDB] Loaded {} actors ({} invalid FormIDs queued for recovery)",
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
        // For sealed older volumes, respect vol->endTime as the upper-time cutoff.
        double queryEnd = 0.0;
        {
            auto* allVols = GetAllVolumesForActor(vol->actorUuid);
            if (allVols && !allVols->empty() &&
                allVols->back().volumeNumber != vol->volumeNumber) {
                // A newer volume exists → this one is sealed.
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

        // Entries were deleted — update sealed endTime so QueueSealedVolumeRecovery
        // doesn't probe beyond the now-missing entry's timestamp and spawn a duplicate volume.
        if (liveCount < vol->lastKnownEntryCount) {
            SKSE::log::info("[SNPD] {} vol {} shrank {} → {} entries on open",
                vol->actorName, vol->volumeNumber, vol->lastKnownEntryCount, liveCount);
            if (vol->endTime > 0.0 && !liveEntries.empty()) {
                double newEnd = liveEntries.back().entry_date;
                if (newEnd != vol->endTime) {
                    SKSE::log::debug("[SNPD]   sealed endTime updated {:.2f} → {:.2f}", vol->endTime, newEnd);
                    DiaryDB::GetSingleton()->UpdateEndTime(vol->actorUuid, vol->volumeNumber, newEnd);
                    vol->endTime = newEnd;
                }
            }
        }

        SetVolumeText(*vol, liveEntries);
    }

} // namespace SkyrimNetDiaries
