# Database and Persistence

SNPD keeps its state in a per-save SQLite database (DiaryDB). The SKSE co-save holds one record: what each diary book form in the save is. **DiaryDB is the source of truth**, but only for SNPD's own data (volume boundaries, rendered text, theft records); which form is a volume's book in a given save comes from that save's co-save record. Diary content always comes from SkyrimNet.

Code: `src/DiaryDB.cpp`, `include/DiaryDB.h` (singleton `SkyrimNetDiaries::DiaryDB`); co-save callbacks in `src/Serialization.cpp`; save-folder detection in `src/SaveFolder.cpp`.

---

## Location and lifetime

`<game>/Data/SKSE/Plugins/SkyrimNetPhysicalDiaries/<saveFolder>/diary.db` (MO2: under `overwrite/`). `<saveFolder>` is SkyrimNet's save folder, `SkyrimNet-<id>`, found by `DetectSaveFolderFromLog` (see [VOLUMES_AND_SYNC.md](VOLUMES_AND_SYNC.md#which-save-am-i-in)).

- Closed at `kPreLoadGame` and `kNewGame`, so nothing written during a load or a new game lands in the previous save's DB. Opened by the post-load sync, or in `SaveCallback` on a new game's first save. `SaveCallback` doesn't open it during a load's post-load wait, when `SkyrimNet.log` may still name the previous save; if the sync can't open it, diary books are paused for that session (see [ARCHITECTURE.md](ARCHITECTURE.md#startup-and-load-sequence)). The folder name must be `SkyrimNet-` followed by digits and dashes; anything else is refused. `Open()` with the folder that is already open does nothing; a different folder closes the old one first.
- WAL journal, `synchronous=NORMAL`.
- **Keyed to the SkyrimNet save folder, not the `.ess`.** Loading an older save of the same character reopens the same DB, which has not been rolled back. That is deliberate: SNPD reconciles against SkyrimNet instead of reverting with the save. See [VOLUMES_AND_SYNC.md](VOLUMES_AND_SYNC.md#save-reverts-the-keep--clear-fork).
- `RevertCallback` (new game or load) clears memory only. The file stays.

## Schema

Created in `EnsureSchema()`.

**`volumes`**, one row per volume, `PRIMARY KEY (actor_uuid, kind, volume_number)`:

| Column | Notes |
|---|---|
| `actor_uuid`, `actor_name` | SkyrimNet identity |
| `actor_form_id` | Actor FormID when the volume was made. Used only as a fallback, and only after a UUID back-check (see [BOOK_FORMS.md](BOOK_FORMS.md#finding-the-npc-findactorforbook)). |
| `book_form_id` | The volume's book form (an `0xFF` runtime FormID). On every load it is matched against the save's co-save record: updated if the save has another form for the volume, the row deleted and recreated if the save has none (see [BOOK_FORMS.md](BOOK_FORMS.md#load)). |
| `kind` | 0: a diary (SkyrimNet's entries; every NPC volume). 1: the player's journal (what they wrote in the book editor). Each kind numbers its volumes from 1. See [EDITING.md](EDITING.md#diaries-and-journals). |
| `volume_number`, `after_id`, `last_id` | The volume's SkyrimNet entry ids, `(after_id, last_id]`; `last_id` -1: a diary not migrated yet (journals ignore their bounds and may keep -1) (see [VOLUMES_AND_SYNC.md](VOLUMES_AND_SYNC.md#volumes)) |
| `end_time` | The latest `entry_date` the volume shows (`latestDate`) |
| `start_time`, `prev_volume_last_creation_time`, `prev_volume_count_at_boundary` | Unused since 2026-10-01 (the date boundaries); left in place |
| `journal_template`, `bio_template_name` | Template EditorID; SkyrimNet bio template name (`player_special` for the player) |
| `last_known_entry_count` | Entry count at the last render |
| `book_text` | Rendered text. A cache: it can always be rebuilt from SkyrimNet. `UpsertVolume` with empty text keeps the stored text. |

**`actor_templates`**: `actor_uuid` (PK), `template_name`. The `last_known_game_time` column is no longer read or written (theft reverts use `stolen_at`, see [THEFT.md](THEFT.md#save-reverts)); it stays so older databases open unchanged.

**`stolen_volumes`**: `(actor_uuid, volume_number)` PK, `stolen_at` (game seconds).

**`blood`**: `entry_id` (SkyrimNet's entry id, PK), `ranges` (the byte ranges of the entry's content written in blood, `"s:e,s:e"`), `content_hash` (FNV-1a 64 of the content they belong to; a mismatch drops the red from the text), `heading` (1: the entry was begun in blood, its heading is red). Only the player's journal entries have rows. See [EDITING.md](EDITING.md#writing-in-blood).

Databases from before 2.0.0 also had a `persisted_in_save` column in `volumes` (it existed because DPF forms survived a reload without saving); the `kind` migration dropped it.

## Schema changes

There is no version table. New columns are added in `EnsureSchema()` with `ALTER TABLE … ADD COLUMN … DEFAULT …`, and the "duplicate column" error on databases that already have them is ignored (`actor_form_id` was added this way). Existing rows are not rewritten. Code must cope with the default value (for example `actor_form_id = 0` → resolve from the UUID). Follow the same pattern for new columns.

Two exceptions so far. First, `kind` joined the primary key (2026-09-29), and SQLite can't change a key. When `volumes` has no `kind` column (`HasColumn`), `EnsureSchema` rebuilds it once in a transaction: a new table, the rows copied (every one kind 0, a diary), the old table dropped, the new one renamed. If that fails it rolls back and the DB doesn't open. It is one-way (an older SNPD fails every volume write on the new key), so the DB is first copied to `diary.db.pre-kind` (`VACUUM INTO`, `DiaryDB::Backup`); to go back to 2.0.x, restore that copy.

**Entry ids (2026-10-01):** diary volumes became runs of SkyrimNet entry ids instead of date ranges. `EnsureSchema` adds `after_id` and `last_id` (default -1) after copying the DB to `diary.db.pre-ids`. The rows are re-cut in the post-load sync, which can read SkyrimNet (`MigrateVolumesToIds`, right after `LoadFromDB`): per actor, every entry in id order, each volume taking its `last_known_entry_count` in turn; the latest volume's rest arrives through the load-time recovery as new entries. On a playthrough without a Keep every volume keeps the same entries; after a Keep some move to a neighbouring volume, once. An actor SkyrimNet can't be read for stays at -1 (its volumes keep their cached text and get no updates) and is tried again next load. Older SNPD builds write no `last_id`; restore `diary.db.pre-ids` to go back. If adding the columns fails, the DB doesn't open.

## Co-save records

Under the unique ID `'SNDB'`:

- **`SNBF`** (version 2, since 2.0.0): every tracked book form, retired ones included, written by `DynamicForms::Save` at the start of `SaveCallback`. Per form: FormID (`uint32`), form type (`uint8`), flags (`uint8`, bit 0 = retired), then three strings (`uint32` length + bytes): key (`"<actor UUID>|v<volume>"`, or `|j<volume>` for a journal), template EditorID, display name. The save itself keeps only a form's flags, so the load callback uses this to fill each book in (see [BOOK_FORMS.md](BOOK_FORMS.md#the-co-save-record)).
- **`SNND`** (version 4): NPC diaries' state ([NPC_DIARIES.md](NPC_DIARIES.md)): the game day they last ran (`int32`, -1 = never), so a reload doesn't run the same day twice, then the daily writers (`uint32` count, then FormIDs, resolved with `ResolveFormID` on load). Version 2 had the same layout (the old dialogue's writers, still loaded); versions 1 and 3 only the day.
- `SaveCallback` also opens DiaryDB on a new game's first save and flushes the volumes; `RevertCallback` clears the in-memory volumes and the tracked forms.

The load callback reads only `SNBF` and `SNND`, so the records older saves carry are skipped:

| Record | Was | Retired |
|---|---|---|
| `SNDB` | Two zero `uint32`s, a sentinel from when volumes lived in the co-save | 2026-09-27 |
| `SNDF` | Save-folder name; ignored on load for some time, since the folder is always detected from `SkyrimNet.log` | 2026-09-27 |
| `SNDC` | FormID → UUID cache, read only by a log-only event sink | 2026-09-26 |

A save made with 2.0.0 can't go back to an older SNPD: its books are `0xFF` forms the older version doesn't know, so they load as empty shells.

## MCM Reset (`ResetAllDiariesInternal`)

Deletes every tracked actor from DiaryDB (`DeleteActor`: their `volumes` and `actor_templates` rows; `ClearAllStolenVolumes`) before it clears memory (the other way round, the next load reads the rows back, the catch-up scan sees books, and Reset seems to stop working after one run), **retires** every diary's book form, then on the game thread removes their copies from the loaded cells, their owner NPCs and merchant chests (`SweepRetiredBooks`); copies elsewhere are removed as their cells load. The forms themselves are never removed from the save (see [BOOK_FORMS.md](BOOK_FORMS.md#retirement)). The catch-up scan rebuilds the diaries on the next load (the player's only with `[Diary] PlayerDiaryBooks` on). The player's journals are **kept** as they were: their books aren't retired, and their rows go back into DiaryDB after the wipe (`RegisterBook`), so the same books keep their looks and entries wherever they are. SkyrimNet's entries are never touched.

## Inspecting a live database

With the game closed, or read-only while it runs:

```powershell
sqlite3 "<MO2>\overwrite\SKSE\Plugins\SkyrimNetPhysicalDiaries\SkyrimNet-<id>\diary.db"
```
```sql
SELECT actor_name, kind, volume_number, book_form_id, after_id, last_id, end_time,
       last_known_entry_count
FROM volumes ORDER BY actor_name, kind, volume_number;

SELECT * FROM stolen_volumes;
```

The current `<id>` is on the last `Using save ID:` line of `SkyrimNet.log`.
