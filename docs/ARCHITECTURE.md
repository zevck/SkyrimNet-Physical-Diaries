# SkyrimNet Physical Diaries Architecture

SkyrimNet Physical Diaries (SNPD) is an SKSE plugin (`SkyrimNetPhysicalDiaries.dll`) plus three small Papyrus scripts, an ESP and a SkyrimNet prompt plugin. It turns the diary entries that [SkyrimNet](https://github.com/MinLL/SkyrimNet-GamePlugin) writes for NPCs into real book items. SkyrimNet already stores those entries, but only as AI context and dashboard text. SNPD reads them through SkyrimNet's public API, splits them into volumes, gives each volume a book form, puts the book in the NPC's inventory, and supplies the text when the player opens it.

**SNPD is a presentation and interaction layer over SkyrimNet's data.** It never writes diary content and never changes SkyrimNet's entries. It owns only the book forms and its own SQLite state. Any design that puts authoritative diary content in SNPD's storage is wrong.

This document gives the map. Follow the links in [Related docs](#related-docs) for detail.

---

## Repo layout

The repo has the layout of an MO2 mod folder. Builds are deployed as a `Physical Diaries - Dev` mod folder into one test instance per runtime (SE, AE, VR). See [DEVELOPMENT.md](DEVELOPMENT.md#deploy).

| Path | What lives there |
|---|---|
| `src/` | C++ sources |
| `include/` | Headers, one per component, plus `PCH.h` (force-included: `RE/Skyrim.h`, `SKSE/SKSE.h`, spdlog), `SkyrimNetPublicAPI.h` (vendored SkyrimNet API loader) and `SkyrimNetPhysicalDiariesAPI.h` (SNPD's own inter-plugin API, for other mods) |
| `lib/commonlibsse-ng/` | CommonLibSSE-NG v9.1.0 as a git submodule. See [DEVELOPMENT.md](DEVELOPMENT.md#build). |
| `Source/Scripts/*.psc` → `Scripts/*.pex` | Papyrus: `SkyrimNetDiaries_EventListener`, `_MCM`, `_Native`. See [PAPYRUS_AND_API.md](PAPYRUS_AND_API.md). |
| `spriggit/SkyrimNetPhysicalDiaries/` → `SkyrimNet Physical Diaries.esp` | The plugin's source as Spriggit YAML, built into `build\esp` ([PLUGIN.md](PLUGIN.md)): the four template books (`SkyrimNetDiaryTemplate`, `…2`, `…3`, `…N`), the MCM quest and the event-listener quest, the blank journals with their leveled list and recipes, the quill and inkwell form lists, and the partly used inkwells (every record: [PLUGIN.md](PLUGIN.md#records)). Also `SNPD_DiaryStolenFaction`, which nothing uses any more (see [KNOWN_ISSUES.md](KNOWN_ISSUES.md)). |
| `SKSE/Plugins/SkyrimNetPhysicalDiaries.dll` | Build output, not tracked (`*.dll` and `*.pex` are gitignored). |
| `SKSE/Plugins/SkyrimNet/external/zevick.physical-diaries/` | SkyrimNet Beta 25+ plugin (external layer): `manifest.json` and `prompts/submodules/system_head/0500_diary_stolen.prompt`, which puts the stolen-diary line in the diary prompt. See [THEFT.md](THEFT.md#how-skyrimnet-learns-about-it). |
| `SKSE/Plugins/SkyrimNetPhysicalDiaries/Locales/*.ini` | Per-language date and title formats. See [LOCALIZATION.md](LOCALIZATION.md). |
| `Interface/Translations/SkyrimNet Physical Diaries_*.txt` | MCM translations (UTF-16 LE with BOM) |
| `swf/<name>/` → `Interface/<name>.swf` | Flash UI: base movies as JPEXS XML (the default plus variants for other UI mods) and our ActionScript classes, built by `Build_Local.ps1` (the SWFs are gitignored). `swf/book/` is the book menu with SNPD's edit mode: vanilla, and a Convenient Reading variant. See [DEVELOPMENT.md](DEVELOPMENT.md#swf). |
| `SkyrimNet Physical Diaries_DESC.ini` | Description Framework descriptions of the inkwells' fill (optional DF; deployed to the mod's root). See [EDITING.md](EDITING.md#quill-and-ink). |
| `Seq/SkyrimNet Physical Diaries.seq` | The start-game dialogue quests, for the engine ([PLUGIN.md](PLUGIN.md)) |
| `utilities/` | `Spriggit.ps1` (the pinned Spriggit CLI) and `esp_to_spriggit.ps1` (an ESP edited in CK or xEdit back to YAML). See [PLUGIN.md](PLUGIN.md). |
| `docs/` | These developer docs |
| Root `*.md` other than `README.md` | Old design notes from single investigations. Most are gitignored. They predate the current code. |

Runtime files. MO2 virtualizes `Data/`, so files SNPD creates land in `overwrite/`.

| File | Written by |
|---|---|
| `Data/SKSE/Plugins/SkyrimNetPhysicalDiaries/<saveFolder>/diary.db` | `DiaryDB` (SQLite). One per SkyrimNet save folder. See [DATABASE.md](DATABASE.md). |
| `Data/SKSE/Plugins/SkyrimNetPhysicalDiaries.ini` | `Config::Save()`, which runs at every startup and on MCM changes |
| `Documents/My Games/Skyrim Special Edition/SKSE/SkyrimNetPhysicalDiaries.log` | The plugin log, truncated each launch |

SNPD also **reads** `SkyrimNet.log` (same folder as its own log) to learn the active save ID, and checks that `Data/SKSE/Plugins/SkyrimNet/data/SkyrimNet-<id>.db` exists.

---

## Component map

```
 SkyrimNet (LLM) writes a diary entry
        │  ModEvent "SkyrimNet_DiaryCreated" {actorFormId,…}
        ▼
 SkyrimNetDiaries_EventListener.psc ──► SkyrimNetDiaries_Native.UpdateDiaryFromEvent(json)
        │                                         (PapyrusAPI.cpp)
        ▼
 VolumeSync.cpp  UpdateDiaryForActorInternal ──► Database.cpp ──► SkyrimNet public API (JSON)
        │   CreateAllVolumesForActor / seal / update in place
        ▼
 BookManager  CreateDiaryBook ─► DynamicForms::Create (engine form factory) ─► configure from template
        │        ─► track (co-save record) ─► RegisterBook ─► DiaryDB
        │        FindActorForBook ─► AddObjectToContainer (game thread)
        ▼
 NPC inventory ◄── pickpocket / trade / return ──► DiaryTheftHandler ──► DiaryDB stolen_volumes
        │                                                                     │
 player opens book                                  snpd_diary_stolen decorator (SkyrimNet prompt)
        ▼
 BookTextHook (TESDescription::GetDescription) ─► BookManager::RefreshVolumeOnOpen ─► diary text
```

| Component | Files | Role |
|---|---|---|
| Entry point, lifecycle | `src/main.cpp` | `SKSEPlugin_Load` and the SKSE message handler (`OnMessage`): template check at `kDataLoaded`, the `kPostLoadGame` sequence |
| Entry → volume sync | `src/VolumeSync.cpp`, `include/VolumeSync.h` | `UpdateDiaryForActorInternal` (create, update, seal, overflow), the recovery and catch-up scans, MCM reset. See [VOLUMES_AND_SYNC.md](VOLUMES_AND_SYNC.md). |
| Book forms and volumes | `src/BookManager.cpp`, `include/BookManager.h` | Template choice, the in-memory volume registry (`books_`) and its FormID and description indexes, load from DB (matched against the save's books), refresh on open. See [BOOK_FORMS.md](BOOK_FORMS.md). |
| Book creation | `src/BookCreation.cpp`, `include/BookCreation.h` | `CreateDiaryBook` (synchronous), `ConfigureDiaryForm`, filling in the save's books on load, volume keys, sweeping retired books' copies away (at retirement, each load, and as cells attach). See [BOOK_FORMS.md](BOOK_FORMS.md). |
| Dynamic forms | `src/DynamicForms.cpp`, `include/DynamicForms.h` | Generic, no SNPD types: engine-persisted runtime forms, their co-save record, retirement (a saved flag: forms are never removed), rebuilding world copies' 3D after a load. See [BOOK_FORMS.md](BOOK_FORMS.md). |
| Actor lookup | `src/ActorLookup.cpp`, `include/ActorLookup.h` | `FindActorForBook`: volume → owning NPC by UUID, with a per-session cache |
| Text rendering | `src/BookText.cpp`, `include/BookText.h` | `FormatDiaryEntries`, text sanitizing, game-date formatting. See [BOOK_TEXT.md](BOOK_TEXT.md). |
| Save folder, co-save | `src/SaveFolder.cpp`, `src/Serialization.cpp` (+ headers) | Detecting the SkyrimNet save folder from `SkyrimNet.log`; the co-save callbacks and the book-forms record. See [DATABASE.md](DATABASE.md). |
| Timeline gate | `src/TimelineGate.cpp`, `include/TimelineGate.h` | Holds the post-load sync until SkyrimNet's keep/clear timeline check has resolved (`PublicGetTimelineState`). See [VOLUMES_AND_SYNC.md](VOLUMES_AND_SYNC.md#waiting-for-the-decision-timelinegate). |
| Inter-plugin API | `src/InterPluginAPI.cpp`, `include/InterPluginAPI.h` | Answers `SNPD_QUERY_*` SKSE messages. See [PAPYRUS_AND_API.md](PAPYRUS_AND_API.md#inter-plugin-api-skse-messaging). |
| Persistence | `src/DiaryDB.cpp`, `include/DiaryDB.h` | Per-save SQLite: volumes, actor templates, stolen volumes, the ranges of journal text written in blood. See [DATABASE.md](DATABASE.md). |
| SkyrimNet client | `src/Database.cpp`, `include/Database.h`, `include/SkyrimNetPublicAPI.h` | Loads SkyrimNet's exported functions, parses diary JSON, UUID ↔ FormID, names, bio template names |
| Diary editing | `src/BookEditor.cpp` (the edit session), `src/EditorWrites.cpp` (the write queue), `src/EditorJournals.cpp` (which journal, blank journals), `include/BookEditor.h`, `include/EditorInternal.h` (their shared state), `swf/book/scripts/__Packages/BookMenu.as` | The player writing in their own journal in the book menu: loading the volume into the SWF's edit mode, keyboard input, saving changed entries to SkyrimNet, writing new ones and tearing entries out (all through one write queue, then `BookManager::ReconcileAfterWrite`), turning a blank journal the player reads into a new journal (journals sit side by side, each holding the entries tagged for it), going back to reading, the `BookMenu::ProcessMessage` hook that turns a close with unsaved changes into a save prompt. Started by a key while the journal is open. See [EDITING.md](EDITING.md). |
| NPC diaries | `src/NpcDiaries.cpp`, `include/NpcDiaries.h` | Optional: once a game day, picks NPCs (SkyrimNet's engagement data, the whitelist faction) and asks SkyrimNet's Papyrus `GenerateDiaryEntry` to write for them; the whitelist dialogue's notice. See [NPC_DIARIES.md](NPC_DIARIES.md). |
| Blank journals | `src/BlankJournals.cpp`, `include/BlankJournals.h` | The blank journal items: their localized name, adding them to general-goods merchants' stock in memory, hiding their recipes while writing is off. See [EDITING.md](EDITING.md#blank-journals). |
| Writing tools | `src/WritingTools.cpp`, `include/WritingTools.h` | Quill and ink: whether the player has a quill, using one dip of ink by swapping their emptiest inkwell for the ESP's next partly used one. See [EDITING.md](EDITING.md#quill-and-ink). |
| Writing mode | `src/WritingMode.cpp`, `include/WritingMode.h` | Whether SNPD's `book.swf` is installed (a marker with an interface version, read from the file at `kDataLoaded`); player writing exists only then. See [EDITING.md](EDITING.md#writing-mode). |
| Text injection | `src/BookTextHook.cpp`, `include/BookTextHook.h` | Hook on `TESDescription::GetDescription`: the book menu's text (no parent form: refresh, styled, Win-1251 for Cyrillic) and other readers' (SkyrimNet's book-read event, Immersive Reading: cached UTF-8). See [BOOK_TEXT.md](BOOK_TEXT.md). |
| Theft | `src/DiaryTheftHandler.cpp`, `include/DiaryTheftHandler.h` | Container-change and menu sinks that record theft, returns and willing handovers; the `snpd_diary_stolen` decorator registration and post-load theft reconciliation. See [THEFT.md](THEFT.md). |
| Papyrus natives | `src/PapyrusAPI.cpp`, `include/PapyrusAPI.h` | MCM getters and setters, the theft API, `UpdateDiaryFromEvent` |
| Localization | `src/Localization.cpp`, `include/Localization.h` | Language detection, locale `.ini`, GMST month and day names, title and date formats. See [LOCALIZATION.md](LOCALIZATION.md). |
| Settings | `include/Config.h` (header-only) | INI load and save. See [CONFIG_AND_MCM.md](CONFIG_AND_MCM.md). |

---

## Startup and load sequence

**`SKSEPlugin_Load`** (`main.cpp`), in order:
1. `InitializeLog()`, then `SKSE::Init`.
2. `Config::Load()` followed at once by `Config::Save()`, so MO2 copies the INI into `overwrite/` and user settings survive mod updates. Debug level is applied from `[General] DebugLog`.
3. Register `OnMessage`; `Serialization::Register()` registers the co-save callbacks under the unique ID `'SNDB'`.
4. `DiaryTheftHandler::Register()` (event sinks), `RegisterRetiredBookSweeper()` (cell-attach sink), `Localization::Initialize()`, `BookTextHook::Install()`, `PapyrusAPI::Register()`.

**`kDataLoaded`**: `Database::InitializeAPI()` and `DiaryTheftHandler::RegisterStolenDecorator()` (the native `snpd_diary_stolen` decorator, once); verify all four templates resolve by EditorID and show a message box naming the likely causes if not;  `WritingMode::Detect()` and, only if writing is on, `BookEditor::Register()` and `WritingTools::OnDataLoaded()`; `BlankJournals::OnDataLoaded()`; `NpcDiaries::OnDataLoaded()` (its records, the topics' localized text, the menu sink and the clock thread; [NPC_DIARIES.md](NPC_DIARIES.md)); `Localization::ReadGMSTs()`.

**`kPreLoadGame`** and **`kNewGame`** both end the session (`EndSession` in `main.cpp`): bump the load generation (an older setup still waiting gives up), `TimelineGate::Reset()`, `BookManager::ClearActorCache()`, close DiaryDB, clear the save folder and the in-memory stolen set, and `SetPostLoadSyncReady(false)` so diary events wait for this load's sync and those deferred from the last session are dropped. `kNewGame` then sets it back to true, since no `kPostLoadGame` follows. A new game gets no `kPreLoadGame`, so before 2026-09-27 a New Game after loading a save kept writing into the previous character's DiaryDB.

**Load callback** (inside the load, before `kPostLoadGame`): read the save's book-forms record and fill in its books (`DynamicForms::Load`, `ConfigureLoadedBooks`), and the NPC diaries record (`SNND`: the day last run, the daily writers). See [BOOK_FORMS.md](BOOK_FORMS.md#load).

**`kPostLoadGame`**:
1. `DynamicForms::RebuildLoadedWorldCopies()`: world copies of our books in the loaded cells were built before the load callback filled the books in. Runs first and does not depend on SkyrimNet. Then `NpcDiaries::SyncEnabled()`: the save restored the dialogue's global, so it is set from the INI again.
2. `Database::InitializeAPI()`. If SkyrimNet is not loaded, stop here.
3. The post-load sync polls every 100 ms (a sleeper thread re-queues a game-thread task) until `Database::IsMemorySystemReady()` (up to 60 s) **and** `TimelineGate::IsSettled()` (no limit while SkyrimNet's keep/clear check is pending). Then:
   - Detect the save folder from `SkyrimNet.log` and `DiaryDB::Open()` it.
   - **If the DB didn't open or the memory system never became ready**, `PauseDiaryBooks()` and stop: with this save's volumes not loaded, every NPC would look new and get a second set of books. Diary events are ignored (logged at debug) until the next load or new game, and the error names the cause.
   - `LoadFromDB()` (rows matched against the save's books; volumes without a book are queued for recreation, books without a volume kept unclaimed), `WarnIfWritingOff()` (writing off but the save has journals: a message box once per game run), `MigrateVolumesToIds()` (once per DiaryDB: volumes from before 2.1 re-cut by entry id, see [DATABASE.md](DATABASE.md#schema-changes)), then `ReconcileWithTimeline()` (volumes reaching past the loaded save are matched against the history SkyrimNet kept).
   - `DiaryTheftHandler::ReconcileAfterLoad()`: drop theft records made after the loaded save's game time (`stolen_at > now`), then reload the in-memory stolen set the decorator reads.
   - `SetPostLoadSyncReady(true)`, then queue immediate recreation for actors whose volumes had no book in this save, then `QueueNewEntryRecovery()` and `QueueBatchCatchUpScan()`.

**Save**: the engine writes our books' change records itself, retired ones included. The co-save `SaveCallback` writes the book-forms record first, then the NPC diaries record (`SNND`), then opens the DB if it isn't open (a new game that never got a `kPostLoadGame`; not during a load's post-load wait, when `SkyrimNet.log` may still name the previous save) and runs `FlushToDB()`. See [DATABASE.md](DATABASE.md#co-save-records).

**New game or load**: `RevertCallback` clears in-memory state, including the tracked forms. `diary.db` on disk is kept on purpose.

---

## One diary entry, end to end

1. SkyrimNet generates an entry. The `snpd_diary_stolen` decorator is evaluated **during** generation, so an NPC whose diary is missing writes about it.
2. SkyrimNet sends ModEvent `SkyrimNet_DiaryCreated`. `SkyrimNetDiaries_EventListener.psc` passes the JSON payload to `SkyrimNetDiaries_Native.UpdateDiaryFromEvent`, which reads `actorFormId` in C++ (a Papyrus `int` can't hold FormIDs of `0x80000000` and up).
3. `UpdateDiaryForFormID` (`PapyrusAPI.cpp`) clears that actor's stolen volumes (the theft has now been written about) and calls `UpdateDiaryForActorInternal` (`VolumeSync.cpp`).
4. `UpdateDiaryForActorInternal` fetches entries and takes one of three paths: create every volume (no volumes yet), start a new volume (the NPC no longer holds the latest one), or update or seal the latest volume in place. See [VOLUMES_AND_SYNC.md](VOLUMES_AND_SYNC.md).
5. New volumes go through `BookManager::CreateDiaryBook`, which creates the form with the engine's factory, configures it from its template, tracks it for the co-save, registers the volume, writes the rendered text to DiaryDB and adds the book to the NPC, all before it returns. See [BOOK_FORMS.md](BOOK_FORMS.md).
6. The player opens the book. The engine asks for its description; `BookTextHook` finds the volume by the description component, calls `RefreshVolumeOnOpen` (which catches entries SkyrimNet added or deleted since the last render), converts the text and returns it in place of the book's own. See [BOOK_TEXT.md](BOOK_TEXT.md).

---

## Threading

| Context | What runs there |
|---|---|
| Main thread | SKSE messages, event sinks, and **Papyrus natives**: they are registered with `callableFromTasklets = false` (CommonLib's default), so the VM defers each call to the game thread. While the game is paused (a menu is open), also every `AddTask` body and input, so opening a book from an inventory runs here. |
| Any thread | The text hook: it reads `BookManager`'s text snapshot and description index, which are under their own mutex, never `books_`. The book-open refresh runs inline only on the main thread while paused; otherwise it is queued as a task. |
| The "Poll controls" job (during play) | Every `AddTask` body, then input polling, one after the other. Player activation happens here, so a book read from the world asks the text hook from this job. |
| Detached `std::thread` | Sleepers that wait, then queue a game-thread task: the post-load readiness poll, `DeferUntilSyncReady`, the NPC diaries clock (a check every 10 s, for the process's life) its spaced-out requests (one every 3 s; each checks a session counter `Revert` bumps, and drops itself after a load), and a check every 100 ms while the Sleep/Wait menu is open. The NPC diaries' `MenuOpenCloseEvent` sink runs on the main thread: it notes where a sleep, wait or fast travel starts and runs a skipped day when it ends ([NPC_DIARIES.md](NPC_DIARIES.md#when)). |
| The editor's write queue | One detached worker, started on the first write, that runs the book editor's saves and deletions in order (SkyrimNet blocks while it re-embeds an entry's memory). Each finished job queues a game-thread task; a job from before a load (`BookEditor::Reset` bumps a generation) is ignored there. |

Rules: anything touching forms, inventories or references must run on the game thread (`SKSE::GetTaskInterface()->AddTask`). "Game thread" means wherever SKSE runs task bodies: the main thread while paused, the "Poll controls" job during play (SKSE calls its task queue from the empty `BSTaskPool` hook target, AE `0x140A4DF00`, which only `Main::Update` and that job call). It is not a fixed OS thread, so never compare against a thread id to decide whether `books_` may be touched. Don't register a native with `callableFromTasklets = true`: it would then run on a VM thread and race `books_`. `DynamicForms`, the actor cache and `BookManager`'s text snapshot (with the description index) each have their own mutex. `books_` has none and is only touched from the game thread, so keep it that way.

---

## Dependencies

| Dependency | Why |
|---|---|
| SkyrimNet | The source of all diary content. SNPD resolves its exports from SkyrimNet's DLL at runtime (`SkyrimNetPublicAPI.h`), and requires public API v11 (`kRequiredApiVersion`): volumes are read with `PublicQueryDiaryEntries` in entry-id order, journals are written with `PublicAddDiaryEntry`, `PublicUpdateDiaryEntry` and `PublicDeleteDiaryEntry`. With an older SkyrimNet, `InitializeAPI` fails, so nothing runs, and a message box at the main menu says so (`[Messages] SkyrimNetTooOld`). |
| powerofthree's Tweaks **or** Native EditorID Fix | Templates are found with `LookupByEditorID`, which needs one of these. Don't read a form's own ID with `GetFormEditorID()`: it returns "" for books without Native EditorID Fix. |
| SkyUI | MCM |
| Address Library (SE/AE) or VR Address Library | The `GetDescription` hook and the `BookMenu` vtable the editor's hooks patch. See [DEVELOPMENT.md](DEVELOPMENT.md#engine-touchpoints). |
| Build: CommonLibSSE-NG v9.1.0 (submodule), vcpkg `sqlite3`, `nlohmann-json`, `spdlog`, `fmt`, `minhook` | |

---

## Related docs

- [BOOK_FORMS.md](BOOK_FORMS.md): engine-persisted book forms, the co-save record, retirement, templates, actor resolution
- [VOLUMES_AND_SYNC.md](VOLUMES_AND_SYNC.md): entries → volumes, recovery scans, save reverts
- [BOOK_TEXT.md](BOOK_TEXT.md): formatting, sanitizing, the hook
- [THEFT.md](THEFT.md): theft, return and handover
- [DATABASE.md](DATABASE.md): DiaryDB and co-save
- [EDITING.md](EDITING.md): the player editing their diary
- [PAPYRUS_AND_API.md](PAPYRUS_AND_API.md): scripts, natives, inter-plugin API
- [DEVELOPMENT.md](DEVELOPMENT.md): build, deploy, logging, engine touchpoints
