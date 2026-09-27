#include "VolumeSync.h"
#include "BookManager.h"
#include "BookText.h"
#include "Config.h"
#include "Database.h"
#include "DiaryDB.h"
#include "Localization.h"
#include "SaveFolder.h"
#include <unordered_map>

namespace SkyrimNetDiaries {

    namespace {

        // =============================================================================
        // Create all diary volumes for an actor from a flat list of entries.
        // Entries are sorted oldest-first and chunked into EntriesPerVolume batches.
        // startingVolumeNumber is 1 for a fresh actor, or latestVolume+1 for additions.
        // =============================================================================

        void CreateAllVolumesForActor(
            const std::string& uuid,
            const std::string& actorName,
            RE::FormID formId,
            const std::string& bioTemplateName,
            std::vector<SkyrimNetDiaries::DiaryEntry> allEntries,
            int startingVolumeNumber)
        {
            if (allEntries.empty()) return;

            // Sort oldest-first by (entry_date, creation_time).
            std::sort(allEntries.begin(), allEntries.end(),
                [](const SkyrimNetDiaries::DiaryEntry& a, const SkyrimNetDiaries::DiaryEntry& b) {
                    if (a.entry_date != b.entry_date) return a.entry_date < b.entry_date;
                    return a.creation_time < b.creation_time;
                });

            const int chunkSize = SkyrimNetDiaries::Config::GetSingleton()->GetEntriesPerVolume();
            auto bookManager = SkyrimNetDiaries::BookManager::GetSingleton();
            int volumeNumber = startingVolumeNumber;

            for (size_t offset = 0; offset < allEntries.size(); offset += chunkSize, ++volumeNumber) {
                size_t end = std::min(offset + static_cast<size_t>(chunkSize), allEntries.size());
                std::vector<SkyrimNetDiaries::DiaryEntry> chunk(allEntries.begin() + offset,
                                                                allEntries.begin() + end);

                // The creation_time of the last entry in the previous chunk is used by GetDiaryEntries
                // to exclude it from this volume when both volumes share the same entry_date boundary.
                double prevChunkLastCreationTime = (offset == 0) ? 0.0 : allEntries[offset - 1].creation_time;

                // Count how many entries in the previous chunk share the boundary date with this chunk's
                // first entry.  Those entries would be returned by the API query for this volume (because
                // their entry_date >= this volume's startTime) but must be excluded.  Storing the exact
                // count prevents over-removal when two entries are truly identical (same entry_date AND
                // creation_time), which is the root cause of the "entry #10 missing" bug.
                int prevChunkCountAtBoundary = 0;
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

                bookManager->CreateDiaryBook(uuid, actorName, volStart, volEnd,
                                             volumeNumber, formId, chunk, bioTemplateName,
                                             prevChunkLastCreationTime, prevChunkCountAtBoundary);
            }
        }

    } // namespace

    // =============================================================================
    // ModEvent-triggered diary update for single actor
    // =============================================================================

