# Settings and the MCM

Code: `include/Config.h` (header-only singleton `SkyrimNetDiaries::Config`), `Source/Scripts/PhysicalDiaries_MCM.psc`, the `MCM_*` natives in `src/PapyrusAPI.cpp`.

---

## The INI

`Data/SKSE/Plugins/PhysicalDiaries.ini`. Loaded once in `SKSEPlugin_Load` and **written straight back** (`Config::Save()`), so MO2 copies it to `overwrite/` and user settings survive mod updates. MCM setters also save immediately.

| Key | Default | Range | Effect |
|---|---|---|---|
| `[General] Language` | *(unset)* | locale name | Forces a locale file; see [LOCALIZATION.md](LOCALIZATION.md) |
| `[General] DebugLog` | 0 | 0/1 | Log level `debug` instead of `info`. Takes effect live from the MCM. |
| `[Diary] ShowDateHeaders` | 1 | 0/1 | SNPD's date header per entry. Also controls whether LLM-written dates are stripped (see [BOOK_TEXT.md](BOOK_TEXT.md#cleaning-llm-output-sanitizebooktext)). |
| `[Diary] PlayerDiaryBooks` | -1 | -1/0/1 | Books of the diary SkyrimNet writes for the player, as for NPCs. -1 (the default): off with writing installed ([EDITING.md](EDITING.md#writing-mode)), on without; the MCM toggle sets 0 or 1, and its default button -1. Off: the player's diary stops at its last entry (SkyrimNet still writes the entries); turning it on catches the diary up. The player's journal isn't affected. See [EDITING.md](EDITING.md#diaries-and-journals). |
| `[Diary] EntriesPerVolume` | 10 | 1–50 | Chunk size for **new** diary volumes, and when the latest diary volume is sealed. Also how many entries a journal of the player's holds: a full one takes no new entry ([EDITING.md](EDITING.md#diaries-and-journals)). Existing volumes keep their size: their range comes from their stored entry ids, `(afterId, lastId]`, not this setting. |
| `[Diary] DeleteKey` | 68 (F10) | 1–255 | DirectX scan code of the key that tears out the entry under the caret while the player writes in a journal, after a question. Registered with Ink & Quill together with the new-entry key (`RegisterKeys`, the whole set each time), at load and when the MCM changes either, or Ink & Quill would keep it from the game while writing. Ink & Quill refuses keys that type or edit and its own edit key: the MCM then says so (`$SNPD_KeyNotWhileWriting`; the key is still saved), and the load logs it. Not a key that types. See [EDITING.md](EDITING.md#tearing-out-an-entry). |
| `[Diary] NewEntryKey` | 0 (unbound) | 0–255 | DirectX scan code of the key that starts a new entry at the end of the open journal of the player's, if it has room; it does nothing outside the book. It never makes a journal: reading a blank journal does. Not a key that types. The key to write is Ink & Quill's (`InkAndQuill.ini`; until 2026-10-02 SNPD's `[Diary] EditKey`, which the next save of the INI drops). Registered with Ink & Quill like `DeleteKey`. See [EDITING.md](EDITING.md#new-entries). |
| `[NpcDiaries] Enabled` | 0 | 0–1 | NPCs write diary entries on their own once a game day ([NPC_DIARIES.md](NPC_DIARIES.md)) |
| `[NpcDiaries] DailyRandom` | 3 | 0–20 | Weighted random picks a day, from actors with recent activity |
| `[NpcDiaries] RunHour` | 22 | 12–23 | The game hour the day's diaries run; a sleep, wait, fast travel or carriage that passes it runs them then |
| `[NpcDiaries] CloseBoost` | 1 | 0–1 | Followers and the spouse weigh double in the random picks |
| `[NpcDiaries] DailyWriters` | 1 | 0–1 | The save's daily writers write every day; off keeps the list ([NPC_DIARIES.md](NPC_DIARIES.md#daily-writers)) |
| `[Fonts] TitleSize` / `DateSize` / `ContentSize` / `SmallSize` | 18 / 16 / 14 / 12 | 8–24 | Sizes in the rendered markup |
| `[Fonts] FontFace` | `$HandwrittenFont` | font name | `face=` in the rendered markup |

**Every integer setting is one row in `Config::kIntSettings`**: section, key, default and range. Getters and setters clamp to that range, so a hand-edited INI can't feed out-of-range values to the code (`EntriesPerVolume = 0` used to loop forever), and a non-numeric value falls back to the default. `Save()` and the startup log walk the same table, so the defaults can't drift apart. `Save()` builds the whole file in memory before writing it, so a failure can't leave it half-written.

**`Save()` writes only known keys.** A key that isn't in `kIntSettings` (or the two string settings, `Language` and `FontFace`) is **dropped from the file at the next startup**.

## The MCM (`PhysicalDiaries_MCM`, `extends SKI_ConfigBase`)

Listed as **Physical Diaries** ("SkyrimNet Physical Diaries" before 2.0.0). SkyUI keeps a menu's name from its first registration, so a save that already had the menu keeps the old name in the list. Settings and Maintenance have two columns (`SetCursorPosition(1)` starts the right one), Daily Writers fills left to right. `OnConfigOpen` sets the page list again (`SetPages`), since `OnConfigInit` runs once per save. Settings has volumes and fonts on the left, writing and NPC diaries on the right; Maintenance has reset on the left, logging on the right.

| Page | Options |
|---|---|
| `$SNPD_PageSettings` | Entries per volume (slider); show date headers (toggle); Your Diary Books (toggle; its default button sets -1, by writing mode: Ink & Quill); font face (menu); four font-size sliders; under `$SNPD_HeaderWriting`: the tear-out and new-entry keys (key maps; disabled while writing is off, `IsWritingOn`); under `$SNPD_HeaderNpcDiaries`: NPCs write diaries (toggle), random writers per day (slider), writing hour (slider, shown `{0}:00`), favor followers and spouse (toggle); under `$SNPD_HeaderSkyrimNet`: two of **SkyrimNet's own** settings, Diary Generation (its `Diary` config `enabled`) and Diary Day Boundary (`respect_day_boundary`). Those two aren't in SNPD's INI: they show the value read through SkyrimNet's Papyrus `GetConfigBool` when the MCM opens (`RefreshSkyrimNetSettings` in `OnConfigOpen`; the answers normally arrive before the page is drawn) and are changed through its `PatchConfig`, which applies and saves SkyrimNet's config file (`NpcDiaries::Get/SetSkyrimNetDiarySetting`, behind the keys `SkyrimNetDiaries` and `SkyrimNetDayBoundary` of the same natives). The NPC Diaries options go through two generic natives, `GetNpcSetting` / `SetNpcSetting(key)`, which look the key up among the `[NpcDiaries]` entries of `kIntSettings`: a new `[NpcDiaries]` key needs no new native pair. |
| `$SNPD_PageDailyWriters` | Write Every Day (toggle, `[NpcDiaries] DailyWriters`) and Add Targeted Actor; under a rule, the save's daily writers two to a row, each asking to be removed when selected ([NPC_DIARIES.md](NPC_DIARIES.md#daily-writers)) |
| `$SNPD_PageMaintenance` | Reset All Diaries (confirm → `ResetAllDiaries`); debug logging (toggle) |

Rebuilding the book text:
- Toggling date headers → `RegenerateTextsOnly()` at once.
- Font face or size changes → one `RegenerateTextsOnly()` when the MCM closes (`OnConfigClose`), and only if something changed.
- `RegenerateTextsOnly` → `BookManager::RegenerateAllDiaryTexts`, which re-renders every tracked volume from SkyrimNet. Forms and volume bounds are untouched; a volume whose query fails keeps its text.

Reset: see [DATABASE.md](DATABASE.md#mcm-reset-resetalldiariesinternal).

Strings are `$SNPD_…` keys translated in `Interface/Translations/` (UTF-16 LE with BOM; see [LOCALIZATION.md](LOCALIZATION.md#mcm-translations)).

## Adding a setting end to end

1. `Config.h`: a row in `kIntSettings` (section, key, default, range) plus a getter and setter that call `Get`/`Set` with it. String settings need their own line in `Save()`.
2. `PapyrusAPI.cpp`: `MCM_Get…` / `MCM_Set…` (the setter calls `Save()`), registered on `PhysicalDiaries_MCM`.
3. `PhysicalDiaries_MCM.psc`: `Native` declarations, the option, handlers, and a regenerate call if it changes rendering.
4. Add `$SNPD_…` keys to **all nine** translation files.
5. Compile the `.pex` and ship both (see [PAPYRUS_AND_API.md](PAPYRUS_AND_API.md#adding-or-changing-a-native-all-four-steps-every-time)).
6. Update the README's MCM section. (No INI ships with the mod; `Config::Save()` writes one on first launch.)
