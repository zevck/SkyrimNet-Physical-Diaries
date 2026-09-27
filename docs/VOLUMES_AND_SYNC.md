# Volumes and Synchronization

How SkyrimNet's diary entries become volumes, how volumes stay in step with SkyrimNet when entries are added or deleted, and how save reverts are handled.

Code: `src/VolumeSync.cpp` (`CreateAllVolumesForActor`, `UpdateDiaryForActorInternal`, `QueueSealedVolumeRecovery`, `QueueBatchCatchUpScan` / `RunDiscoveryBatch`), `src/Database.cpp` (`GetDiaryEntries`), `src/BookManager.cpp` (`RefreshVolumeOnOpen`).

---

## Entries

`Database::GetDiaryEntries(formId, limit, startTime, endTime, prevVolumeLastCreationTime, prevVolumeCountAtBoundary)` calls SkyrimNet's `PublicGetDiaryEntries` and parses the JSON into `DiaryEntry`:

| Field | Meaning |
|---|---|
| `actor_uuid`, `actor_name` | SkyrimNet identity. The UUID is deterministic per NPC and stable across saves. |
| `entry_date` | In-game time, **seconds** since game start. Compare with `RE::Calendar::GetCurrentGameTime() * 86400.0`. |
| `creation_time` | Real-world write time. Breaks ties between entries with the same `entry_date`. |
| `content`, `location`, `emotion`, `importance_score` | Text and metadata. Only `content` is rendered. |

The result is always sorted oldest first by `(entry_date, creation_time)`. `formId = 0` returns entries for all actors (used by the catch-up scan). SkyrimNet's per-entry ID is **not** stable (it renumbers when older entries are deleted), so entries are identified by timestamps.

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

SkyrimNet's time bounds are **inclusive**, so if the last entries of volume *n* share an `entry_date` with the first entry of volume *n+1*, the query for *n+1* returns them too. `GetDiaryEntries` removes **exactly** `prevVolumeCountAtBoundary` entries that satisfy `entry_date <= startTime && creation_time <= prevVolumeLastCreationTime`, in sorted order. Storing the count, not just the timestamp, is what stops it removing too many when two entries are identical. That over-removal was the "entry #10 missing" bug. Don't simplify it to a timestamp comparison.

`CreateAllVolumesForActor(uuid, name, formId, bioTemplate, entries, startingVolume)` sorts the entries, cuts them into chunks of `EntriesPerVolume` (default 10), computes the boundary fields per chunk, and calls `CreateDiaryBook` for each.

---

## When an entry arrives: `UpdateDiaryForActorInternal`

Called from the `UpdateDiaryFromEvent` native (event listener), from load-time recovery, and from the catch-up scan. It needs the API initialized and SkyrimNet's memory system ready.

1. **No volumes yet** → fetch every entry (limit 10000) and create all volumes from 1.
2. **Volumes exist** → fetch entries after the latest volume's `endTime` (the API bound is inclusive, so entries `<= endTime` are dropped client-side).
   - **None:** if the latest volume was never saved (`persistedInSave = false`) and its `endTime` is later than the current game time, the range is left over from a quit-without-save. Unregister the actor and rebuild from scratch. Otherwise, nothing to do.
   - **The NPC no longer holds the latest volume** (pickpocketed, traded, lost): start new volumes at latest + 1 from the new entries. The old volume stays frozen.
   - **The NPC holds it:** fetch entries from the volume's `startTime` (up to `EntriesPerVolume + 1`).
     - At or over the limit → **seal**: re-render with exactly `EntriesPerVolume` entries, set `endTime` to the last included entry, and create overflow volumes from the rest.
     - Under the limit → re-render in place and move `endTime` forward.

Rendering writes the text to DiaryDB (`UpdateBookText`) and to `cachedBookText`, and updates `lastKnownEntryCount`.

"Sealed" means two things in this code. Here, a volume is sealed when it hits the entry limit. In `RefreshVolumeOnOpen`, a volume is sealed if a newer volume exists. In `QueueSealedVolumeRecovery`, it means `endTime > 0`, which is true of almost every volume, including the latest (see [KNOWN_ISSUES.md](KNOWN_ISSUES.md)).

---