    void UpdateDiaryForActorInternal(RE::FormID formId) {
        SKSE::log::debug("=== UpdateDiaryForActorInternal called for FormID 0x{:X} ===", formId);

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

            if (uuid.empty() || uuid == "0") {
                SKSE::log::warn("Could not resolve FormID 0x{:X} to UUID - skipping update", formId);
                return;
            }

            std::string actorName = SkyrimNetDiaries::Database::GetActorName(uuid);
            // Fallback: if SkyrimNet hasn't registered this NPC yet, use the RE game name directly.
            if (actorName.empty()) {
                if (auto* actor = RE::TESForm::LookupByID<RE::Actor>(formId)) {
                    actorName = actor->GetName();
                }
            }
            if (actorName.empty()) {
                SKSE::log::warn("UpdateDiaryForActor: could not resolve actor name for FormID 0x{:X}, skipping", formId);
                return;
            }
            SKSE::log::debug("Processing diary update for {} (UUID: {})", actorName, uuid);

            auto latestVolume = bookManager->GetBookForActor(uuid);

            if (!latestVolume) {
                // ----------------------------------------------------------------
                // No volumes at all — fetch every entry ever written and build all
                // volumes from the beginning in chronological order.
                // ----------------------------------------------------------------
                auto allEntries = SkyrimNetDiaries::Database::GetDiaryEntries(formId, 10000, 0.0, 0.0);

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
            auto newEntries = SkyrimNetDiaries::Database::GetDiaryEntries(formId, 10000, latestVolume->endTime, 0.0);
            newEntries.erase(
                std::remove_if(newEntries.begin(), newEntries.end(),
                    [&](const SkyrimNetDiaries::DiaryEntry& e) { return e.entry_date <= latestVolume->endTime; }),
                newEntries.end());

            if (newEntries.empty()) {
                // No entries after the recorded endTime.  If this volume was never persisted
                // to a save and its endTime is ahead of current game-time (stale from a
                // quit-without-save + reload scenario), do a full rebuild from all entries.
                if (!latestVolume->persistedInSave) {
                    auto calendar = RE::Calendar::GetSingleton();
                    double currentTime = calendar ? calendar->GetCurrentGameTime() * 86400.0 : 0.0;
                    if (latestVolume->endTime > currentTime) {
                        SKSE::log::info("Stale endTime {:.2f} > currentTime {:.2f} for {} — rebuilding from all entries",
                                       latestVolume->endTime, currentTime, actorName);
                        auto bookManager2 = SkyrimNetDiaries::BookManager::GetSingleton();
                        bookManager2->UnregisterBook(uuid);
                        auto allEntries = SkyrimNetDiaries::Database::GetDiaryEntries(formId, 10000, 0.0, 0.0);
                        if (!allEntries.empty()) {
                            std::string bioTemplateName = SkyrimNetDiaries::Database::GetTemplateNameByUUID(uuid);
                            CreateAllVolumesForActor(uuid, actorName, formId, bioTemplateName, std::move(allEntries), 1);
                        }
                        return;
                    }
                }
                SKSE::log::debug("No new entries for {} since {:.2f}", actorName, latestVolume->endTime);
                return;
            }

            SKSE::log::debug("Found {} new entries for {} since {:.2f}", newEntries.size(), actorName, latestVolume->endTime);

            auto npcActor = RE::TESForm::LookupByID<RE::Actor>(formId);
            if (!npcActor) {
                SKSE::log::warn("Actor 0x{:X} not found - cannot update diary", formId);
                return;
            }

            // Check if NPC still has the latest volume
            bool npcHasBook = false;
            auto inv = npcActor->GetInventory();
            for (const auto& [item, invData] : inv) {
                if (item->GetFormID() == latestVolume->bookFormId && invData.first > 0) {
                    npcHasBook = true;
                    break;
                }
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

            // NPC has the book — check if it's full
            const int MAX_ENTRIES = SkyrimNetDiaries::Config::GetSingleton()->GetEntriesPerVolume();
            auto currentVolumeEntries = SkyrimNetDiaries::Database::GetDiaryEntries(
                formId, MAX_ENTRIES + 1, latestVolume->startTime, 0.0);

            if (static_cast<int>(currentVolumeEntries.size()) >= MAX_ENTRIES) {
                // Volume is full — seal it with exactly MAX_ENTRIES entries, writing the final text,
                // then route everything strictly after the cut timestamp into new overflow volumes.

                // Sort oldest → newest before sealing.
                std::sort(currentVolumeEntries.begin(), currentVolumeEntries.end(),
                    [](const SkyrimNetDiaries::DiaryEntry& a, const SkyrimNetDiaries::DiaryEntry& b) {
                        if (a.entry_date != b.entry_date) return a.entry_date < b.entry_date;
                        return a.creation_time < b.creation_time;
                    });

                // Finalized slice: exactly the first MAX_ENTRIES entries.
                std::vector<SkyrimNetDiaries::DiaryEntry> finalizedEntries(
                    currentVolumeEntries.begin(),
                    currentVolumeEntries.begin() + MAX_ENTRIES);
                double cutTime = finalizedEntries.back().entry_date;

                // Write the sealed volume with its complete, final content.
                std::string bookName = SkyrimNetDiaries::Localization::GetSingleton()->FormatBookName(actorName, latestVolume->volumeNumber);
                std::string sealedText = FormatDiaryEntries(finalizedEntries, actorName,
                                                            latestVolume->startTime, cutTime, MAX_ENTRIES);
                SkyrimNetDiaries::DiaryDB::GetSingleton()->UpdateBookText(
                    uuid, latestVolume->volumeNumber, sealedText, MAX_ENTRIES);
                latestVolume->cachedBookText = sealedText;
                bookManager->UpdateBookEndTime(uuid, latestVolume->volumeNumber, cutTime);
                bookManager->UpdateVolumeEntryCount(uuid, latestVolume->volumeNumber, MAX_ENTRIES);

                // Overflow: entries in currentVolumeEntries beyond MAX_ENTRIES.
                // Note: currentVolumeEntries already includes both old and new entries, so we don't
                // need to separately add from newEntries (doing so would duplicate entries).
                std::vector<SkyrimNetDiaries::DiaryEntry> overflowEntries;
                for (size_t oi = static_cast<size_t>(MAX_ENTRIES); oi < currentVolumeEntries.size(); ++oi) {
                    overflowEntries.push_back(currentVolumeEntries[oi]);
                }

                SKSE::log::info("{} volume {} sealed at {} entries (cutTime {:.2f}), {} overflow entries → creating new volumes",
                               actorName, latestVolume->volumeNumber, MAX_ENTRIES, cutTime, overflowEntries.size());

                if (!overflowEntries.empty()) {
                    std::string bioTemplateName = SkyrimNetDiaries::Database::GetTemplateNameByUUID(uuid);
                    CreateAllVolumesForActor(uuid, actorName, formId, bioTemplateName, std::move(overflowEntries),
                                             latestVolume->volumeNumber + 1);
                }
            } else {
                // Update the current volume in place
                std::string bookName = SkyrimNetDiaries::Localization::GetSingleton()->FormatBookName(actorName, latestVolume->volumeNumber);

                // Sort current volume entries oldest-first before formatting.
                std::sort(currentVolumeEntries.begin(), currentVolumeEntries.end(),
                    [](const SkyrimNetDiaries::DiaryEntry& a, const SkyrimNetDiaries::DiaryEntry& b) {
                        if (a.entry_date != b.entry_date) return a.entry_date < b.entry_date;
                        return a.creation_time < b.creation_time;
                    });

                double newEndTime = currentVolumeEntries.back().entry_date;
                std::string bookText = FormatDiaryEntries(currentVolumeEntries, actorName,
                                                          latestVolume->startTime, newEndTime, MAX_ENTRIES);
                int newEntryCount = static_cast<int>(currentVolumeEntries.size());
                SkyrimNetDiaries::DiaryDB::GetSingleton()->UpdateBookText(
                    uuid, latestVolume->volumeNumber, bookText, newEntryCount);
                latestVolume->cachedBookText = bookText;

                bookManager->UpdateBookEndTime(uuid, latestVolume->volumeNumber, newEndTime);
                bookManager->UpdateVolumeEntryCount(uuid, latestVolume->volumeNumber, newEntryCount);

                SKSE::log::info("Updated {} volume {} with {} entries",
                               actorName, latestVolume->volumeNumber, currentVolumeEntries.size());
            }

        } catch (const std::exception& e) {
            SKSE::log::error("Exception in UpdateDiaryForActor: {}", e.what());
        } catch (...) {
            SKSE::log::error("Unknown exception in UpdateDiaryForActor");
        }
    }

    // =============================================================================
    // QueueSealedVolumeRecovery — detect entries written after a sealed volume
    // (the revert+KEEP scenario: SkyrimNet retains entries our DB didn't track).
    // For each actor whose latest volume is sealed, probes SkyrimNet for any entry
    // strictly after the seal timestamp. If found, queues UpdateDiaryForActorInternal.
    // =============================================================================
    void QueueSealedVolumeRecovery(const std::unordered_set<std::string>& skipUuids) {
        const auto& allBooks = SkyrimNetDiaries::BookManager::GetSingleton()->GetAllBooks();
        int recoveryCount = 0;

        for (const auto& [uuid, volumes] : allBooks) {
            if (volumes.empty()) continue;
            if (skipUuids.count(uuid)) continue;  // already queued for immediate recovery

            // Find the latest volume.
            const SkyrimNetDiaries::DiaryBookData* latest = nullptr;
            for (const auto& vol : volumes) {
                if (!latest || vol.volumeNumber > latest->volumeNumber) {
                    latest = &vol;
                }
            }
            if (!latest) continue;

            uint32_t actorFormId = SkyrimNetDiaries::Database::GetFormIDForUUID(uuid);
            if (actorFormId == 0) continue;

            if (latest->persistedInSave == false) {
                // Volume was never committed to a .ess save.  If the current game-time
                // is earlier than the volume's endTime the player reverted to an older
                // save and the volume's time range is now stale — force a rebuild so
                // new entries at the reverted game-time aren't silently dropped.
                auto calendar = RE::Calendar::GetSingleton();
                if (calendar && latest->endTime > 0.0) {
                    double currentTime = calendar->GetCurrentGameTime() * 86400.0;
                    if (latest->endTime > currentTime) {
                        SKSE::log::info("[Recovery] {} vol {} not persisted and endTime {:.2f} > current {:.2f} — queuing forced rebuild",
                                       latest->actorName, latest->volumeNumber, latest->endTime, currentTime);
                        RE::FormID fid = static_cast<RE::FormID>(actorFormId);
                        SKSE::GetTaskInterface()->AddTask([fid]() {
                            UpdateDiaryForActorInternal(fid);
                        });
                        ++recoveryCount;
                        continue;
                    }
                }
            }

            if (latest->endTime > 0.0) {
                // Sealed volume: probe for any entry strictly after the seal timestamp.
                // This handles the revert+KEEP scenario where SkyrimNet retained entries
                // our DB didn't track.
                auto checkEntries = SkyrimNetDiaries::Database::GetDiaryEntries(
                    actorFormId, 1, latest->endTime + 0.001, 0.0);

                if (!checkEntries.empty()) {
                    SKSE::log::info("[Recovery] {} vol {} sealed at {:.2f} but new entries exist — queuing update",
                                   latest->actorName, latest->volumeNumber, latest->endTime);
                    RE::FormID fid = static_cast<RE::FormID>(actorFormId);
                    SKSE::GetTaskInterface()->AddTask([fid]() {
                        UpdateDiaryForActorInternal(fid);
                    });
                    ++recoveryCount;
                }
            } else {
                // Open volume (endTime == 0): check whether SkyrimNet wrote new entries
                // while the game was paused (e.g. via dashboard) that the mod event never
                // delivered.  Use the volume's startTime as the lower bound — anything
                // strictly after the latest tracked entry_date is new.
                // We fetch 2 entries from startTime so we can count how many the current
                // volume already accounts for vs how many now exist.
                auto allSinceStart = SkyrimNetDiaries::Database::GetDiaryEntries(
                    actorFormId, 10000, latest->startTime, 0.0);

                int liveCount = static_cast<int>(allSinceStart.size());

                // Compare against lastKnownEntryCount — this is the count from the last
                // time UpdateBookText ran, persisted in DiaryDB.  Any positive delta means
                // SkyrimNet wrote entries (e.g. via dashboard while paused) that the book
                // text doesn't yet include.  This catches both the sub-overflow case
                // (new entries but still under maxPerVolume) and the overflow case.
                if (liveCount > latest->lastKnownEntryCount) {
                    SKSE::log::info("[Recovery] {} vol {} (open) has {} live entries vs {} known — queuing update",
                                   latest->actorName, latest->volumeNumber, liveCount,
                                   latest->lastKnownEntryCount);
                    RE::FormID fid = static_cast<RE::FormID>(actorFormId);
                    SKSE::GetTaskInterface()->AddTask([fid]() {
                        UpdateDiaryForActorInternal(fid);
                    });
                    ++recoveryCount;
                }
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
        // Pass 1 (discovery): Calls PublicGetDiaryEntries(formId=0) in pages of 50
        //   entries to cheaply discover which actor UUIDs have any diary content,
        //   without loading every entry into memory at once.  Each page is one game-
        //   thread task, chained until fewer than 50 raw entries are returned.
        //
        // Pass 2 (per-actor fetch): Once discovery finishes, one task per actor does a
        //   full GetDiaryEntries call for just that FormID, creates all its volumes,
        //   then frees the memory.  Tasks run on successive game-thread ticks so the
        //   load is spread out.
        //
        // The whole scan is skipped if any volumes are already tracked (i.e. this save
        // has been loaded before with the mod active).
        // =============================================================================

        // Shared state carried across discovery batch tasks via shared_ptr.
        struct DiscoveryState {
            std::unordered_map<std::string, std::string> actorUuidToName; // uuid -> name
            double oldestTimestampSeen = 0.0; // lower bound for next page query
            std::unordered_set<std::string> skip;  // UUIDs already queued for immediate recovery
        };

        // Forward declaration so QueueBatchCatchUpScan can reference it.
        void RunDiscoveryBatch(std::shared_ptr<DiscoveryState> state);

    } // namespace

    void QueueBatchCatchUpScan(std::unordered_set<std::string> skipUuids) {
        SKSE::log::debug("QueueBatchCatchUpScan: checking all actors for missing diary books");
        auto state = std::make_shared<DiscoveryState>();
        state->skip = std::move(skipUuids);
        SKSE::GetTaskInterface()->AddTask([state]() { RunDiscoveryBatch(state); });
    }

    namespace {

        void RunDiscoveryBatch(std::shared_ptr<DiscoveryState> state) {
            try {
                // Fetch next page of up to 50 entries across all actors.
                // endTime=0.0 on the first call means no upper bound.
                // On subsequent calls we pass the oldest timestamp seen so far to page backward.
                double endTime = state->oldestTimestampSeen;
                auto rawEntries = SkyrimNetDiaries::Database::GetDiaryEntries(0, 50, 0.0, endTime);
                bool morePages = (static_cast<int>(rawEntries.size()) >= 50);

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

                SKSE::log::info("QueueBatchCatchUpScan: discovery done, queuing {} per-actor tasks",
                                state->actorUuidToName.size());

                auto bookManager = SkyrimNetDiaries::BookManager::GetSingleton();
                auto taskInterface = SKSE::GetTaskInterface();

                for (const auto& [uuid, name] : state->actorUuidToName) {
                    // Skip actors already being handled by immediate recovery.
                    if (state->skip.count(uuid)) continue;
                    // Skip actors that got volumes from a regular diary event during discovery.
                    if (bookManager->GetBookForActor(uuid)) continue;

                    taskInterface->AddTask(
                        [uuid, name]() {
                            try {
                                // Skip if volumes appeared between queue time and execution.
                                auto* bm = SkyrimNetDiaries::BookManager::GetSingleton();
                                if (bm->GetBookForActor(uuid)) return;

                                RE::FormID formId = SkyrimNetDiaries::Database::GetFormIDForUUID(uuid);
                                if (formId == 0) {
                                    SKSE::log::warn("CatchUp: cannot resolve UUID {} to FormID, skipping", uuid);
                                    return;
                                }

                                auto entries = SkyrimNetDiaries::Database::GetDiaryEntries(
                                    formId, 10000, 0.0, 0.0);
                                if (entries.empty()) return;

                                // Resolve actor name — fall back to RE game name if the diary JSON had no name.
                                std::string actorName = name;
                                if (actorName.empty()) {
                                    actorName = SkyrimNetDiaries::Database::GetActorName(uuid);
                                }
                                if (actorName.empty()) {
                                    if (auto* actor = RE::TESForm::LookupByID<RE::Actor>(formId)) {
                                        actorName = actor->GetName();
                                    }
                                }
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
    // NOT touched - books will be regenerated on the next diary event or Rebuild.
    // Returns the number of actor records cleared (negative on exception).
    // =============================================================================
    int ResetAllDiariesInternal() {
        SKSE::log::info("ResetAllDiariesInternal: starting");

        auto bookManager   = SkyrimNetDiaries::BookManager::GetSingleton();

        int actorsAffected = 0;
        int booksRemoved   = 0;

        try {
            const auto& allBooks = bookManager->GetAllBooks();

            // Build a flat list of (uuid, bookFormId) pairs to remove from inventory.
            // Inventory removal MUST happen on the game thread — queue a single task for it.
            // We use uuid (not a cached FormID) so ESL load-order shifts don't matter.
            struct RemovalEntry { std::string uuid; RE::FormID bookFormId; std::string label; };
            std::vector<RemovalEntry> pendingRemovals;

            // Collect UUIDs while we iterate — needed below for DiaryDB row deletion
            // (must be captured before bookManager->Revert() empties the map).
            std::vector<std::string> uuidsToDelete;
            uuidsToDelete.reserve(allBooks.size());

            for (const auto& [uuid, volumes] : allBooks) {
                if (volumes.empty()) continue;
                ++actorsAffected;
                uuidsToDelete.push_back(uuid);

                for (const auto& vol : volumes) {
                    pendingRemovals.push_back({uuid, vol.bookFormId,
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
            if (diaryDb && diaryDb->IsOpen()) {
                int dbRowsCleared = 0;
                for (const auto& uuid : uuidsToDelete) {
                    if (diaryDb->DeleteActor(uuid)) ++dbRowsCleared;
                    diaryDb->ClearAllStolenVolumes(uuid);
                }
                SKSE::log::info("ResetAllDiariesInternal: deleted {} actor(s) from DiaryDB", dbRowsCleared);
            } else {
                SKSE::log::warn("ResetAllDiariesInternal: DiaryDB not open — DB rows NOT deleted "
                                "(reload will restore the diaries)");
            }

            // Clear all in-memory tracking immediately (safe — no game-thread state involved).
            bookManager->Revert();
            SaveFolder::Clear();

            // Dispatch inventory removals to the game thread.  We deliberately do NOT
            // dispose the DPF forms (see the per-entry comment below) to avoid
            // poisoning DPF's FormID recycle pool.
            if (!pendingRemovals.empty()) {
                SKSE::GetTaskInterface()->AddTask([pendingRemovals]() {
                    for (const auto& entry : pendingRemovals) {
                        auto* bookForm = RE::TESForm::LookupByID<RE::TESObjectBOOK>(entry.bookFormId);
                        if (!bookForm) {
                            SKSE::log::warn("  Reset: book form 0x{:X} not found for {}", entry.bookFormId, entry.label);
                            continue;
                        }

                        // --- Step 1: Sweep all currently-loaded references ---
                        // ForEachReference covers the active worldspace/interior, so books in
                        // nearby NPC inventories, containers, shelves, etc. are removed
                        // immediately without waiting for a reload.
                        {
                            auto* tesWorld = RE::TES::GetSingleton();
                            if (tesWorld) {
                                RE::TESBoundObject* filterForm = bookForm;
                                auto filter = [filterForm](RE::TESBoundObject& obj) {
                                    return &obj == filterForm;
                                };
                                tesWorld->ForEachReference([&](RE::TESObjectREFR* ref) -> RE::BSContainer::ForEachResult {
                                    if (!ref || ref->IsDeleted()) return RE::BSContainer::ForEachResult::kContinue;
                                    auto inv = ref->GetInventory(filter);
                                    auto it  = inv.find(bookForm);
                                    if (it != inv.end() && it->second.first > 0) {
                                        ref->RemoveItem(bookForm, it->second.first,
                                            RE::ITEM_REMOVE_REASON::kRemove, nullptr, nullptr);
                                        SKSE::log::debug("  Removed {} from ref 0x{:X} ({})",
                                            entry.label, ref->GetFormID(),
                                            ref->GetBaseObject() ? ref->GetBaseObject()->GetName() : "?");
                                    }
                                    return RE::BSContainer::ForEachResult::kContinue;
                                });
                            }
                        }

                        // --- We intentionally do NOT Dispose or SetDelete the form ---
                        //
                        // DPF.Dispose() marks the form's FormRecord as `deleted`, which
                        // adds it to DPF's recycle pool.  DPF's AddForm() recycles deleted
                        // records by FormID before allocating fresh ones, and its persisted
                        // pool (co-save + cache file) accumulates DUPLICATE deleted records
                        // for the same FormID across repeated reset/reload cycles.  Those
                        // duplicates get recycled more than once, handing the SAME FormID to
                        // two different actors — the root cause of cross-linked diary content
                        // (e.g. "Frea's Diary" showing Fetri El's text).
                        //
                        // SetDelete(true) is also avoided: the form would be dropped on the
                        // next load, DPF's restore would fail, and DPF would re-mark the
                        // record deleted — re-poisoning the pool the same way.
                        //
                        // Leaving the form fully alive keeps DPF's lastFormId monotonically
                        // increasing, so every future Create() gets a unique FormID.  The
                        // orphaned book is inert: removed from all loaded inventories above,
                        // no longer in our DiaryDB, never re-added.  Trade-off: NPCs in
                        // unloaded cells keep a stale (untracked) copy until regeneration,
                        // and orphaned forms slowly accumulate — both harmless versus the
                        // alternative of corrupted, cross-linked diaries.
                        (void)bookForm;
                    }
                });
            }

            SKSE::log::info("ResetAllDiariesInternal: cleared {} volumes for {} actor(s), {} inventory removals queued",
                            booksRemoved, actorsAffected, pendingRemovals.size());
            return actorsAffected;

        } catch (const std::exception& e) {
            SKSE::log::error("ResetAllDiariesInternal exception: {}", e.what());
            return -1;
        }
    }

} // namespace SkyrimNetDiaries
