# Settings and the MCM

Code: `include/Config.h` (header-only singleton `SkyrimNetDiaries::Config`), `Source/Scripts/SkyrimNetDiaries_MCM.psc`, the `MCM_*` natives in `src/PapyrusAPI.cpp`.

---

## The INI

`Data/SKSE/Plugins/SkyrimNetPhysicalDiaries.ini`. Loaded once in `SKSEPlugin_Load` and **written straight back** (`Config::Save()`), so MO2 copies it to `overwrite/` and user settings survive mod updates. MCM setters also save immediately.

| Key | Default | Range | Effect |
|---|---|---|---|
| `[General] Language` | *(unset)* | locale name | Forces a locale file; see [LOCALIZATION.md](LOCALIZATION.md) |
| `[General] DebugLog` | 0 | 0/1 | Log level `debug` instead of `info`. Takes effect live from the MCM. |
| `[Diary] ShowDateHeaders` | 1 | 0/1 | SNPD's date header per entry. Also controls whether LLM-written dates are stripped (see [BOOK_TEXT.md](BOOK_TEXT.md#cleaning-llm-output-sanitizebooktext)). |
| `[Diary] EntriesPerVolume` | 10 | 1–50 | Chunk size for **new** volumes, and when the latest volume is sealed. Existing volumes keep their size: their range comes from stored boundaries, not this setting. |
| `[Fonts] TitleSize` / `DateSize` / `ContentSize` / `SmallSize` | 18 / 16 / 14 / 12 | 8–24 | Sizes in the rendered markup |
| `[Fonts] FontFace` | `$HandwrittenFont` | font name | `face=` in the rendered markup |

**Every integer setting is one row in `Config::kIntSettings`**: section, key, default and range. Getters and setters clamp to that range, so a hand-edited INI can't feed out-of-range values to the code (`EntriesPerVolume = 0` used to loop forever), and a non-numeric value falls back to the default. `Save()` and the startup log walk the same table, so the defaults can't drift apart. `Save()` builds the whole file in memory before writing it, so a failure can't leave it half-written.

**`Save()` writes only known keys.** A key that isn't in `kIntSettings` (or the two string settings, `Language` and `FontFace`) is **dropped from the file at the next startup**.

## The MCM (`SkyrimNetDiaries_MCM`, `extends SKI_ConfigBase`)

| Page | Options |
|---|---|
| `$SNPD_PageSettings` | Entries per volume (slider); show date headers (toggle); font face (menu); four font-size sliders |
| `$SNPD_PageMaintenance` | Reset All Diaries (confirm → `ResetAllDiaries`); debug logging (toggle) |

Rebuilding the book text:
- Toggling date headers → `RegenerateTextsOnly()` at once.
- Font face or size changes → one `RegenerateTextsOnly()` when the MCM closes (`OnConfigClose`), and only if something changed.
- `RegenerateTextsOnly` → `BookManager::RegenerateAllDiaryTexts`, which re-renders every tracked volume from SkyrimNet. Forms and boundaries are untouched; a volume whose query fails keeps its text.

Reset: see [DATABASE.md](DATABASE.md#mcm-reset-resetalldiariesinternal).

Strings are `$SNPD_…` keys translated in `Interface/Translations/` (UTF-16 LE with BOM; see [LOCALIZATION.md](LOCALIZATION.md#mcm-translations)).

## Adding a setting end to end

1. `Config.h`: a row in `kIntSettings` (section, key, default, range) plus a getter and setter that call `Get`/`Set` with it. String settings need their own line in `Save()`.
2. `PapyrusAPI.cpp`: `MCM_Get…` / `MCM_Set…` (the setter calls `Save()`), registered on `SkyrimNetDiaries_MCM`.
3. `SkyrimNetDiaries_MCM.psc`: `Native` declarations, the option, handlers, and a regenerate call if it changes rendering.
4. Add `$SNPD_…` keys to **all nine** translation files.
5. Compile the `.pex` and ship both (see [PAPYRUS_AND_API.md](PAPYRUS_AND_API.md#adding-or-changing-a-native-all-four-steps-every-time)).
6. Update the README's MCM section. (No INI ships with the mod; `Config::Save()` writes one on first launch.)
