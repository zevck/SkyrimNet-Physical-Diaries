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

#include "VolumeSync.h"
#include "ActorLookup.h"
#include "BookCreation.h"
#include "BookManager.h"
#include "BookText.h"
#include "Config.h"
#include "Database.h"
#include "DiaryDB.h"
#include "DiaryTheftHandler.h"
#include "Localization.h"
#include "SaveFolder.h"
#include "TimelineGate.h"
#include <atomic>
#include <chrono>
#include <thread>
#include <unordered_map>

namespace SkyrimNetDiaries {

    namespace {

        std::atomic<bool> g_postLoadSyncReady{ true };
        std::atomic<bool> g_booksPaused{ false };
        // Bumped whenever a load starts, so deferred events from the previous load are dropped.
        std::atomic<std::uint32_t> g_syncGeneration{ 0 };

        // How often a deferred diary event checks whether the post-load sync has run.
        constexpr auto kDeferPollInterval = std::chrono::milliseconds(500);

        // Entries per discovery page in the catch-up scan.
        constexpr int kDiscoveryPageSize = 50;

        // The actor's display name: SkyrimNet's, else the game's.  "" if neither knows it.
        std::string ResolveActorName(const std::string& uuid, RE::FormID formId, std::string known = {}) {
            if (known.empty()) known = SkyrimNetDiaries::Database::GetActorName(uuid);
            if (known.empty()) {
                if (auto* actor = RE::TESForm::LookupByID<RE::Actor>(formId)) known = actor->GetName();
            }
            return known;
        }

        // =============================================================================
        // Create all diary volumes for an actor from a flat list of entries.
        // Entries are sorted oldest-first and chunked into EntriesPerVolume batches.
        // startingVolumeNumber is 1 for a fresh actor, or latestVolume+1 for additions.
        // firstPrevLastCreationTime / firstPrevCountAtBoundary are the boundary data
        // between the existing volume startingVolumeNumber-1 and the first new one
        // (0 when there is no shared entry_date to tie-break).
        // =============================================================================

        void CreateAllVolumesForActor(
            const std::string& uuid,
            const std::string& actorName,
            RE::FormID formId,
            const std::string& bioTemplateName,
            std::vector<SkyrimNetDiaries::DiaryEntry> allEntries,
            int startingVolumeNumber,
            double firstPrevLastCreationTime = 0.0,
            int firstPrevCountAtBoundary = 0)
        {
            if (allEntries.empty()) return;

            std::sort(allEntries.begin(), allEntries.end(), EntryOlder);

            const int chunkSize = SkyrimNetDiaries::Config::GetSingleton()->GetEntriesPerVolume();
            auto bookManager = SkyrimNetDiaries::BookManager::GetSingleton();
            int volumeNumber = startingVolumeNumber;

            for (size_t offset = 0; offset < allEntries.size(); offset += chunkSize, ++volumeNumber) {
                size_t end = std::min(offset + static_cast<size_t>(chunkSize), allEntries.size());
                std::vector<SkyrimNetDiaries::DiaryEntry> chunk(allEntries.begin() + offset,
                                                                allEntries.begin() + end);

                // The creation_time of the last entry in the previous chunk is used by GetDiaryEntries
                // to exclude it from this volume when both volumes share the same entry_date boundary.
                double prevChunkLastCreationTime = (offset == 0) ? firstPrevLastCreationTime
                                                                 : allEntries[offset - 1].creation_time;

                // Count how many entries in the previous chunk share the boundary date with this chunk's
                // first entry.  Those entries would be returned by the API query for this volume (because
                // their entry_date >= this volume's startTime) but must be excluded.  Storing the exact
                // count prevents over-removal when two entries are truly identical (same entry_date AND
                // creation_time), which is the root cause of the "entry #10 missing" bug.
                int prevChunkCountAtBoundary = (offset == 0) ? firstPrevCountAtBoundary : 0;
                if (offset > 0) {
                    double boundaryDate = chunk.front().entry_date;
                    for (int i = static_cast<int>(offset) - 1; i >= 0 && allEntries[i].entry_date == boundaryDate; --i) {
                        ++prevChunkCountAtBoundary;
                    }
                }

                // volume 1 uses 0.0 (no lower bound); later volumes start at their first entry's date,
                // which under normal circumstances is strictly greater than the previous chunk's last date.
                double volStart = (volumeNumber == 1) ? 0.0 : chunk.front().entry_date;
                double volEnd   = chunk.back().entry_date;

                SKSE::log::debug("Creating volume {} for {} ({} entries, {:.2f}\u2013{:.2f})",
                               volumeNumber, actorName, chunk.size(), volStart, volEnd);

                bookManager->CreateDiaryBook(uuid, actorName, volStart, volumeNumber, formId, chunk,
                                             bioTemplateName, prevChunkLastCreationTime, prevChunkCountAtBoundary);
            }
        }

    } // namespace

