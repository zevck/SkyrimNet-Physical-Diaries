# Database and Persistence

SNPD keeps its state in a per-save SQLite database (DiaryDB). The SKSE co-save holds only a sentinel and the save-folder name. **DiaryDB is the source of truth**, but only for SNPD's own data (volume boundaries, book FormIDs, rendered text, theft records). Diary content always comes from SkyrimNet.

Code: `src/DiaryDB.cpp`, `include/DiaryDB.h` (singleton `SkyrimNetDiaries::DiaryDB`); co-save callbacks in `src/Serialization.cpp`; save-folder detection in `src/SaveFolder.cpp`.

---

## Location and lifetime

`<game>/Data/SKSE/Plugins/SkyrimNetPhysicalDiaries/<saveFolder>/diary.db` (MO2: under `overwrite/`). `<saveFolder>` is SkyrimNet's save folder, `SkyrimNet-<id>`, found by `DetectSaveFolderFromLog` (see [VOLUMES_AND_SYNC.md](VOLUMES_AND_SYNC.md#which-save-am-i-in)).

- Closed at `kPreLoadGame`, so nothing written during a load lands in the previous save's DB. Opened by the post-load sync, or in `SaveCallback` if a save happens before it (a new game that has had no load, or a save during the load wait). The folder name must be `SkyrimNet-` followed by digits and dashes; anything else is refused. `Open()` with the folder that is already open does nothing; a different folder closes the old one first.
- WAL journal, `synchronous=NORMAL`.
- **Keyed to the SkyrimNet save folder, not the `.ess`.** Loading an older save of the same character reopens the same DB, which has not been rolled back. That is deliberate: SNPD reconciles against SkyrimNet instead of reverting with the save. See [VOLUMES_AND_SYNC.md](VOLUMES_AND_SYNC.md#save-reverts-the-keep--clear-fork).
- `RevertCallback` (new game or load) clears memory only. The file stays.

## Schema

Created in `EnsureSchema()`.

**`volumes`**, one row per volume, `PRIMARY KEY (actor_uuid, volume_number)`:

| Column | Notes |
|---|---|
| `actor_uuid`, `actor_name` | SkyrimNet identity |
| `actor_form_id` | Actor FormID when the volume was made. Used only as a fallback, and only after a UUID back-check (see [BOOK_FORMS.md](BOOK_FORMS.md#finding-the-npc-findactorforbook)). |
| `book_form_id` | The DPF book form. Checked on every load; a row whose form no longer resolves is deleted and recreated. |
| `volume_number`, `start_time`, `end_time`, `prev_volume_last_creation_time`, `prev_volume_count_at_boundary` | Boundaries (see [VOLUMES_AND_SYNC.md](VOLUMES_AND_SYNC.md#volume-boundaries)) |
| `journal_template`, `bio_template_name` | Template EditorID; SkyrimNet bio template name (`player_special` for the player) |
| `last_known_entry_count` | Entry count at the last render |
| `book_text` | Rendered text. A cache: it can always be rebuilt from SkyrimNet. `UpsertVolume` with empty text keeps the stored text. |
| `persisted_in_save` | 1 once a save has included the volume (`MarkAllVolumesPersisted` at `kSaveGame`, and `UpsertVolume` from the in-memory flag, which never lowers it). The upsert path matters on a new game's first save, where the DB is only opened in `SaveCallback` after `kSaveGame` ran. |

**`actor_templates`**: `actor_uuid` (PK), `template_name`, `last_known_game_time` (for detecting loads of earlier saves; see [THEFT.md](THEFT.md#save-reverts)).

**`stolen_volumes`**: `(actor_uuid, volume_number)` PK, `stolen_at` (game seconds).

## Schema changes

There is no version table. New columns are added in `EnsureSchema()` with `ALTER TABLE … ADD COLUMN … DEFAULT …`, and the "duplicate column" error on databases that already have them is ignored (`persisted_in_save` and `actor_form_id` were added this way). Existing rows are not rewritten. Code must cope with the default value (for example `actor_form_id = 0` → resolve from the UUID). Follow the same pattern for new columns.

## Co-save records

Unique ID `'SNDB'`, record version 3.

| Record | Contents | Status |
|---|---|---|
| `SNDB` | Two zero `uint32`s. `BookManager::Save`/`Load` write and ignore it. | Sentinel from when volumes lived in the co-save |
| `SNDF` | Save-folder name | Written for compatibility, ignored on load: the folder is always detected from `SkyrimNet.log` |
| `SNDC` | FormID → UUID cache | **Retired 2026-09-26.** Its only reader was a log-only event sink. Older saves still have the record; `LoadCallback` has no branch for it and SKSE skips unread records. |

In short, the co-save does no real work any more. Removing it changes the save format, so it is left for a deliberate change (see [KNOWN_ISSUES.md](KNOWN_ISSUES.md)).

## MCM Reset (`ResetAllDiariesInternal`)

Deletes every tracked actor from DiaryDB (`DeleteActor` + `ClearAllStolenVolumes`), clears memory, then on the game thread removes each book from every reference in the loaded cells (`TES::ForEachReference`). Books in unloaded cells are not reached. **DPF forms are deliberately not disposed** (see [BOOK_FORMS.md](BOOK_FORMS.md#the-two-dpf-bugs-this-pipeline-works-around)). The catch-up scan rebuilds every book on the next load. SkyrimNet's entries are never touched.

## Inspecting a live database

With the game closed, or read-only while it runs:

```powershell
sqlite3 "<MO2>\overwrite\SKSE\Plugins\SkyrimNetPhysicalDiaries\SkyrimNet-<id>\diary.db"
```
```sql
SELECT actor_name, volume_number, book_form_id, start_time, end_time,
       last_known_entry_count, persisted_in_save
FROM volumes ORDER BY actor_name, volume_number;

SELECT * FROM stolen_volumes;
```

The current `<id>` is on the last `Using save ID:` line of `SkyrimNet.log`.
