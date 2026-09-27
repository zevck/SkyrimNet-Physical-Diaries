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

    void BookManager::Initialize(const std::string& baseTemplate, 
                                 const std::string& journal01,
                                 const std::string& journal02,
                                 const std::string& journal03,
                                 const std::string& journal04,
                                 const std::string& nightingaleJournal) {
        templateBookEditorId_ = baseTemplate;
        
        // Store journal variants (include base template for more variety)
        journalTemplates_.push_back(baseTemplate);
        if (!journal01.empty()) journalTemplates_.push_back(journal01);
        if (!journal02.empty()) journalTemplates_.push_back(journal02);
        if (!journal03.empty()) journalTemplates_.push_back(journal03);
        if (!journal04.empty()) journalTemplates_.push_back(journal04);
        
        nightingaleTemplate_ = nightingaleJournal;
        
        // Log initialization
        if (!journalTemplates_.empty() && !nightingaleTemplate_.empty()) {
            SKSE::log::info("BookManager initialized with base template: {}, {} journal variants, Nightingale: {}",
                           baseTemplate, journalTemplates_.size(), nightingaleTemplate_);
        } else if (!journalTemplates_.empty()) {
            SKSE::log::info("BookManager initialized with base template: {}, {} journal variants",
                           baseTemplate, journalTemplates_.size());
        } else if (!nightingaleTemplate_.empty()) {
            SKSE::log::info("BookManager initialized with base template: {}, Nightingale: {}",
                           baseTemplate, nightingaleTemplate_);
        } else {
            SKSE::log::info("BookManager initialized with base template: {}", baseTemplate);
        }
    }

    std::string BookManager::SelectJournalTemplate(const std::string& actorUuid, const std::string& actorName) {
        // Check if we already selected a template for this actor
        // An empty name (a row created by UpdateLastKnownGameTime before any volume) is no choice.
        auto it = actorTemplates_.find(actorUuid);
        if (it != actorTemplates_.end() && !it->second.empty()) {
            SKSE::log::debug("Using cached journal template for {}: {}", actorName, it->second);
            return it->second;
        }
        
        std::string selectedTemplate;
        
        // Check for Nightingale NPCs (special journal)
        if (!nightingaleTemplate_.empty()) {
            if (actorName == "Karliah" || actorName == "Gallus" || actorName == "Mercer Frey") {
                selectedTemplate = nightingaleTemplate_;
                SKSE::log::debug("Selected Nightingale journal for {}", actorName);
                actorTemplates_[actorUuid] = selectedTemplate;
                DiaryDB::GetSingleton()->UpsertActorTemplate(actorUuid, selectedTemplate);
                return selectedTemplate;
            }
        }
        
        // If no variants configured, use base template
        if (journalTemplates_.empty()) {
            selectedTemplate = templateBookEditorId_;
            SKSE::log::debug("No journal variants - using base template for {}", actorName);
            actorTemplates_[actorUuid] = selectedTemplate;
            return selectedTemplate;
        }
        
        // Pick a variant from the actor's UUID so the choice is stable across reloads.
        selectedTemplate = journalTemplates_[std::hash<std::string>{}(actorUuid) % journalTemplates_.size()];
        
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

    void BookManager::RegisterBook(const std::string& actorUuid, const std::string& actorName,
                                   RE::FormID bookFormId, double startTime, double endTime, int volumeNumber,
                                   const std::string& journalTemplate,
                                   const std::string& bioTemplateName,
                                   double prevVolumeLastCreationTime,
                                   int prevVolumeCountAtBoundary,
                                   RE::FormID actorFormId) {
        DiaryBookData data;
        data.actorUuid = actorUuid;
        data.actorName = actorName;
        data.bookFormId = bookFormId;
        data.startTime = startTime;
        data.endTime = endTime;
        data.volumeNumber = volumeNumber;
        data.journalTemplate = journalTemplate;
        data.bioTemplateName = bioTemplateName;
        data.prevVolumeLastCreationTime = prevVolumeLastCreationTime;
        data.prevVolumeCountAtBoundary = prevVolumeCountAtBoundary;
        data.actorFormId = actorFormId;
        // Pre-warm the runtime cache so RefreshVolumeOnOpen never needs the UUID roundtrip.
        if (actorFormId != 0) {
            data.cachedActorFormId = actorFormId;
        }

        books_[actorUuid].push_back(data);
        SKSE::log::info("Registered book for actor {}: FormID 0x{:X}, Volume {} (template: {}, subfolder: {})",
                       actorUuid, bookFormId, volumeNumber, journalTemplate, bioTemplateName);

        // Persist row so it survives a save revert (bookText written by caller via UpdateBookText).
        DiaryDB::VolumeRow dbRow;
        dbRow.actorUuid                  = data.actorUuid;
        dbRow.actorName                  = data.actorName;
        dbRow.actorFormId                = static_cast<uint32_t>(actorFormId);
        dbRow.bookFormId                 = static_cast<uint32_t>(data.bookFormId);
        dbRow.volumeNumber               = data.volumeNumber;
        dbRow.startTime                  = data.startTime;
        dbRow.endTime                    = data.endTime;
        dbRow.journalTemplate            = data.journalTemplate;
        dbRow.bioTemplateName            = data.bioTemplateName;
        dbRow.lastKnownEntryCount        = data.lastKnownEntryCount;
        dbRow.prevVolumeLastCreationTime = data.prevVolumeLastCreationTime;
        dbRow.prevVolumeCountAtBoundary  = data.prevVolumeCountAtBoundary;
        // bookText left empty – caller sets it via UpdateBookText immediately after.
        DiaryDB::GetSingleton()->UpsertVolume(dbRow);
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
    
    void BookManager::SetVolumeText(DiaryBookData& vol, const std::vector<DiaryEntry>& entries) {
        const int maxEntries = Config::GetSingleton()->GetEntriesPerVolume();
        std::string text = FormatDiaryEntries(entries, vol.actorName, vol.startTime, vol.endTime, maxEntries);
        const int count = static_cast<int>(entries.size());
        DiaryDB::GetSingleton()->UpdateBookText(vol.actorUuid, vol.volumeNumber, text, count);
        SKSE::log::debug("{} volume {}: text set from {} entries (was {})",
                         vol.actorName, vol.volumeNumber, count, vol.lastKnownEntryCount);
        vol.cachedBookText = std::move(text);
        vol.lastKnownEntryCount = count;
    }

    void BookManager::UnregisterBook(const std::string& actorUuid) {
        auto it = books_.find(actorUuid);
        if (it != books_.end()) {
            SKSE::log::info("Unregistered {} volumes for {}", it->second.size(), actorUuid);
            books_.erase(it);
        }
        DiaryDB::GetSingleton()->DeleteActor(actorUuid);
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

                    const int MAX_ENTRIES_PER_VOLUME = SkyrimNetDiaries::Config::GetSingleton()->GetEntriesPerVolume();

                    double queryStart = (bookData.volumeNumber == 1) ? 0.0 : bookData.startTime;
                    double queryEnd = bookData.endTime; // 0.0 = no upper bound for latest volume

                    std::string bookTitle = Localization::GetSingleton()->FormatBookName(bookData.actorName, bookData.volumeNumber);

                    SKSE::log::debug("[Regen] '{}' vol={} stored startTime={:.2f} endTime={:.2f} -> queryStart={:.2f} queryEnd={:.2f}",
                                    bookTitle, bookData.volumeNumber,
                                    bookData.startTime, bookData.endTime,
                                    queryStart, queryEnd);

                    // Sealed volumes never show more than MAX_ENTRIES_PER_VOLUME entries.
                    auto volumeEntries = SkyrimNetDiaries::Database::GetVolumeEntries(
                        actorFormId, queryStart, queryEnd, bookData.prevVolumeLastCreationTime,
                        bookData.prevVolumeCountAtBoundary,
                        bookData.endTime > 0.0 ? MAX_ENTRIES_PER_VOLUME : 0);

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
                DiaryDB::VolumeRow row;
                row.actorUuid                  = data.actorUuid;
                row.actorName                  = data.actorName;
                row.bookFormId                 = static_cast<uint32_t>(data.bookFormId);
                row.volumeNumber               = data.volumeNumber;
                row.startTime                  = data.startTime;
                row.endTime                    = data.endTime;
                row.journalTemplate            = data.journalTemplate;
                row.bioTemplateName            = data.bioTemplateName;
                row.lastKnownEntryCount        = data.lastKnownEntryCount;
                row.prevVolumeLastCreationTime = data.prevVolumeLastCreationTime;
                row.prevVolumeCountAtBoundary  = data.prevVolumeCountAtBoundary;
                row.bookText                   = data.cachedBookText;
                row.persistedInSave            = data.persistedInSave;  // preserve — MarkAllVolumesPersisted sets on save
                db->UpsertVolume(row);
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

    // ---------------------------------------------------------------------------
    // EnsureBookInInventory: if the NPC doesn't have the book, add it.
    // Called after LoadFromDB for volumes whose DPF form exists but may not be
    // in the NPC's inventory (e.g. after reload-without-save).
    //
    // actorUuid is REQUIRED to correctly route diaries for non-unique NPCs
    // (guards, bandits, wolves) — passing it lets FindActorForBook resolve
    // by UUID instead of falling back to name matching which can collide.
    // ---------------------------------------------------------------------------
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

        // Check inventory — avoid adding a duplicate.
        auto inv = actor->GetInventory();
        for (const auto& [item, invData] : inv) {
            if (item && item->GetFormID() == bookFormId && invData.first > 0) {
                SKSE::log::debug("[EnsureInventory] '{}' already in {}'s inventory", bookName, actorName);
                return;
            }
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
            // Volumes that were never committed to a .ess save are ephemeral.
            // The player may have quit-without-saving after diary creation, then
            // loaded an older save whose game-time is EARLIER than the volume's
            // recorded endTime.  Any new diary entry generated at that reverted
            // game-time would have entry_date <= endTime and be silently filtered
            // out as "belongs to previous volume" — so the book is never rebuilt.
            //
            // Safety: only delete if the DPF form is also gone.  If the form still
            // exists the row may be a legitimate migrated row (persisted_in_save
            // added via ALTER TABLE DEFAULT 0 on an older install) that just hasn't
            // been re-saved yet.  In that case let QueueInventoryCheck / the catch-up
            // scan handle it gracefully rather than nuking it here.
            if (!row.persistedInSave) {
                auto* form = RE::TESForm::LookupByID(static_cast<RE::FormID>(row.bookFormId));
                bool formValid = form && form->GetFormType() == RE::FormType::Book;
                if (!formValid) {
                    SKSE::log::info("[LoadFromDB] {} vol {} was never saved and DPF form is gone — removing stale row and queuing recreation",
                                   row.actorName, row.volumeNumber);
                    db->DeleteVolume(row.actorUuid, row.volumeNumber);
                    invalidActors.push_back(row.actorUuid);
                    continue;
                }
                // Form still alive but not persisted — fall through and load normally.
                // QueueSealedVolumeRecovery / catch-up will detect the stale endTime
                // and rebuild if entries exist beyond it.
                SKSE::log::info("[LoadFromDB] {} vol {} not persisted but DPF form 0x{:X} still valid — loading and deferring to catch-up",
                               row.actorName, row.volumeNumber, row.bookFormId);
            }

            // Validate the DPF book form still exists (it is lost on save revert).
            auto* form = RE::TESForm::LookupByID(static_cast<RE::FormID>(row.bookFormId));
            if (!form || form->GetFormType() != RE::FormType::Book) {
                SKSE::log::warn("[LoadFromDB] FormID 0x{:X} for {} vol {} is invalid — removing and queuing recovery",
                               row.bookFormId, row.actorName, row.volumeNumber);
                db->DeleteVolume(row.actorUuid, row.volumeNumber);
                invalidActors.push_back(row.actorUuid);
                continue;
            }

            DiaryBookData data;
            data.actorUuid                   = row.actorUuid;
            data.actorName                   = row.actorName;
            data.bookFormId                  = static_cast<RE::FormID>(row.bookFormId);
            data.volumeNumber                = row.volumeNumber;
            data.startTime                   = row.startTime;
            data.endTime                     = row.endTime;
            data.journalTemplate             = row.journalTemplate;
            data.bioTemplateName             = row.bioTemplateName;
            data.lastKnownEntryCount         = row.lastKnownEntryCount;
            data.prevVolumeLastCreationTime  = row.prevVolumeLastCreationTime;
            data.prevVolumeCountAtBoundary   = row.prevVolumeCountAtBoundary;
            data.cachedBookText              = row.bookText;  // pre-warmed from DB
            data.persistedInSave             = row.persistedInSave;
            data.actorFormId                 = static_cast<RE::FormID>(row.actorFormId);
            // Pre-warm cachedActorFormId so RefreshVolumeOnOpen never needs UUID roundtrip.
            if (row.actorFormId != 0) {
                data.cachedActorFormId = static_cast<RE::FormID>(row.actorFormId);
            }

            // Claim this loaded diary's FormID so a later creation can't be handed
            // the same ID (guards against a duplicate deleted record in DPF's pool
            // that happens to match an already-live loaded diary).
            ClaimBookFormId(static_cast<RE::FormID>(row.bookFormId), data.actorUuid);

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

        // Lazy FormID lookup — prefer the authoritative stored actorFormId; fall back
        // to UUID roundtrip only for volumes that pre-date this field (actorFormId == 0).
        if (vol->cachedActorFormId == 0) {
            if (vol->actorFormId != 0) {
                vol->cachedActorFormId = vol->actorFormId;
            } else {
                vol->cachedActorFormId = Database::GetFormIDForUUID(vol->actorUuid);
            }
        }
        if (vol->cachedActorFormId == 0) return;

        const int MAX_ENTRIES = Config::GetSingleton()->GetEntriesPerVolume();
        double queryStart = (vol->volumeNumber == 1) ? 0.0 : vol->startTime;

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

        // For sealed volumes, cap to MAX_ENTRIES so boundary tie-breaking is deterministic.
        auto liveEntries = Database::GetVolumeEntries(
            vol->cachedActorFormId, queryStart, queryEnd,
            vol->prevVolumeLastCreationTime, vol->prevVolumeCountAtBoundary,
            vol->endTime > 0.0 ? MAX_ENTRIES : 0);
        int liveCount = static_cast<int>(liveEntries.size());

        // Fast path: nothing changed and cache is warm with current-format text — nothing to do.
        // If the cached text is in the old format (no <font> tags, generated before font-tag
        // support was added), fall through to force a one-time regeneration even when the
        // entry count hasn't changed.  This upgrades stale DB rows automatically on first open.
        // EXCEPTION: if liveCount == 0 we have nothing to regenerate FROM — in that case
        // the cached text (e.g. externally loaded DB text) must be preserved as-is regardless
        // of format.  Regenerating with an empty entry list would replace real content with
        // "All entries removed", which is wrong for test/imported books.
        bool textIsCurrentFormat = vol->cachedBookText.find("<font face='") != std::string::npos;
        if (liveCount == vol->lastKnownEntryCount && !vol->cachedBookText.empty() && textIsCurrentFormat) {
            return;
        }
        // EXCEPTION (see comment above): nothing to regenerate FROM when liveCount==0 —
        // preserve whatever cached text exists, regardless of format.  This handles both
        // old-format rows and current-format text for test/imported entries that were never
        // written to the SkyrimNet API DB.
        if (liveCount == 0 && !vol->cachedBookText.empty()) {
            SKSE::log::info("[SNPD] {} vol {} has no live API entries — preserving cached text (test/imported data)",
                vol->actorName, vol->volumeNumber);
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