    // =============================================================================
    // ModEvent-triggered diary update for single actor
    // =============================================================================

    void SetPostLoadSyncReady(bool ready) {
        if (!ready) {
            ++g_syncGeneration;
            g_booksPaused.store(false);  // a new session gets a fresh start
        }
        g_postLoadSyncReady.store(ready);
    }

    void PauseDiaryBooks() {
        g_booksPaused.store(true);
    }

    bool IsPostLoadSyncReady() {
        return g_postLoadSyncReady.load();
    }

    void DeferUntilSyncReady(RE::FormID formId, void (*handler)(RE::FormID)) {
        const auto generation = g_syncGeneration.load();
        std::thread([formId, handler, generation]() {
            std::this_thread::sleep_for(kDeferPollInterval);
            SKSE::GetTaskInterface()->AddTask([formId, handler, generation]() {
                if (generation != g_syncGeneration.load()) return;  // a newer load took over
                handler(formId);
            });
        }).detach();
    }

    void UpdateDiaryForActorInternal(RE::FormID formId) {
        SKSE::log::debug("=== UpdateDiaryForActorInternal called for FormID 0x{:X} ===", formId);

        if (!g_postLoadSyncReady.load()) {
            SKSE::log::debug("Diary update for FormID 0x{:X} waiting for the post-load sync", formId);
            DeferUntilSyncReady(formId, UpdateDiaryForActorInternal);
            return;
        }
        if (g_booksPaused.load()) {
            SKSE::log::debug("Diary books are paused this session — ignoring the entry for FormID 0x{:X}", formId);
            return;
        }

        // Initialize SkyrimNet API if not already done
        if (!SkyrimNetDiaries::Database::InitializeAPI()) {
            SKSE::log::error("Failed to initialize SkyrimNet API - diary update skipped");
            return;
        }

        // Check if memory system is ready
        if (!SkyrimNetDiaries::Database::IsMemorySystemReady()) {
            SKSE::log::warn("SkyrimNet memory system not ready yet - diary update deferred");
            return;
        }

        try {
            auto bookManager = SkyrimNetDiaries::BookManager::GetSingleton();
            std::string uuid = SkyrimNetDiaries::Database::GetUUIDFromFormID(formId);

            if (uuid.empty()) {
                SKSE::log::warn("Could not resolve FormID 0x{:X} to UUID - skipping update", formId);
                return;
            }

            const std::string actorName = ResolveActorName(uuid, formId);
            if (actorName.empty()) {
                SKSE::log::warn("UpdateDiaryForActor: could not resolve actor name for FormID 0x{:X}, skipping", formId);
                return;
            }
            SKSE::log::debug("Processing diary update for {} (UUID: {})", actorName, uuid);

            // Volumes still being created aren't in books_ yet: deciding now would
            // create them a second time.  Try again once they're registered.
            if (HasPendingCreations(uuid)) {
                SKSE::log::debug("{} has volumes still being created — update waits", actorName);
                DeferUntilSyncReady(formId, UpdateDiaryForActorInternal);
                return;
            }

            auto latestVolume = bookManager->GetBookForActor(uuid);

            if (!latestVolume) {
                // ----------------------------------------------------------------
                // No volumes at all — fetch every entry ever written and build all
                // volumes from the beginning in chronological order.
                // ----------------------------------------------------------------
                auto allEntries = SkyrimNetDiaries::Database::GetDiaryEntries(formId, kFetchAllEntries, 0.0, 0.0);

                if (allEntries.empty()) {
                    SKSE::log::debug("No diary entries for {} (UUID: {})", actorName, uuid);
                    return;
                }

                SKSE::log::info("First-time init for {}: {} total entries → creating all volumes from 1",
                               actorName, allEntries.size());

                std::string bioTemplateName = SkyrimNetDiaries::Database::GetTemplateNameByUUID(uuid);
                CreateAllVolumesForActor(uuid, actorName, formId, bioTemplateName, std::move(allEntries), 1);
                return;
            }

            // Volumes exist — only care about entries strictly newer than the latest volume's end.
            // Backfill bioTemplateName for saves loaded from an older co-save that didn't store it.
            // GetBookForActor returns a raw pointer into the stored vector — safe to mutate on the game thread.
            if (latestVolume->bioTemplateName.empty()) {
                latestVolume->bioTemplateName = SkyrimNetDiaries::Database::GetTemplateNameByUUID(uuid);
                if (!latestVolume->bioTemplateName.empty()) {
                    SKSE::log::debug("Backfilled bioTemplateName '{}' for {} (migrated save)",
                                   latestVolume->bioTemplateName, actorName);
                }
            }

            // GetDiaryEntries startTime is inclusive, so we fetch from endTime and then strip
            // any entries whose timestamp is <= endTime (they belong to the previous volume).
            auto newEntries = SkyrimNetDiaries::Database::GetDiaryEntries(formId, kFetchAllEntries, latestVolume->endTime, 0.0);
            newEntries.erase(
                std::remove_if(newEntries.begin(), newEntries.end(),
                    [&](const SkyrimNetDiaries::DiaryEntry& e) { return e.entry_date <= latestVolume->endTime; }),
                newEntries.end());

            // A new entry can be dated at or before the latest volume's end: written at
            // the same game moment as its last entry, or after a Keep revert (history
            // newer than the loaded save stays in SkyrimNet).  The date test above misses
            // those, so count the entries from the volume's start instead.
            bool datedInsideVolume = false;
            if (newEntries.empty()) {
                const auto sinceStart = SkyrimNetDiaries::Database::GetVolumeEntries(formId, {
                    .startTime = latestVolume->startTime,
                    .prevLastCreationTime = latestVolume->prevVolumeLastCreationTime,
                    .prevCountAtBoundary = latestVolume->prevVolumeCountAtBoundary });
                if (static_cast<int>(sinceStart.size()) <= latestVolume->lastKnownEntryCount) {
                    SKSE::log::debug("No new entries for {} since {:.2f}", actorName, latestVolume->endTime);
                    return;
                }
                datedInsideVolume = true;
                SKSE::log::info("{}: {} new entries dated at or before the end of volume {}",
                                actorName, sinceStart.size() - latestVolume->lastKnownEntryCount, latestVolume->volumeNumber);
            }

            SKSE::log::debug("Found {} new entries for {} since {:.2f}", newEntries.size(), actorName, latestVolume->endTime);

            auto npcActor = RE::TESForm::LookupByID<RE::Actor>(formId);
            if (!npcActor) {
                SKSE::log::warn("Actor 0x{:X} not found - cannot update diary", formId);
                return;
            }

            // Check if NPC still has the latest volume
            const bool npcHasBook = CountInInventory(
                npcActor, RE::TESForm::LookupByID<RE::TESBoundObject>(latestVolume->bookFormId)) > 0;

            if (!npcHasBook && datedInsideVolume) {
                SKSE::log::warn("{} no longer has volume {} and the new entries are dated inside it — not added to a book",
                                actorName, latestVolume->volumeNumber);
                return;
            }
            if (!npcHasBook) {
                // Player took it or it was stolen — start a fresh volume.
                SKSE::log::info("{} no longer has volume {} — creating new volumes from {} ({} new entries)",
                               actorName, latestVolume->volumeNumber, latestVolume->volumeNumber + 1, newEntries.size());
                std::string bioTemplateName = SkyrimNetDiaries::Database::GetTemplateNameByUUID(uuid);
                CreateAllVolumesForActor(uuid, actorName, formId, bioTemplateName, std::move(newEntries),
                                         latestVolume->volumeNumber + 1);
                return;
            }

            // NPC has the book: every entry from the volume's start, oldest first.  The
            // whole range is needed, not maxEntries + 1: SkyrimNet's limit keeps the
            // newest entries, which would drop this volume's oldest ones.
            const int maxEntries = SkyrimNetDiaries::Config::GetSingleton()->GetEntriesPerVolume();
            auto currentVolumeEntries = SkyrimNetDiaries::Database::GetVolumeEntries(formId, {
                .startTime = latestVolume->startTime,
                .prevLastCreationTime = latestVolume->prevVolumeLastCreationTime,
                .prevCountAtBoundary = latestVolume->prevVolumeCountAtBoundary });
            if (currentVolumeEntries.empty()) {
                SKSE::log::warn("{} volume {}: no entries from its start time — skipping update",
                                actorName, latestVolume->volumeNumber);
                return;
            }

            if (static_cast<int>(currentVolumeEntries.size()) >= maxEntries) {
                // Volume is full — seal it with exactly maxEntries entries, writing the final text,
                // then route everything after the cut into new overflow volumes.
                std::vector<SkyrimNetDiaries::DiaryEntry> finalizedEntries(
                    currentVolumeEntries.begin(),
                    currentVolumeEntries.begin() + maxEntries);
                std::vector<SkyrimNetDiaries::DiaryEntry> overflowEntries(
                    currentVolumeEntries.begin() + maxEntries,
                    currentVolumeEntries.end());
                const double cutTime = finalizedEntries.back().entry_date;

                bookManager->UpdateBookEndTime(uuid, latestVolume->volumeNumber, cutTime);
                bookManager->SetVolumeText(*latestVolume, finalizedEntries);

                SKSE::log::info("{} volume {} sealed at {} entries (cutTime {:.2f}), {} overflow entries → creating new volumes",
                               actorName, latestVolume->volumeNumber, maxEntries, cutTime, overflowEntries.size());

                if (!overflowEntries.empty()) {
                    // Boundary data for the first overflow volume: how many sealed entries
                    // share its first entry_date, and the last sealed entry's creation_time.
                    const double boundaryDate = overflowEntries.front().entry_date;
                    const int countAtBoundary = static_cast<int>(std::count_if(
                        finalizedEntries.begin(), finalizedEntries.end(),
                        [boundaryDate](const SkyrimNetDiaries::DiaryEntry& e) { return e.entry_date == boundaryDate; }));
                    const double lastSealedCreationTime = finalizedEntries.back().creation_time;

                    const int nextVolume = latestVolume->volumeNumber + 1;
                    std::string bioTemplateName = SkyrimNetDiaries::Database::GetTemplateNameByUUID(uuid);
                    CreateAllVolumesForActor(uuid, actorName, formId, bioTemplateName, std::move(overflowEntries),
                                             nextVolume, lastSealedCreationTime, countAtBoundary);
                }
            } else {
                // Update the current volume in place.
                bookManager->UpdateBookEndTime(uuid, latestVolume->volumeNumber, currentVolumeEntries.back().entry_date);
                bookManager->SetVolumeText(*latestVolume, currentVolumeEntries);

                SKSE::log::info("Updated {} volume {} with {} entries",
                               actorName, latestVolume->volumeNumber, currentVolumeEntries.size());
            }

        } catch (const std::exception& e) {
            SKSE::log::error("Exception in UpdateDiaryForActor: {}", e.what());
        } catch (...) {
            SKSE::log::error("Unknown exception in UpdateDiaryForActor");
        }
    }

