# Volumes and Synchronization

How SkyrimNet's diary entries become volumes, how volumes stay in step with SkyrimNet when entries are added or deleted, and how save reverts are handled.

Code: `src/VolumeSync.cpp` (`CreateAllVolumesForActor`, `UpdateDiaryForActorInternal`, `QueueSealedVolumeRecovery`, `QueueBatchCatchUpScan` / `RunDiscoveryBatch`), `src/Database.cpp` (`GetDiaryEntries`), `src/BookManager.cpp` (`RefreshVolumeOnOpen`), `src/TimelineGate.cpp` (waiting for SkyrimNet's keep/clear decision).

---

## Entries

`Database::GetDiaryEntries(formId, limit, startTime, endTime, &ok)` calls SkyrimNet's `PublicGetDiaryEntries` and parses the JSON into `DiaryEntry`:

| Field | Meaning |
|---|---|
| `actor_uuid`, `actor_name` | SkyrimNet identity. The UUID is deterministic per NPC and stable across saves. |
| `entry_date` | In-game time, **seconds** since game start. Compare with `RE::Calendar::GetCurrentGameTime() * 86400.0`. |
| `creation_time` | Real-world write time. Breaks ties between entries with the same `entry_date`. |
| `content` | The entry text (SkyrimNet's other fields, such as `location`, aren't read). |

The result is always sorted oldest first by `(entry_date, creation_time)` (`EntryOlder`). **The limit is applied by SkyrimNet to the newest entries** (its query is `ORDER BY entry_date DESC LIMIT n`), so a limited query returns the latest *n* entries in the range, not the first. The catch-up scan's backward paging, the recovery probe and `TimelineGate` rely on that. Whenever "a volume's entries" or "the first N" is meant, use `Database::GetVolumeEntries(formId, VolumeBounds, &ok)`, or `BookManager::GetLiveEntries(vol, …)`, which fills the bounds from the volume and its successor: it fetches the whole range and removes the entries the previous and next volumes own on a shared date (see [Volume boundaries](#volume-boundaries)). `formId = 0` returns entries for all actors (used by the catch-up scan). SkyrimNet's per-entry ID is **not** stable (it renumbers when older entries are deleted), so entries are identified by timestamps.

**Same-named generic NPCs share one diary.** SkyrimNet groups memory by actor name, so every "Whiterun Guard" is one identity and one set of volumes. That is upstream behaviour. The documented workaround is a unique-names mod.

---

## Volume boundaries

A volume is a time range of one actor's entries (`DiaryBookData` / DiaryDB `volumes`):

| Field | Meaning |
|---|---|
| `startTime` | 0 for volume 1; otherwise the `entry_date` of the volume's first entry |
| `endTime` | `entry_date` of the volume's last entry |
| `prevVolumeLastCreationTime`, `prevVolumeCountAtBoundary` | Tie-break data for the boundary with the previous volume |
| `lastKnownEntryCount` | Entry count at the last render. Used to detect additions and deletions. |

SkyrimNet's time bounds are **inclusive**, so if the last entries of volume *n* share an `entry_date` with the first entry of volume *n+1*, the query for *n+1* returns them too. `GetVolumeEntries` removes **exactly** `prevVolumeCountAtBoundary` entries that satisfy `entry_date <= startTime && creation_time <= prevVolumeLastCreationTime`, in sorted order. Storing the count, not just the timestamp, is what stops it removing too many when two entries are identical. That over-removal was the "entry #10 missing" bug. Don't simplify it to a timestamp comparison.

The same data bounds the earlier volume from above: `GetVolumeEntries` keeps, on the next volume's start date, only the next volume's `prevVolumeCountAtBoundary` entries (with `creation_time <= prevVolumeLastCreationTime`). The rest are the next volume's. `GetLiveEntries` looks the next volume up for this. **An existing volume's size comes only from these stored boundaries, never from the current `EntriesPerVolume`.** Until 2026-09-27 sealed volumes were capped at the current setting, so lowering it cut their last entries, and opening the book then moved `endTime` back and left those entries in no volume.

`CreateAllVolumesForActor(uuid, name, formId, bioTemplate, entries, startingVolume)` sorts the entries, cuts them into chunks of `EntriesPerVolume` (default 10), computes the boundary fields per chunk, and calls `CreateDiaryBook` for each.

---

## When an entry arrives: `UpdateDiaryForActorInternal`

Called from the `UpdateDiaryFromEvent` native (event listener), from load-time recovery, and from the catch-up scan. It needs the API initialized and SkyrimNet's memory system ready.

It waits rather than runs in two cases, re-running itself 500 ms later on the game thread (`DeferUntilSyncReady`):
- **From `kPreLoadGame` until the post-load sync has run** (`SetPostLoadSyncReady`). DiaryDB isn't loaded yet, so every actor would look new and get a duplicate volume 1. The event native waits too, so the theft clear doesn't land in the previous save's DB. A wait from an earlier load is dropped when a new load starts; that load's recovery and catch-up scans pick the entry up.
- **While the actor has volumes still being created** (`HasPendingCreations`, see [BOOK_FORMS.md](BOOK_FORMS.md#pending-creations-and-cancellation)). They aren't in `books_` yet, so deciding now would create them twice.

1. **No volumes yet** → fetch every entry (limit 10000) and create all volumes from 1.
2. **Volumes exist** → fetch entries after the latest volume's `endTime` (the API bound is inclusive, so entries `<= endTime` are dropped client-side).
   - **None by date:** count the entries from the volume's start instead. A new entry can be dated at or before the latest volume's end: written at the same game moment as its last entry (several entries often share one timestamp), or after a Keep revert, when history newer than the loaded save stays in SkyrimNet. The date test misses both. If the count is higher than `lastKnownEntryCount`, carry on as if there were new entries: when the NPC holds the volume, the path below re-fetches from its start and places them in date order. When the NPC doesn't, they can't start a clean new volume (their dates fall inside the previous one), so they are logged and left out. Otherwise, nothing to do.
   - **The NPC no longer holds the latest volume** (pickpocketed, traded, lost): start new volumes at latest + 1 from the new entries. The old volume stays frozen.
   - **The NPC holds it:** fetch every entry from the volume's `startTime` with `GetVolumeEntries` (including the volume's boundary data).
     - At or over the limit → **seal**: re-render with exactly `EntriesPerVolume` entries, set `endTime` to the last included entry, and create overflow volumes from the rest. The first overflow volume gets boundary data from the sealed volume (its last entry's `creation_time` and how many of its entries share the overflow's first `entry_date`).
     - Under the limit → re-render in place and move `endTime` forward.

Rendering writes the text to DiaryDB (`UpdateBookText`) and to `cachedBookText`, and updates `lastKnownEntryCount`.

"Sealed" means three things in this code. Here, a volume is sealed when it hits the entry limit. In `RefreshVolumeOnOpen`, a volume is sealed if a newer volume exists. In `QueueSealedVolumeRecovery`, it means `endTime > 0`, which is true of almost every volume, including the latest (see [KNOWN_ISSUES.md](KNOWN_ISSUES.md)).

---

## On load: recovery and catch-up

Both run at the end of the `kPostLoadGame` setup (see [ARCHITECTURE.md](ARCHITECTURE.md#startup-and-load-sequence)). They share a skip set: recovery skips actors already queued for recreation and adds every actor it queues, so the catch-up scan never also creates volumes for them. The catch-up scan additionally skips actors with pending creations.

**`QueueSealedVolumeRecovery`**, per actor with volumes, looking at the latest volume:
- `endTime > 0` → ask SkyrimNet for one entry after `endTime + 0.001`. If one exists → queue `UpdateDiaryForActorInternal`. This catches entries written while SNPD was not listening: the KEEP choice on a revert, or entries created from the SkyrimNet dashboard while the game was paused.
- `endTime == 0` (legacy rows only) → compare the live entry count with `lastKnownEntryCount`.

**`QueueBatchCatchUpScan`** finds actors who have entries but no volumes (first install on an existing save, or after Reset):
1. Discovery: `RunDiscoveryBatch` asks for 50 entries across all actors (`formId = 0`) per game-thread task, paging backward by the oldest timestamp seen, and collects the distinct UUIDs. A page made up only of boundary duplicates ends the scan, which prevents an infinite loop.
2. One task per discovered actor without volumes: resolve UUID → FormID, fetch all entries, create all volumes.

Each is spread over game-thread ticks to avoid a load-time hitch. The scan remembers which load it belongs to; if another load or a new game starts while it is still paging, the rest of it is dropped (its `books_` view would be the new session's).

---

## On open: `RefreshVolumeOnOpen`

Called by the book hook right before the text is injected. It asks SkyrimNet for the volume's live entries through `GetLiveEntries` (bounded by `endTime` if a newer volume exists; open-ended for the latest volume), and re-renders if the count differs from `lastKnownEntryCount` or the cached text predates font tags. `FormatDiaryEntries` renders exactly the entries it is given, so a newer entry the latest volume hasn't absorbed yet is shown (and counted) until the next update seals or extends the volume. Until 2026-09-27 it filtered them back out by `endTime` while still counting them, which left a blank last page that the inter-plugin API returned as the latest entry. If entries were deleted, it also moves a sealed volume's `endTime` back to the new last entry.

---

## Save reverts: the KEEP / CLEAR fork

When the player loads an older save, **SkyrimNet asks whether to clear its history after that save point or keep it.** SNPD must handle both, and they fail in opposite directions. The rule:

> A diary entry that SkyrimNet no longer has must never keep appearing in a book, and an entry SkyrimNet still has must never be silently dropped from one.

This is why DiaryDB is keyed to the SkyrimNet save folder rather than stored in the `.ess`: SNPD's state deliberately outlives the save file, so it can **reconcile against** SkyrimNet instead of being reverted with it.

### Waiting for the decision: `TimelineGate`

SkyrimNet asks the question from its own `kPostLoadGame` work, and its database reports ready well before the player answers. Syncing at that point builds books from "future" entries that a CLEAR then deletes, and the book left behind opens blank. So the post-load sync also waits for the timeline to settle:

1. Once SkyrimNet's database is ready, `TimelineGate::IsSettled` asks SkyrimNet's own question: is the player's latest event (`PublicGetRecentEvents(player, 1)`, `Database::GetPlayerLastEventTime`) later than the current game time? SkyrimNet prompts exactly then. If not, there is nothing to decide and the sync starts at once. That is every normal load, and also a load where only some *diary entries* lie in the future: SkyrimNet doesn't ask then, so its history stays as it is.
2. Otherwise it waits for SkyrimNet's prompt. A MinHook detour on `MessageBoxData::QueueMessage` (`RELOCATION_ID(51422, 52271)`) compares each queued box with the text of `skynet_DeleteHistoryMessage` (looked up by EditorID at `kDataLoaded`), and wraps that box's callback to see the button. Button 0 is Keep; anything else is Clear.
3. **Keep** → sync at once. **Clear** → sync once SkyrimNet has deleted the future entries (a few ms; 10 s cap). While the prompt is on screen there is no time limit.
4. A prompt is expected but not seen within 30 s (for example its text didn't match) → sync anyway and log a warning. If the player's future events disappear first, SkyrimNet has cleared them and the sync starts.

This is a stopgap until SkyrimNet's public API can report whether its timeline check is still pending (see [KNOWN_ISSUES.md](KNOWN_ISSUES.md)).

### KEEP and CLEAR

### Reconciling: `ReconcileWithTimeline`

Right after `LoadFromDB`, once the timeline is settled, every volume that ends after the loaded save's game time is checked against the entries SkyrimNet still has (by fetching its range again; the button the player pressed is only logged). This follows SkyrimNet's decision exactly, whether or not the volume was ever saved:

- **Clear** deleted those entries: the volume is re-rendered and its `endTime` moved back to its last live entry. A trailing run of volumes with **no** entries left is dropped from `books_` and DiaryDB, and their books are taken back from the NPC. This runs before `QueueInventoryCheck`, so dropped volumes are never re-added.
- **Keep** left them in place: counts and end times match, nothing changes. A later entry dated inside the latest volume is caught by the count check in `UpdateDiaryForActorInternal` (above).
- **A volume newer than the loaded save** that a later save included (an in-session revert, then Keep) is still `persisted_in_save`, so nothing would give it back to the NPC. When all its live entries are dated after the loaded game time, the flag is cleared (`DiaryDB::ClearPersisted`; the upsert can't lower it) and `QueueInventoryCheck` re-adds the book.
- **A failed query** (`ok = false`) is neither: the actor's volumes are left alone and a warning is logged, so a SkyrimNet error at load can't delete books.

It replaces the old game-time rebuild ("not saved and `endTime` after now"), which guessed from `persistedInSave` and missed saved volumes after an in-session revert, leaving new entries filtered out as older than the volume's end.

### KEEP and CLEAR outside a load

**KEEP (SkyrimNet kept entries SNPD never saw)** is also handled eagerly at load by `QueueSealedVolumeRecovery`, above.

**Deletions SNPD didn't see happen** (for example from the SkyrimNet dashboard) are handled lazily in `RefreshVolumeOnOpen`, which re-renders and **moves the sealed `endTime` back**. That `endTime` move is essential: without it, `QueueSealedVolumeRecovery` would look past the deleted entry's timestamp on the next load and create a duplicate volume. The two are coupled; don't change one without the other. Lazy is fine for CLEAR (nothing is wrong until someone reads the book), but `lastKnownEntryCount` stays stale until that volume is opened.

Backwards time travel also clears stolen-volume records at load (see [THEFT.md](THEFT.md#save-reverts)).

**Every entry deleted outside a load** (for example from the SkyrimNet dashboard): the next open renders the "all entries removed" page (`EmptyVolumeText`, marked with `kEmptySentinel`), and the inter-plugin API reports `NoEntries`. `RefreshVolumeOnOpen` only does this when the query succeeded: `GetVolumeEntries` reports a failed query (`ok = false`: SkyrimNet unavailable, an exception, an empty or non-array response) separately, and then the cached text is kept and a warning logged. (Until 2026-09-27 zero live entries always kept the old text, a guard for test/imported books that no longer exist.) SkyrimNet's own export answers `[]` on an internal error, which still looks like zero entries.

---

## Which save am I in?

SNPD's per-save state lives in `…/SkyrimNetPhysicalDiaries/<saveFolder>/diary.db`. The folder name is `SkyrimNet-<id>`, where `<id>` is the value on the last `Using save ID: ` line in `SkyrimNet.log` (`DetectSaveFolderFromLog`). It is only accepted if SkyrimNet's own `SkyrimNet-<id>.db` exists. SkyrimNet's API needs no save parameter: it always serves the active save. So both SNPD's DB and SkyrimNet's answers depend on SkyrimNet's idea of the current save.

This detection runs in the post-load sync, after the waits for SkyrimNet's database and timeline decision, by which time SkyrimNet has logged the new save ID.
