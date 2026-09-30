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
#include "DynamicForms.h"
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

        // Sorts the entries and creates volumes of EntriesPerVolume from startingVolumeNumber.  firstPrev* are the
        // boundary data with the existing previous volume (see docs/VOLUMES_AND_SYNC.md#volume-boundaries).

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

                // How many previous-chunk entries share this chunk's first date: an exact count, so identical
                // entries aren't over-removed (see docs/VOLUMES_AND_SYNC.md#volume-boundaries).
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

    // ModEvent-triggered diary update for single actor

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

    bool PlayerDiaryFrozen(RE::FormID actorFormId) {
        return actorFormId == 0x14 && !SkyrimNetDiaries::Config::GetSingleton()->GetPlayerDiaryBooks();
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
        if (PlayerDiaryFrozen(formId)) {
            SKSE::log::debug("PlayerDiaryBooks is off: no diary update for the player");
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

            auto latestVolume = bookManager->GetBookForActor(uuid);

            if (!latestVolume) {
                // No volumes at all: build every volume from all entries ever written.
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

            // Backfill bioTemplateName for older co-saves that didn't store it.  GetBookForActor returns a raw
            // pointer into the stored vector: safe to mutate on the game thread.
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

            // A new entry can be dated at or before the volume's end (same game moment, or a Keep revert), which the
            // date test misses: count the entries from the volume's start instead.
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

            // NPC has the book: fetch the whole range, not maxEntries + 1, since SkyrimNet's limit keeps the
            // newest entries and would drop this volume's oldest ones.
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

                bookManager->UpdateBookEndTime(*latestVolume, cutTime);
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
                bookManager->UpdateBookEndTime(*latestVolume, currentVolumeEntries.back().entry_date);
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

        struct Removal {
            std::string uuid;
            VolumeKind kind;
            std::string name;
            int fromVolume;
            std::vector<RE::FormID> books;
        };
        std::vector<Removal> removals;
        int reRendered = 0;

        // Each actor's diary, and the player's journals, on its own.
        for (auto& [chain, volumes] : bookManager->GetAllBooksRef()) {
            if (volumes.empty()) continue;
            if (volumes.front().kind == VolumeKind::Written) {
                // Journals hold their tagged entries whatever their dates, and a journal made
                // after the save has no book in it (LoadFromDB dropped it): only re-render.
                const RE::FormID formId = Database::GetFormIDForUUID(volumes.front().actorUuid);
                if (formId == 0) continue;
                for (auto& vol : volumes) {
                    bool ok = false;
                    const auto live = bookManager->GetLiveEntries(vol, formId, 0.0, &ok);
                    if (!ok) break;
                    if (static_cast<int>(live.size()) == vol.lastKnownEntryCount) continue;
                    SKSE::log::info("[Timeline] {} journal {}: {} → {} entries", vol.actorName, vol.volumeNumber,
                                    vol.lastKnownEntryCount, live.size());
                    bookManager->SetVolumeText(vol, live);
                    ++reRendered;
                }
                continue;
            }
            if (!DatedAfter(volumes.back().endTime, now)) continue;
            const std::string uuid = volumes.front().actorUuid;
            const RE::FormID formId = Database::GetFormIDForUUID(uuid);
            if (formId == 0) continue;

            // Trailing volumes left with no entries.
            Removal tail{ uuid, volumes.front().kind, volumes.back().actorName, 0, {} };
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
                if (static_cast<int>(live.size()) == vol.lastKnownEntryCount &&
                    live.back().entry_date == vol.endTime) {
                    continue;  // SkyrimNet kept this history: the volume is unchanged
                }
                SKSE::log::info("[Timeline] {} vol {}: {} → {} entries, end {:.2f} → {:.2f}",
                                vol.actorName, vol.volumeNumber, vol.lastKnownEntryCount, live.size(),
                                vol.endTime, live.back().entry_date);
                bookManager->UpdateBookEndTime(vol, live.back().entry_date);
                bookManager->SetVolumeText(vol, live);
                ++reRendered;
            }
            if (tail.fromVolume != 0 && !queryFailed) removals.push_back(std::move(tail));
        }

        for (auto& r : removals) {
            SKSE::log::info("[Timeline] {}: volumes from {} have no entries left in SkyrimNet — removing {} book(s)",
                            r.name, r.fromVolume, r.books.size());
            // Also retires the books.
            bookManager->UnregisterVolumesFrom(r.uuid, r.kind, r.fromVolume);
        }
        if (!removals.empty()) {
            // Take the retired books from the NPCs, the loaded cells and merchant chests.
            SKSE::GetTaskInterface()->AddTask([]() { SweepRetiredBooks(); });
        }

        if (reRendered > 0 || !removals.empty()) {
            SKSE::log::info("[Timeline] Reconciled with SkyrimNet's history ({}): {} volume(s) re-rendered, {} actor(s) lost volumes",
                            TimelineGate::Outcome(), reRendered, removals.size());
        }
    }

    // At load, queues an update for actors whose latest volume misses entries SkyrimNet has (written while SNPD
    // wasn't listening: KEEP on a revert, dashboard edits).
    void QueueNewEntryRecovery(std::unordered_set<std::string>& skipUuids) {
        const auto& allBooks = SkyrimNetDiaries::BookManager::GetSingleton()->GetAllBooks();
        int recoveryCount = 0;

        for (const auto& [chain, volumes] : allBooks) {
            if (volumes.empty()) continue;
            const auto* latest = &volumes.back();  // volumes are sorted by number
            // Only diaries grow from SkyrimNet's entries; the player's journal only in the editor.
            if (latest->kind != VolumeKind::Generated) continue;
            const std::string& uuid = latest->actorUuid;
            if (skipUuids.count(uuid)) continue;  // already queued for immediate recovery

            uint32_t actorFormId = SkyrimNetDiaries::Database::GetFormIDForUUID(uuid);
            if (actorFormId == 0) continue;
            if (PlayerDiaryFrozen(actorFormId)) continue;

            bool needsUpdate = false;
            if (latest->endTime > 0.0) {
                // Probe for any entry strictly after the latest volume's end: entries SNPD never saw.
                const auto checkEntries = SkyrimNetDiaries::Database::GetDiaryEntries(
                    actorFormId, 1, latest->endTime + 0.001, 0.0);
                if (!checkEntries.empty()) {
                    SKSE::log::info("[Recovery] {} vol {} ends at {:.2f} but newer entries exist — queuing update",
                                   latest->actorName, latest->volumeNumber, latest->endTime);
                    needsUpdate = true;
                }
            } else {
                // Only rows from early versions lack an endTime: compare the entry count from the volume's start
                // with the count it was last rendered with.
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
            SKSE::log::info("[Recovery] Queued {} actor(s) with entries newer than their latest volume", recoveryCount);
        }
    }

    namespace {

        // Load-time catch-up scan: paged discovery of actors with entries, then one task per actor without volumes
        // (see docs/VOLUMES_AND_SYNC.md#on-load-recovery-and-catch-up).

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
                // Next page across all actors, paging backward from the oldest timestamp seen (0.0: no upper bound).
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
                    if (bookManager->GetBookForActor(uuid)) { ++skippedHaveBooks; continue; }
                    ++queued;

                    taskInterface->AddTask(
                        [uuid, name, generation = state->generation]() {
                            if (generation != g_syncGeneration.load()) return;
                            try {
                                // Skip if volumes appeared between queue time and execution.
                                auto* bm = SkyrimNetDiaries::BookManager::GetSingleton();
                                if (bm->GetBookForActor(uuid)) return;

                                RE::FormID formId = SkyrimNetDiaries::Database::GetFormIDForUUID(uuid);
                                if (formId == 0) {
                                    SKSE::log::warn("CatchUp: cannot resolve UUID {} to FormID, skipping", uuid);
                                    return;
                                }
                                if (PlayerDiaryFrozen(formId)) return;

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

    // MCM Reset (docs/DATABASE.md#mcm-reset-resetalldiariesinternal): retires every diary book, keeps the journals,
    // never touches SkyrimNet's entries.  Returns the number of actors cleared (negative on exception).
    int ResetAllDiariesInternal() {
        SKSE::log::info("ResetAllDiariesInternal: starting");

        auto bookManager   = SkyrimNetDiaries::BookManager::GetSingleton();

        try {
            // The player's journals are kept as they are (docs/DATABASE.md#mcm-reset): they hold
            // what the player wrote, and a copy of their DiaryDB rows goes back after the wipe.
            std::unordered_set<std::string> actors;
            std::vector<DiaryBookData> journals;
            for (const auto& [chain, volumes] : bookManager->GetAllBooks()) {
                if (volumes.empty()) continue;
                if (volumes.front().kind == VolumeKind::Written) {
                    journals.insert(journals.end(), volumes.begin(), volumes.end());
                } else {
                    actors.insert(volumes.front().actorUuid);
                }
            }
            const int actorsAffected = static_cast<int>(actors.size());

            // Wipe DiaryDB rows (and theft tracking) first: otherwise LoadFromDB reads them back on the next load
            // and the catch-up scan skips regeneration, so Reset seems to stop working after one cycle.
            auto* diaryDb = SkyrimNetDiaries::DiaryDB::GetSingleton();
            if (diaryDb->IsOpen()) {
                int dbRowsCleared = 0;
                for (const auto& uuid : actors) {
                    if (diaryDb->DeleteActor(uuid)) ++dbRowsCleared;
                    diaryDb->ClearAllStolenVolumes(uuid);
                }
                SKSE::log::info("ResetAllDiariesInternal: deleted {} actor(s) from DiaryDB", dbRowsCleared);
                DiaryTheftHandler::SyncStolenCache();
            } else {
                SKSE::log::warn("ResetAllDiariesInternal: DiaryDB not open — DB rows NOT deleted "
                                "(the next load recreates the diaries)");
            }

            // Retire every book of ours, including any this session couldn't match to a volume: they stay in the
            // save and show the "all entries removed" page.
            bookManager->Revert();
            SaveFolder::Clear();
            int retired = 0;
            for (const auto& record : DynamicForms::Tracked()) {
                std::string uuid;
                VolumeKind kind{};
                int volume = 0;
                ParseVolumeKey(record.key, uuid, kind, volume);
                if (kind == VolumeKind::Written) continue;
                bookManager->RetireBook(record.formId, uuid, kind);
                ++retired;
            }
            for (auto& journal : journals) bookManager->RegisterBook(std::move(journal));

            SKSE::GetTaskInterface()->AddTask([]() { SweepRetiredBooks(); });

            SKSE::log::info("ResetAllDiariesInternal: retired {} book(s) for {} actor(s), kept {} journal(s)", retired,
                            actorsAffected, journals.size());
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