    void ReconcileWithTimeline() {
        const double now = SkyrimNetDiaries::CurrentGameTimeSeconds();
        if (now <= 0.0) return;
        auto* bookManager = BookManager::GetSingleton();

        struct Removal { std::string uuid; std::string name; RE::FormID actorFormId; int fromVolume; std::vector<RE::FormID> books; };
        std::vector<Removal> removals;
        int reRendered = 0;

        for (auto& [uuid, volumes] : bookManager->GetAllBooksRef()) {
            if (volumes.empty() || !DatedAfter(volumes.back().endTime, now)) continue;
            const RE::FormID formId = Database::GetFormIDForUUID(uuid);
            if (formId == 0) continue;

            Removal tail{ uuid, volumes.back().actorName, formId, 0, {} };  // trailing volumes left with no entries
            bool queryFailed = false;
            for (auto& vol : volumes) {
                if (!DatedAfter(vol.endTime, now)) continue;
                bool ok = false;
                const auto live = bookManager->GetLiveEntries(vol, formId, vol.endTime, &ok);
                if (!ok) {
                    // A failed read is not "SkyrimNet cleared these entries": leave this
                    // actor's volumes alone rather than delete them.
                    SKSE::log::warn("[Timeline] {} vol {}: couldn't read entries from SkyrimNet — leaving this diary as it is",
                                    vol.actorName, vol.volumeNumber);
                    queryFailed = true;
                    break;
                }
                if (live.empty()) {
                    if (tail.fromVolume == 0) tail.fromVolume = vol.volumeNumber;
                    tail.books.push_back(vol.bookFormId);
                    continue;
                }
                tail.fromVolume = 0;  // only a trailing run of empty volumes is dropped
                tail.books.clear();
                if (vol.persistedInSave && DatedAfter(live.front().entry_date, now)) {
                    // Every entry is newer than this save, so the save never had the
                    // volume; a later save did (an in-session revert, then Keep).  Let
                    // QueueInventoryCheck give the book back to the NPC.
                    vol.persistedInSave = false;
                    DiaryDB::GetSingleton()->ClearPersisted(uuid, vol.volumeNumber);
                    SKSE::log::info("[Timeline] {} vol {} is newer than this save — returning it to the NPC",
                                    vol.actorName, vol.volumeNumber);
                }
                if (static_cast<int>(live.size()) == vol.lastKnownEntryCount &&
                    live.back().entry_date == vol.endTime) {
                    continue;  // SkyrimNet kept this history: the volume is unchanged
                }
                SKSE::log::info("[Timeline] {} vol {}: {} → {} entries, end {:.2f} → {:.2f}",
                                vol.actorName, vol.volumeNumber, vol.lastKnownEntryCount, live.size(),
                                vol.endTime, live.back().entry_date);
                bookManager->UpdateBookEndTime(uuid, vol.volumeNumber, live.back().entry_date);
                bookManager->SetVolumeText(vol, live);
                ++reRendered;
            }
            if (tail.fromVolume != 0 && !queryFailed) removals.push_back(std::move(tail));
        }

        for (auto& r : removals) {
            SKSE::log::info("[Timeline] {}: volumes from {} have no entries left in SkyrimNet — removing {} book(s)",
                            r.name, r.fromVolume, r.books.size());
            bookManager->UnregisterVolumesFrom(r.uuid, r.fromVolume);
            // Take the books back from the NPC.  Forms stay alive (never Dispose,
            // see docs/BOOK_FORMS.md).
            SKSE::GetTaskInterface()->AddTask([actorFormId = r.actorFormId, books = r.books, name = r.name]() {
                auto* npc = RE::TESForm::LookupByID<RE::Actor>(actorFormId);
                if (!npc) return;
                for (const auto bookId : books) {
                    auto* book = RE::TESForm::LookupByID<RE::TESObjectBOOK>(bookId);
                    if (!book) continue;
                    if (const auto count = CountInInventory(npc, book); count > 0) {
                        npc->RemoveItem(book, count, RE::ITEM_REMOVE_REASON::kRemove, nullptr, nullptr);
                        SKSE::log::info("[Timeline] Removed book 0x{:X} from {}'s inventory", bookId, name);
                    }
                }
            });
        }

        if (reRendered > 0 || !removals.empty()) {
            SKSE::log::info("[Timeline] Reconciled with SkyrimNet's history ({}): {} volume(s) re-rendered, {} actor(s) lost volumes",
                            TimelineGate::Outcome(), reRendered, removals.size());
        }
    }