## On load: recovery and catch-up

Both run at the end of the `kPostLoadGame` setup (see [ARCHITECTURE.md](ARCHITECTURE.md#startup-and-load-sequence)). They share a skip set so an actor already queued for recreation is not queued twice.

**`QueueSealedVolumeRecovery`**, per actor with volumes, looking at the latest volume:
- Not persisted and `endTime` later than now → queue a forced update (quit-without-save range).
- `endTime > 0` → ask SkyrimNet for one entry after `endTime + 0.001`. If one exists → queue `UpdateDiaryForActorInternal`. This catches entries written while SNPD was not listening: the KEEP choice on a revert, or entries created from the SkyrimNet dashboard while the game was paused.
- `endTime == 0` (legacy rows only) → compare the live entry count with `lastKnownEntryCount`.

**`QueueBatchCatchUpScan`** finds actors who have entries but no volumes (first install on an existing save, or after Reset):
1. Discovery: `RunDiscoveryBatch` asks for 50 entries across all actors (`formId = 0`) per game-thread task, paging backward by the oldest timestamp seen, and collects the distinct UUIDs. A page made up only of boundary duplicates ends the scan, which prevents an infinite loop.
2. One task per discovered actor without volumes: resolve UUID → FormID, fetch all entries, create all volumes.

Each is spread over game-thread ticks to avoid a load-time hitch.

---

## On open: `RefreshVolumeOnOpen`

Called by the book hook right before the text is injected. It asks SkyrimNet for the volume's live entries (bounded by `endTime` if a newer volume exists; open-ended for the latest volume), and re-renders if the count differs from `lastKnownEntryCount` or the cached text predates font tags. If entries were deleted, it also moves a sealed volume's `endTime` back to the new last entry.

---

## Save reverts: the KEEP / CLEAR fork

When the player loads an older save, **SkyrimNet asks whether to clear its history after that save point or keep it.** SNPD must handle both, and they fail in opposite directions. The rule:

> A diary entry that SkyrimNet no longer has must never keep appearing in a book, and an entry SkyrimNet still has must never be silently dropped from one.

This is why DiaryDB is keyed to the SkyrimNet save folder rather than stored in the `.ess`: SNPD's state deliberately outlives the save file, so it can **reconcile against** SkyrimNet instead of being reverted with it.

**KEEP (SkyrimNet kept entries SNPD never saw)** is handled eagerly at load by `QueueSealedVolumeRecovery`, above.

**CLEAR (SkyrimNet deleted entries SNPD already rendered)** is handled lazily in `RefreshVolumeOnOpen`, which re-renders and **moves the sealed `endTime` back**. That `endTime` move is essential: without it, `QueueSealedVolumeRecovery` would look past the deleted entry's timestamp on the next load and create a duplicate volume. The two are coupled; don't change one without the other. Lazy is fine for CLEAR (nothing is wrong until someone reads the book), but `lastKnownEntryCount` stays stale until that volume is opened.

Backwards time travel also clears stolen-volume records at load (see [THEFT.md](THEFT.md#save-reverts)).

**Known gap:** when a CLEAR deletes **every** entry of a volume, the book keeps its old text. `RefreshVolumeOnOpen` returns early when the live count is 0 and cached text exists (to protect imported or test books), which skips the re-render that would produce the "all entries removed" page. See [KNOWN_ISSUES.md](KNOWN_ISSUES.md).

---

## Which save am I in?

SNPD's per-save state lives in `…/SkyrimNetPhysicalDiaries/<saveFolder>/diary.db`. The folder name is `SkyrimNet-<id>`, where `<id>` is the value on the last `Using save ID: ` line in `SkyrimNet.log` (`DetectSaveFolderFromLog`). It is only accepted if SkyrimNet's own `SkyrimNet-<id>.db` exists. SkyrimNet's API needs no save parameter: it always serves the active save. So both SNPD's DB and SkyrimNet's answers depend on SkyrimNet's idea of the current save.

This detection runs at the start of `kPostLoadGame`, **before** the wait for SkyrimNet's memory system. If SkyrimNet has not yet logged the new save ID, SNPD opens the previous save's `diary.db`. See [KNOWN_ISSUES.md](KNOWN_ISSUES.md).
