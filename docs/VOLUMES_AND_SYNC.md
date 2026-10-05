# Volumes and Synchronization

How SkyrimNet's diary entries become volumes, how volumes stay in step with SkyrimNet when entries are added or deleted, and how save reverts are handled.

Code: `src/VolumeSync.cpp` (`CreateAllVolumesForActor`, `UpdateDiaryForActorInternal`, `QueueNewEntryRecovery`, `QueueBatchCatchUpScan` / `RunDiscoveryBatch`, `ReconcileWithTimeline`, `MigrateVolumesToIds`), `src/Database.cpp` (`GetEntriesById`, `GetDiaryEntries`), `src/BookManager.cpp` (`RefreshVolumeOnOpen`), `src/TimelineGate.cpp` (waiting for SkyrimNet's keep/clear decision).

---

## Entries

Volumes read SkyrimNet's entries with `Database::GetEntriesById(formId, afterId, upToId, limit, &ok, kind)`: SkyrimNet's `PublicQueryDiaryEntries` (public API v11, which SNPD requires) with `orderBy: IdAsc` and its inclusive `minId` / `maxId` set to `afterId + 1` / `upToId`, parsed into `DiaryEntry`, **in write order**. SkyrimNet's entry ids are `AUTOINCREMENT`: a new entry always gets a higher id, ids are never reused (not after a delete, not after a Clear), and an edit keeps its id. So id order is the order the entries were written, whatever their in-game dates: after a Keep a new entry can be dated before entries already written, but its id never comes before theirs. SkyrimNet's limit keeps the lowest ids in range. For the player the query also filters by tag inside SkyrimNet, before the limit: the diary leaves out `snpd_player_written`, a journal asks for it ([EDITING.md](EDITING.md#diaries-and-journals)).

| Field | Meaning |
|---|---|
| `actor_uuid`, `actor_name` | SkyrimNet identity. The UUID is deterministic per NPC and stable across saves (a rename makes a new one: [KNOWN_ISSUES.md](KNOWN_ISSUES.md) #8). |
| `entry_date` | In-game time, **seconds** since game start. Compare with `RE::Calendar::GetCurrentGameTime() * 86400.0`. Shown as the entry's date; not used to place it. |
| `creation_time` | Real-world write time. Not used. |
| `content` | The entry text. |
| `id` | SkyrimNet's entry id: the order of the entries and the bounds of a volume, and what the book editor saves and deletes by. |
| `tags` | SkyrimNet's tags. `snpd_player_written` marks an entry the player wrote in their journal (and `snpd_written_in_blood` one with text written in blood, see [EDITING.md](EDITING.md#writing-in-blood)); its first line is then kept as written (see [BOOK_TEXT.md](BOOK_TEXT.md#cleaning-llm-output-sanitizebooktext)). SkyrimNet's other fields, such as `location`, aren't read. |

`Database::GetDiaryEntries(formId, limit, startTime, endTime, &ok, kind)` (SkyrimNet's `PublicGetDiaryEntries`: an `entry_date` range, its limit keeping the newest, the result sorted oldest first by `EntryOlder`) is kept for the questions that are about dates: the catch-up scan's discovery across all actors, the editor's journal numbering, and NPC diaries' "wrote today" and "already written up" tests.

SkyrimNet merges an actor's **co-identities** into every diary read: former selves after a rename and identity links ([KNOWN_ISSUES.md](KNOWN_ISSUES.md) #8 and #9). If that set grows, entries with ids inside a finished volume's range can appear; the volume shows them the next time it's opened.

**Same-named generic NPCs share one diary.** SkyrimNet groups memory by actor name, so every "Whiterun Guard" is one identity and one set of volumes. That is upstream behaviour. The documented workaround is a unique-names mod.

---

## Volumes

A diary volume holds one actor's entries with ids in `(afterId, lastId]` (`DiaryBookData` / DiaryDB `volumes`):

| Field | Meaning |
|---|---|
| `afterId` | The previous volume's `lastId` (0 for volume 1) |
| `lastId` | Its last entry at its last update or seal. -1: a row from before 2026-10-01, not migrated yet ([DATABASE.md](DATABASE.md#schema-changes)) |
| `latestDate` | The latest `entry_date` it shows (DiaryDB column `end_time`), for `ReconcileWithTimeline` |
| `lastKnownEntryCount` | Entry count at the last render |

`BookManager::GetLiveEntries` reads a volume: the ids after `afterId`, up to `lastId` once a newer volume exists (and for the player's diary with `[Diary] PlayerDiaryBooks` off). **The latest volume is open-ended**, so an entry written since its last update shows as soon as the book is opened. Volumes are runs of consecutive ids (of that actor's entries), so a volume never depends on the next one, and a deletion needs no bookkeeping: `lastId` stays a valid bound when that entry is gone, and the volume shows one page fewer. **A volume's size comes only from its ids**, never from the current `EntriesPerVolume`, which only decides where the latest volume is sealed next.

Entries are shown in write order, so after a Keep the dates run backwards once, at the rewind: the honest record of a rewound timeline. The title page shows the earliest and latest date (`TitlePageDates`), not the first and last page's.

`CreateAllVolumesForActor(uuid, name, formId, bioTemplate, entries, startingVolume, afterId)` cuts the entries (in id order) into chunks of `EntriesPerVolume` and calls `CreateDiaryBook` for each; a chunk's `afterId` is the previous chunk's last id.

**Empty volumes** (`BookManager::CreateEmptyVolume`) are only the player's journals, made by the first save in a blank journal. Journals aren't split by time: each holds the entries tagged for it (`snpd_journal_<n>`), side by side, so their entry-id bounds (`afterId`, `lastId`) mean nothing, and they grow only in the book editor ([EDITING.md](EDITING.md#diaries-and-journals)). Nothing on this page seals or extends them; `ReconcileWithTimeline` only re-renders a journal whose entry count changed, and `RestoreLostJournals` makes one again after a Keep when the save has no book for it.

---

## When an entry arrives: `UpdateDiaryForActorInternal`

Called from the `UpdateDiaryFromEvent` native (event listener), from load-time recovery, and from the catch-up scan. It needs the API initialized and SkyrimNet's memory system ready.

It waits rather than runs **from `kPreLoadGame` until the post-load sync has run**, re-running itself 500 ms later on the game thread (`DeferUntilSyncReady`) (`SetPostLoadSyncReady`). DiaryDB isn't loaded yet, so every actor would look new and get a duplicate volume 1. The event native waits too, so the theft clear doesn't land in the previous save's DB. A wait from an earlier load is dropped when a new load starts; that load's recovery and catch-up scans pick the entry up. Book creation is synchronous, so a volume is in `books_` as soon as it is created and there is nothing else to wait for.

1. **No volumes yet** → fetch every entry and create all volumes from 1.
2. **Volumes exist** (and migrated: a `lastId` of -1 gets no update until the migration has run) → fetch the entries after the latest volume's `lastId`: written since its last update, whatever their dates. None → nothing to do.
   - **The NPC no longer holds the latest volume** (pickpocketed, traded, lost): it keeps `(afterId, lastId]`, and the new entries start volumes at latest + 1.
   - **The NPC holds it:** fetch every entry after its `afterId`.
     - Over the limit → **seal**: re-render with the first `EntriesPerVolume`, set `lastId` to the last of them, and create volumes from the rest after it.
     - Otherwise → re-render in place and set `lastId` to the last entry.

Rendering writes the text to DiaryDB (`UpdateBookText`) and to `cachedBookText`, and updates `lastKnownEntryCount` and, for a diary, `latestDate`.

**Sealed** means only this: the volume hit the entry limit and later entries went into a new volume. Elsewhere the code says what it checks: an **earlier volume** is any volume with a newer one after it (`GetLiveEntries` ends it at `lastId`), and the **latest volume** is the one new entries go into.

Until 2026-10-01 volumes were split by `entry_date`, with tie-break data for entries sharing a date (`prev_volume_count_at_boundary`, the "entry #10 missing" bug) and an `endTime` that had to move back on deletions. After a Keep a new entry could be dated inside a finished volume: it was added in the middle of that book, or dropped if the NPC no longer held the latest volume.

---

## On load: recovery and catch-up

Both run at the end of the `kPostLoadGame` setup (see [ARCHITECTURE.md](ARCHITECTURE.md#startup-and-load-sequence)). They share a skip set: recovery skips actors already queued for recreation and adds every actor it queues, so the catch-up scan never also creates volumes for them. The catch-up scan additionally skips actors that got volumes while it ran.

**`QueueNewEntryRecovery`**, per actor with volumes, looking at the latest volume:
- Ask SkyrimNet for one entry after the latest volume's `lastId`. If there is one → queue `UpdateDiaryForActorInternal`. This catches entries written while SNPD was not listening: the KEEP choice on a revert, or entries created from the SkyrimNet dashboard while the game was paused.

**`QueueBatchCatchUpScan`** finds actors who have entries but no volumes (first install on an existing save, or after Reset):
1. Discovery: `RunDiscoveryBatch` asks for 50 entries across all actors (`formId = 0`) per game-thread task, paging backward by the oldest timestamp seen, and collects the distinct UUIDs. A page made up only of boundary duplicates ends the scan, which prevents an infinite loop.
2. One task per discovered actor without volumes: resolve UUID → FormID, fetch all entries (`GetEntriesById`), create all volumes.

Each is spread over game-thread ticks to avoid a load-time hitch. The scan remembers which load it belongs to; if another load or a new game starts while it is still paging, the rest of it is dropped (its `books_` view would be the new session's).

---

## On open: `RefreshVolumeOnOpen`

Called from the `OpenBookMenu` hook (`RefreshBeforeOpen`) on every open, the engine's or another mod's, right before the menu gets its text ([BOOK_TEXT.md](BOOK_TEXT.md#delivery-the-getdescription-and-openbookmenu-hooks)); from the no-parent `GetDescription` branch only if that hook failed to install. It asks SkyrimNet for the volume's live entries through `GetLiveEntries` (up to `lastId` for an earlier volume or a frozen player diary; open-ended for the latest volume), renders them, and stores the result if it differs from the cached text. Comparing the text rather than the count catches an entry's text edited outside SNPD (SkyrimNet's dashboard, another mod), as well as changed fonts, names or text from before font tags. Rendering is pure string work, small next to the query. A volume with the player's edits still being written to SkyrimNet (`BookEditor::HasPendingWrites`) is skipped: SkyrimNet still has the old text, and the last write re-renders it (`ReconcileAfterWrite`). `FormatDiaryEntries` renders exactly the entries it is given, so a newer entry the latest volume hasn't absorbed yet is shown (and counted) until the next update seals or extends the volume.

---

## Save reverts: the KEEP / CLEAR fork

When the player loads an older save, **SkyrimNet asks whether to clear its history after that save point or keep it.** SNPD must handle both, and they fail in opposite directions. The rule:

> A diary entry that SkyrimNet no longer has must never keep appearing in a book, and an entry SkyrimNet still has must never be silently dropped from one.

This is why DiaryDB is keyed to the SkyrimNet save folder rather than stored in the `.ess`: SNPD's state deliberately outlives the save file, so it can **reconcile against** SkyrimNet instead of being reverted with it.

### Waiting for the decision: `TimelineGate`

SkyrimNet asks the question from its own `kPostLoadGame` work, and its database reports ready well before the player answers. Syncing at that point builds books from "future" entries that a CLEAR then deletes, and the book left behind opens blank. So the post-load sync also waits for the timeline to settle.

`TimelineGate::IsSettled` polls `PublicGetTimelineState` (`Database::GetTimelineState`). SkyrimNet marks its check pending at `kPreLoadGame` and keeps it pending while the prompt is open and while a Clear is still deleting, so the sync starts as soon as it reads none (0), kept (2) or cleared (3). There is no time limit while it's pending. The state also gives the outcome logged by `ReconcileWithTimeline`. SNPD doesn't listen for the `SkyrimNet_TimelineResolved` ModEvent: the post-load sync already polls every 100 ms.

If the prompt never appears, SkyrimNet gives up after 120 s and keeps its history (Keep), and the sync starts then. Until 2026-10-01 SNPD watched the prompt itself, through a hook on `MessageBoxData::QueueMessage`.

### Reconciling: `ReconcileWithTimeline`

Right after `LoadFromDB`, once the timeline is settled, every diary volume showing an entry dated after the loaded save's game time (`latestDate`) is checked against the entries SkyrimNet still has (by fetching its range again; the outcome is only logged). This follows SkyrimNet's decision exactly, whether or not the volume was ever saved:

- **Clear** deleted those entries: the volume is re-rendered (its bounds stay). A Clear deletes entries written after the save, so they are the newest ids: a trailing run of volumes with **no** entries left is dropped from `books_` and DiaryDB, and their books are retired and swept: taken from the NPC, the loaded cells and merchant chests, and from other cells as they load (see [BOOK_FORMS.md](BOOK_FORMS.md#retirement)).
- **Keep** left them in place: counts match, nothing changes. Entries written after the Keep get higher ids and go into the latest volume, whatever their dates.
- **A volume newer than the loaded save** (an in-session revert, then Keep) has no book in this save, so `LoadFromDB` has already deleted its row and queued the actor: the volume is recreated from the entries SkyrimNet kept.
- **A failed query** (`ok = false`) is neither: the actor's volumes are left alone and a warning is logged, so a SkyrimNet error at load can't delete books.

### KEEP and CLEAR outside a load

**KEEP (SkyrimNet kept entries SNPD never saw)** is also handled eagerly at load by `QueueNewEntryRecovery`, above.

**Deletions SNPD makes** (tearing out an entry in the book editor) call `BookManager::ReconcileAfterWrite` once SkyrimNet has finished the volume's pending writes: a re-render from SkyrimNet. Deleting an entry leaves every other entry in its volume: the bounds are ids.

**Deletions SNPD didn't see happen** (for example from the SkyrimNet dashboard) show the next time the volume is opened: `RefreshVolumeOnOpen` re-renders it. Nothing else changes: the bounds are ids, so the load-time recovery can't mistake a gap for new entries.

Backwards time travel also clears stolen-volume records at load (see [THEFT.md](THEFT.md#save-reverts)).

**Every entry deleted outside a load** (for example from the SkyrimNet dashboard): the next open renders the "all entries removed" page (`EmptyVolumeText`, marked with `kEmptySentinel`), and the inter-plugin API reports `NoEntries`. `RefreshVolumeOnOpen` only does this when the query succeeded: `GetEntriesById` reports a failed query (`ok = false`: SkyrimNet unavailable, an exception, an empty or non-array response) separately, and then the cached text is kept and a warning logged. (Until 2026-09-27 zero live entries always kept the old text, a guard for test/imported books that no longer exist.) SkyrimNet's own export answers `[]` on an internal error, which still looks like zero entries.

---

## Which save am I in?

SNPD's per-save state lives in `…/PhysicalDiaries/<saveFolder>/diary.db`. The folder name is `SkyrimNet-<id>`, where `<id>` is the value on the last `Using save ID: ` line in `SkyrimNet.log` (`DetectSaveFolderFromLog`). It is only accepted if SkyrimNet's own `SkyrimNet-<id>.db` exists. SkyrimNet's API needs no save parameter: it always serves the active save. So both SNPD's DB and SkyrimNet's answers depend on SkyrimNet's idea of the current save.

This detection runs in the post-load sync, after the waits for SkyrimNet's database and timeline decision, by which time SkyrimNet has logged the new save ID.