    // =============================================================================
    // QueueSealedVolumeRecovery — detect entries written after a sealed volume
    // (the revert+KEEP scenario: SkyrimNet retains entries our DB didn't track).
    // For each actor whose latest volume is sealed, probes SkyrimNet for any entry
    // strictly after the seal timestamp. If found, queues UpdateDiaryForActorInternal.
    // =============================================================================
    void QueueSealedVolumeRecovery(std::unordered_set<std::string>& skipUuids) {
        const auto& allBooks = SkyrimNetDiaries::BookManager::GetSingleton()->GetAllBooks();
        int recoveryCount = 0;

        for (const auto& [uuid, volumes] : allBooks) {
            if (volumes.empty()) continue;
            if (skipUuids.count(uuid)) continue;  // already queued for immediate recovery

            const auto* latest = &volumes.back();  // volumes are sorted by number

            uint32_t actorFormId = SkyrimNetDiaries::Database::GetFormIDForUUID(uuid);
            if (actorFormId == 0) continue;

            bool needsUpdate = false;
            if (latest->endTime > 0.0) {
                // Probe for any entry strictly after the latest volume's end: entries
                // SkyrimNet has that SNPD never saw (the KEEP choice on a revert, or
                // dashboard entries while the game was paused).
                const auto checkEntries = SkyrimNetDiaries::Database::GetDiaryEntries(
                    actorFormId, 1, latest->endTime + 0.001, 0.0);
                if (!checkEntries.empty()) {
                    SKSE::log::info("[Recovery] {} vol {} ends at {:.2f} but newer entries exist — queuing update",
                                   latest->actorName, latest->volumeNumber, latest->endTime);
                    needsUpdate = true;
                }
            } else {
                // Legacy rows without an end: compare the entry count from the volume's
                // start with the count it was last rendered with.
                const int liveCount = static_cast<int>(SkyrimNetDiaries::Database::GetDiaryEntries(
                    actorFormId, kFetchAllEntries, latest->startTime, 0.0).size());
                if (liveCount > latest->lastKnownEntryCount) {
                    SKSE::log::info("[Recovery] {} vol {} (open) has {} live entries vs {} known — queuing update",
                                   latest->actorName, latest->volumeNumber, liveCount, latest->lastKnownEntryCount);
                    needsUpdate = true;
                }
            }

            if (needsUpdate) {
                const RE::FormID fid = static_cast<RE::FormID>(actorFormId);
                SKSE::GetTaskInterface()->AddTask([fid]() { UpdateDiaryForActorInternal(fid); });
                ++recoveryCount;
                skipUuids.insert(uuid);
            }
        }

        if (recoveryCount > 0) {
            SKSE::log::info("[Recovery] Queued {} actor(s) for sealed-volume recovery", recoveryCount);
        }
    }

