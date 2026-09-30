# Localization

Book titles, dates and volume numbering follow the game language. Translators add a language by dropping in files; no rebuild is needed. The user-facing guide is the "Localization" section of [README.md](../README.md#localization).

Code: `src/Localization.cpp`, `include/Localization.h` (singleton `SkyrimNetDiaries::Localization`).

---

## Choosing the language

`Localization::Initialize()` runs in `SKSEPlugin_Load`, before `BookTextHook::Install()`:

1. `[General] Language` in `SkyrimNetPhysicalDiaries.ini`, if set (lets an English game use, say, German books).
2. Otherwise `sLanguage` from `Documents/My Games/Skyrim Special Edition/Skyrim.ini` (found with `SHGetFolderPath`).
3. Otherwise `ENGLISH`.

The name must match a locale file, e.g. `GERMAN` → `Locales/GERMAN.ini`.

## Locale files

`SKSE/Plugins/SkyrimNetPhysicalDiaries/Locales/<LANGUAGE>.ini`. Nine ship: CHINESE, ENGLISH, FRENCH, GERMAN, ITALIAN, JAPANESE, POLISH, RUSSIAN, SPANISH.

The folder is located from **the DLL's own path** (`GetModuleHandleExA` on a function address), not `SKSE::log::log_directory()`, which points to Documents.

| Section / key | Used for | Placeholders |
|---|---|---|
| `[Format] DateLong` | Entry date headers | `{Day}` weekday, `{d}` day, `{Month}`, `{y}` year |
| `[Format] DateShort` | Title-page date range | `{d}`, `{Month}`, `{y}` |
| `[Format] DiaryTitle` | Book title | `{Name}` |
| `[Format] BlankJournal` | The blank journal item's name (the ESP's is English; set at load) | |
| `[Format] JournalTitle` | The player's journal's title (writing mode; must differ from `DiaryTitle`) | `{Name}` |
| `[Format] VolumeSuffix` | Appended from volume 2 on (default `, v{n}`) | `{n}` number, `{cn}` Chinese numeral |
| `[Format] EmptyVolumeText` | Page shown when all of a volume's entries were deleted (not in a journal, which is left blank) | |
| `[Messages] TemplatesMissing` | The startup warning shown when the template books are missing. One line; `\n` is a line break. The button uses the game's own `sOk` string. | |
| `[Messages] EditSavePrompt`, `EditSave`, `EditDiscard`, `EditKeepWriting` | The book editor's prompt when the player closes their diary with unsaved changes, and its three buttons ([EDITING.md](EDITING.md#closing)). Only ENGLISH.ini has them so far; other languages get the English defaults from `Localization.cpp`. | |
| `[Messages] EditSaveFailed` | Notification when SkyrimNet doesn't save an edit. Same fallback. | |
| `[Messages] EditDeletePrompt`, `EditDelete`, `EditKeep` | The prompt when the player tears out an entry, and its two buttons ([EDITING.md](EDITING.md#tearing-out-an-entry)). Same fallback. | `{Date}` the entry's date (prompt only) |
| `[Messages] EditDeleteFailed` | Notification when SkyrimNet doesn't delete an entry. Same fallback. | |
| `[Messages] EditNeedsSkyrimNet`, `EditNeedsPause` | Notifications when writing can't start: SkyrimNet is older than public API v11, or the book menu doesn't pause the game ([EDITING.md](EDITING.md#opening)). Same fallback. | |
| `[Messages] EditEmptiedHint` | Notification when a save keeps an emptied entry (removing one is tearing it out). Same fallback. | |
| `[Messages] EditNotLatest`, `EditNoJournal`, `EditJournalNotCarried` | Notifications for the new-entry key: new entries go in the latest journal; during play, the player has no journal, or doesn't carry it ([EDITING.md](EDITING.md#new-entries)). Same fallback. | |
| `[Messages] EditNeedsQuill`, `EditInkRanDry`, `EditOk` | Quill and ink: the message box when writing can't start (OK button), and the notice when an inkwell runs dry ([EDITING.md](EDITING.md#quill-and-ink)). Same fallback. | |
| `[Messages] EditBloodPrompt`, `EditBloodYes`, `EditBloodNo`, `EditTooWeak` | No ink: the prompt to write in blood and its buttons, and the message box when the player is too weak ([EDITING.md](EDITING.md#writing-in-blood)). Same fallback. | |
| `[Messages] EditStartedVolume`, `BlankJournalFailed` | A blank journal read from the inventory became the player's journal, or couldn't ([EDITING.md](EDITING.md#reading-a-blank-journal)). Same fallback. | |
| `[Months]`, `[Days]` | Optional name overrides, keyed by the English name | |

Anything missing falls back to English.

## Month and day names

Precedence: locale `[Months]`/`[Days]` → the game's GMSTs → English. It applies per **list**, not per name: a single `[Months]` key makes all twelve month names come from the locale file (English for any it leaves out) and skips the GMSTs; `[Days]` works the same way. So a locale file should list all of them, and never leave a value empty (an empty name would match every entry when stripping LLM-written dates). `ReadGMSTs()` runs at `kDataLoaded`, since GMSTs are not loaded at plugin load. It reads `sMonthJanuary`… (which hold the Tamrielic names, Morning Star onward) and `sDaySunday`…. Mods such as Seasons of Skyrim change the month GMSTs, which is why locale files should include `[Months]` and `[Days]`.

## MCM translations

`Interface/Translations/SkyrimNet Physical Diaries_<LANGUAGE>.txt`, one per shipped locale. **Must be UTF-16 LE with BOM**; Skyrim ignores UTF-8 translation files. Keys are the `$SNPD_…` strings used in `SkyrimNetDiaries_MCM.psc`.

## Rules

- Don't hard-code user-visible, language-specific strings outside `Localization.cpp` and the locale files.
- **Never compare names.** Book titles and NPC names are localized (theft detection used to filter on the English word "Diary" and did nothing in 8 of 9 languages). Identify books by FormID (`BookManager::GetBookForFormID`) and NPCs by UUID or base form.
- Cyrillic text is converted to Windows-1251 at injection time. See [BOOK_TEXT.md](BOOK_TEXT.md#utf-8--windows-1251).
- The MCM font dropdown: `$StartGameFont` renders as boxes, and in vanilla `$HandwrittenBold` is the same as `$HandwrittenFont`. SkyUI's `OnOptionMenuOpen` must fill the list without checking the option ID, or the menu opens empty.