    namespace {

        // =============================================================================
        // Queued batch catch-up scan used on save load.
        //
        // Pass 1 (discovery): Calls PublicGetDiaryEntries(formId=0) in pages of
        //   kDiscoveryPageSize entries to cheaply discover which actor UUIDs have any
        //   diary content, without loading every entry into memory at once.  Each page
        //   is one game-thread task, chained until a short page comes back.
        //
        // Pass 2 (per-actor fetch): Once discovery finishes, one task per actor does a
        //   full GetDiaryEntries call for just that FormID, creates all its volumes,
        //   then frees the memory.  Tasks run on successive game-thread ticks so the
        //   load is spread out.
        //
        // Actors that already have volumes (or have volumes being created) are skipped,
        // as are those the recovery pass already queued.
        // =============================================================================

        // Shared state carried across discovery batch tasks via shared_ptr.
        struct DiscoveryState {
            std::unordered_map<std::string, std::string> actorUuidToName; // uuid -> name
            double oldestTimestampSeen = 0.0; // lower bound for next page query
            std::unordered_set<std::string> skip;  // UUIDs already queued for immediate recovery
            std::uint32_t generation = 0;          // the load this scan belongs to
        };

        // Forward declaration so QueueBatchCatchUpScan can reference it.
        void RunDiscoveryBatch(std::shared_ptr<DiscoveryState> state);

    } // namespace

    void QueueBatchCatchUpScan(std::unordered_set<std::string> skipUuids) {
        SKSE::log::debug("QueueBatchCatchUpScan: checking all actors for missing diary books");
        auto state = std::make_shared<DiscoveryState>();
        state->skip = std::move(skipUuids);
        state->generation = g_syncGeneration.load();
        SKSE::GetTaskInterface()->AddTask([state]() { RunDiscoveryBatch(state); });
    }

    namespace {

        void RunDiscoveryBatch(std::shared_ptr<DiscoveryState> state) {
            // A load (or new game) since the scan started: books_ belongs to that session now.
            if (state->generation != g_syncGeneration.load()) return;
            try {
                // Fetch the next page of entries across all actors.
                // endTime=0.0 on the first call means no upper bound.
                // On subsequent calls we pass the oldest timestamp seen so far to page backward.
                double endTime = state->oldestTimestampSeen;
                auto rawEntries = SkyrimNetDiaries::Database::GetDiaryEntries(0, kDiscoveryPageSize, 0.0, endTime);
                bool morePages = (static_cast<int>(rawEntries.size()) >= kDiscoveryPageSize);

                // endTime is treated as inclusive by the API (same as startTime), so
                // filter out any entries at or after the boundary to avoid re-processing.
                if (state->oldestTimestampSeen > 0.0) {
                    rawEntries.erase(
                        std::remove_if(rawEntries.begin(), rawEntries.end(),
                            [&](const SkyrimNetDiaries::DiaryEntry& e) {
                                return e.entry_date >= state->oldestTimestampSeen;
                            }),
                        rawEntries.end());
                }

                if (rawEntries.empty() && morePages) {
                    // Entire batch was boundary duplicates — stop to avoid an infinite loop.
                    morePages = false;
                }

                // Accumulate actor UUIDs and find the oldest timestamp for the next page.
                for (const auto& e : rawEntries) {
                    if (!e.actor_uuid.empty()) {
                        state->actorUuidToName.emplace(e.actor_uuid, e.actor_name);
                    }
                    if (state->oldestTimestampSeen == 0.0 || e.entry_date < state->oldestTimestampSeen) {
                        state->oldestTimestampSeen = e.entry_date;
                    }
                }

                SKSE::log::debug("DiscoveryBatch: got {} entries ({}), {} distinct actors so far",
                                rawEntries.size(), morePages ? "more pages" : "last page",
                                state->actorUuidToName.size());

                if (morePages) {
                    // Chain the next discovery batch as a separate task.
                    SKSE::GetTaskInterface()->AddTask([state]() { RunDiscoveryBatch(state); });
                    return;
                }

                // Discovery complete — queue one full-fetch task per actor.
                if (state->actorUuidToName.empty()) {
                    SKSE::log::debug("QueueBatchCatchUpScan: no actors with diary entries found");
                    return;
                }

                auto bookManager = SkyrimNetDiaries::BookManager::GetSingleton();
                auto taskInterface = SKSE::GetTaskInterface();
                int queued = 0, skippedRecovering = 0, skippedHaveBooks = 0;

                for (const auto& [uuid, name] : state->actorUuidToName) {
                    // Skip actors already being handled by immediate recovery.
                    if (state->skip.count(uuid)) { ++skippedRecovering; continue; }
                    // Skip actors that got volumes from a regular diary event during discovery.
                    if (bookManager->GetBookForActor(uuid) || HasPendingCreations(uuid)) { ++skippedHaveBooks; continue; }
                    ++queued;

                    taskInterface->AddTask(
                        [uuid, name, generation = state->generation]() {
                            if (generation != g_syncGeneration.load()) return;
                            try {
                                // Skip if volumes appeared between queue time and execution.
                                auto* bm = SkyrimNetDiaries::BookManager::GetSingleton();
                                if (bm->GetBookForActor(uuid) || HasPendingCreations(uuid)) return;

                                RE::FormID formId = SkyrimNetDiaries::Database::GetFormIDForUUID(uuid);
                                if (formId == 0) {
                                    SKSE::log::warn("CatchUp: cannot resolve UUID {} to FormID, skipping", uuid);
                                    return;
                                }

                                auto entries = SkyrimNetDiaries::Database::GetDiaryEntries(formId);
                                if (entries.empty()) return;

                                const std::string actorName = ResolveActorName(uuid, formId, name);
                                if (actorName.empty()) {
                                    SKSE::log::warn("CatchUp: could not resolve actor name for UUID {} (FormID 0x{:X}), skipping", uuid, formId);
                                    return;
                                }

                                // formId already in hand — skip the redundant UUID→FormID call inside GetTemplateNameByUUID.
                                std::string bioTemplate = SkyrimNetDiaries::Database::GetBioTemplateName(formId);
                                SKSE::log::info("CatchUp: creating books for {} ({} entries)", actorName, entries.size());
                                CreateAllVolumesForActor(uuid, actorName, formId, bioTemplate, std::move(entries), 1);

                            } catch (const std::exception& e) {
                                SKSE::log::error("CatchUp task exception for {}: {}", name, e.what());
                            } catch (...) {
                                SKSE::log::error("CatchUp task unknown exception for {}", name);
                            }
                        });
                }

                SKSE::log::info("QueueBatchCatchUpScan: discovery found {} actor(s) with entries: {} queued, "
                                "{} already being recreated, {} already have books",
                                state->actorUuidToName.size(), queued, skippedRecovering, skippedHaveBooks);

            } catch (const std::exception& e) {
                SKSE::log::error("RunDiscoveryBatch exception: {}", e.what());
            } catch (...) {
                SKSE::log::error("RunDiscoveryBatch unknown exception");
            }
        }

    } // namespace

    // =============================================================================
    // MCM Reset: remove all tracked diary books from NPC inventories and clear all
    // BookManager and DiaryDB tracking.  SkyrimNet diary ENTRIES are
    // NOT touched - books are recreated by the next diary event or load (catch-up).
    // Returns the number of actor records cleared (negative on exception).
    // =============================================================================
    int ResetAllDiariesInternal() {
        SKSE::log::info("ResetAllDiariesInternal: starting");

        // Creations still queued would otherwise register volumes after the reset.
        CancelPendingCreations();

        auto bookManager   = SkyrimNetDiaries::BookManager::GetSingleton();

        int actorsAffected = 0;
        int booksRemoved   = 0;

        try {
            const auto& allBooks = bookManager->GetAllBooks();

            // Every tracked book, removed from inventories on the game thread below.
            struct RemovalEntry { RE::FormID bookFormId; std::string label; };
            std::vector<RemovalEntry> pendingRemovals;

            for (const auto& [uuid, volumes] : allBooks) {
                if (volumes.empty()) continue;
                ++actorsAffected;

                for (const auto& vol : volumes) {
                    pendingRemovals.push_back({vol.bookFormId,
                        vol.actorName + " vol " + std::to_string(vol.volumeNumber)});

                    ++booksRemoved;
                }
            }

            // Wipe DiaryDB rows BEFORE clearing in-memory state.  Without this,
            // LoadFromDB() on the next save reload reads the persisted rows back,
            // the catch-up scan sees actors already have books, and skips
            // regeneration — making Reset appear to "stop working" after one cycle.
            // DeleteActor removes from both `volumes` and `actor_templates` tables.
            // Also clear stolen-volume tracking so theft state doesn't linger.
            auto* diaryDb = SkyrimNetDiaries::DiaryDB::GetSingleton();
            if (diaryDb->IsOpen()) {
                int dbRowsCleared = 0;
                for (const auto& [uuid, volumes] : allBooks) {
                    if (diaryDb->DeleteActor(uuid)) ++dbRowsCleared;
                    diaryDb->ClearAllStolenVolumes(uuid);
                }
                SKSE::log::info("ResetAllDiariesInternal: deleted {} actor(s) from DiaryDB", dbRowsCleared);
                DiaryTheftHandler::SyncStolenCache();
            } else {
                SKSE::log::warn("ResetAllDiariesInternal: DiaryDB not open — DB rows NOT deleted "
                                "(reload will restore the diaries)");
            }

            // Clear all in-memory tracking immediately (safe — no game-thread state involved).
            bookManager->Revert();
            SaveFolder::Clear();

            // Dispatch inventory removals to the game thread.
            if (!pendingRemovals.empty()) {
                SKSE::GetTaskInterface()->AddTask([pendingRemovals]() {
                    for (const auto& entry : pendingRemovals) {
                        auto* bookForm = RE::TESForm::LookupByID<RE::TESObjectBOOK>(entry.bookFormId);
                        if (!bookForm) {
                            SKSE::log::warn("  Reset: book form 0x{:X} not found for {}", entry.bookFormId, entry.label);
                            continue;
                        }

                        // ForEachReference covers the active worldspace/interior, so books in
                        // nearby NPC inventories, containers, shelves, etc. are removed
                        // immediately without waiting for a reload.
                        {
                            auto* tesWorld = RE::TES::GetSingleton();
                            if (tesWorld) {
                                tesWorld->ForEachReference([&](RE::TESObjectREFR* ref) -> RE::BSContainer::ForEachResult {
                                    if (!ref || ref->IsDeleted()) return RE::BSContainer::ForEachResult::kContinue;
                                    if (const auto count = CountInInventory(ref, bookForm); count > 0) {
                                        ref->RemoveItem(bookForm, count,
                                            RE::ITEM_REMOVE_REASON::kRemove, nullptr, nullptr);
                                        SKSE::log::debug("  Removed {} from ref 0x{:X} ({})",
                                            entry.label, ref->GetFormID(),
                                            ref->GetBaseObject() ? ref->GetBaseObject()->GetName() : "?");
                                    }
                                    return RE::BSContainer::ForEachResult::kContinue;
                                });
                            }
                        }

                        // The form is never Disposed or SetDeleted: both poison DPF's
                        // recycle pool and later hand one FormID to two actors.  See
                        // docs/BOOK_FORMS.md.
                    }
                });
            }

            SKSE::log::info("ResetAllDiariesInternal: cleared {} volumes for {} actor(s), {} inventory removals queued",
                            booksRemoved, actorsAffected, pendingRemovals.size());
            return actorsAffected;

        } catch (const std::exception& e) {
            SKSE::log::error("ResetAllDiariesInternal exception: {}", e.what());
            return -1;
        } catch (...) {
            SKSE::log::error("ResetAllDiariesInternal: unknown exception");
            return -1;
        }
    }

} // namespace SkyrimNetDiaries
