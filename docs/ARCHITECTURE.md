# SkyrimNet Physical Diaries Architecture

SkyrimNet Physical Diaries (SNPD) is an SKSE plugin (`SkyrimNetPhysicalDiaries.dll`) plus four small Papyrus scripts, an ESP and a SkyrimNet prompt plugin. It turns the diary entries that [SkyrimNet](https://github.com/MinLL/SkyrimNet-GamePlugin) writes for NPCs into real book items. SkyrimNet already stores those entries, but only as AI context and dashboard text. SNPD reads them through SkyrimNet's public API, splits them into volumes, gives each volume a book form, puts the book in the NPC's inventory, and supplies the text when the player opens it.

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
| `Source/Scripts/*.psc` → `Scripts/*.pex` | Papyrus: `SkyrimNetDiaries_API`, `_EventListener`, `_MCM`, `_Native`. See [PAPYRUS_AND_API.md](PAPYRUS_AND_API.md). |
| `SkyrimNet Physical Diaries.esp` | The four template books (`SkyrimNetDiaryTemplate`, `…2`, `…3`, `…N`), the MCM quest and the event-listener quest. Also `SNPD_DiaryStolenFaction`, which nothing uses any more (see [KNOWN_ISSUES.md](KNOWN_ISSUES.md)). |
| `SKSE/Plugins/SkyrimNetPhysicalDiaries.dll` | Build output, not tracked (`*.dll` and `*.pex` are gitignored). |
| `SKSE/Plugins/SkyrimNet/external/zevick.physical-diaries/` | SkyrimNet Beta 25+ plugin (external layer): `manifest.json` and `prompts/submodules/system_head/0500_diary_stolen.prompt`, which puts the stolen-diary line in the diary prompt. See [THEFT.md](THEFT.md#how-skyrimnet-learns-about-it). |
| `SKSE/Plugins/SkyrimNetPhysicalDiaries/Locales/*.ini` | Per-language date and title formats. See [LOCALIZATION.md](LOCALIZATION.md). |
| `Interface/Translations/SkyrimNet Physical Diaries_*.txt` | MCM translations (UTF-16 LE with BOM) |
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
 BookManager  CreateDiaryBook ─► serial queue ─► DPF.Create() (Papyrus VM) ─► DPFCreateCallback
        │        claim FormID ─► configure from template ─► RegisterBook ─► DiaryDB
        │        FindActorForBook ─► AddObjectToContainer (game thread)
        ▼
 NPC inventory ◄── pickpocket / trade / return ──► DiaryTheftHandler ──► DiaryDB stolen_volumes
        │                                                                     │
 player opens book                                  snpd_diary_stolen decorator (SkyrimNet prompt)
        ▼
 BookTextHook (BookMenu::OpenBookMenu) ─► BookManager::RefreshVolumeOnOpen ─► inject text
```

| Component | Files | Role |
|---|---|---|
| Entry point, lifecycle | `src/main.cpp` | `SKSEPlugin_Load` and the SKSE message handler (`OnMessage`): dependency checks at `kDataLoaded`, `kSaveGame`, the `kPostLoadGame` sequence |
| Entry → volume sync | `src/VolumeSync.cpp`, `include/VolumeSync.h` | `UpdateDiaryForActorInternal` (create, update, seal, overflow), the recovery and catch-up scans, MCM reset. See [VOLUMES_AND_SYNC.md](VOLUMES_AND_SYNC.md). |
| Book forms and volumes | `src/BookManager.cpp`, `include/BookManager.h` | Template choice, the in-memory volume registry (`books_`), load from DB, inventory re-add, refresh on open. See [BOOK_FORMS.md](BOOK_FORMS.md). |
| Book creation | `src/BookCreation.cpp`, `include/BookCreation.h` | `CreateDiaryBook`, the serial DPF create queue, `DPFCreateCallback`, the FormID claim table, the `sourceFiles` fix. See [BOOK_FORMS.md](BOOK_FORMS.md). |
| Actor lookup | `src/ActorLookup.cpp`, `include/ActorLookup.h` | `FindActorForBook`: volume → owning NPC by UUID, with a per-session cache |
| Text rendering | `src/BookText.cpp`, `include/BookText.h` | `FormatDiaryEntries`, text sanitizing, game-date formatting. See [BOOK_TEXT.md](BOOK_TEXT.md). |
| Save folder, co-save | `src/SaveFolder.cpp`, `src/Serialization.cpp` (+ headers) | Detecting the SkyrimNet save folder from `SkyrimNet.log`; the co-save callbacks (used for their timing; no records). See [DATABASE.md](DATABASE.md). |
| Timeline gate | `src/TimelineGate.cpp`, `include/TimelineGate.h` | Holds the post-load sync until SkyrimNet's keep/clear timeline prompt is answered (MinHook detour on `MessageBoxData::QueueMessage`). See [VOLUMES_AND_SYNC.md](VOLUMES_AND_SYNC.md#waiting-for-the-decision-timelinegate). |
| Inter-plugin API | `src/InterPluginAPI.cpp`, `include/InterPluginAPI.h` | Answers `SNPD_QUERY_*` SKSE messages. See [PAPYRUS_AND_API.md](PAPYRUS_AND_API.md#inter-plugin-api-skse-messaging). |
| Persistence | `src/DiaryDB.cpp`, `include/DiaryDB.h` | Per-save SQLite: volumes, actor templates, stolen volumes. See [DATABASE.md](DATABASE.md). |
| SkyrimNet client | `src/Database.cpp`, `include/Database.h`, `include/SkyrimNetPublicAPI.h` | Loads SkyrimNet's exported functions, parses diary JSON, UUID ↔ FormID, names, bio template names |
| Text injection | `src/BookTextHook.cpp`, `include/BookTextHook.h` | Hooks on `BookMenu::OpenBookMenu` (substitutes diary text when a book opens; passes VR's ninth argument) and `TESDescription::GetDescription` (diary text for other readers such as SkyrimNet's book-read event and Immersive Reading); UTF-8 → Win-1251. See [BOOK_TEXT.md](BOOK_TEXT.md). |
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
4. `DiaryTheftHandler::Register()` (event sinks), `Localization::Initialize()`, `BookTextHook::Install()`, `TimelineGate::Install()`, `PapyrusAPI::Register()`.

**`kDataLoaded`**: warn with a message box if `Dynamic Persistent Forms.esp` is missing; `Database::InitializeAPI()` and `DiaryTheftHandler::RegisterStolenDecorator()` (the native `snpd_diary_stolen` decorator, once); verify all four templates resolve by EditorID and show a message box naming the likely causes if not; `TimelineGate::OnDataLoaded()` (finds SkyrimNet's prompt text); `Localization::ReadGMSTs()`.

**`kPreLoadGame`** and **`kNewGame`** both end the session (`EndSession` in `main.cpp`): bump the load generation (an older setup still waiting gives up), `TimelineGate::Reset()`, `CancelPendingCreations()`, `BookManager::ClearActorCache()` (actor cache and FormID claims), close DiaryDB, clear the save folder and the in-memory stolen set, and `SetPostLoadSyncReady(false)` so diary events wait for this load's sync and those deferred from the last session are dropped. `kNewGame` then sets it back to true, since no `kPostLoadGame` follows. A new game gets no `kPreLoadGame`, so before 2026-09-27 a New Game after loading a save kept writing into the previous character's DiaryDB.

**`kPostLoadGame`**:
1. `BookManager::SanitizeLoadedBookForms()`: clear the invalid `sourceFiles` pointer DPF leaves on some clones (VR). Runs first and does not depend on SkyrimNet.
2. `Database::InitializeAPI()`. If SkyrimNet is not loaded, stop here.
3. The post-load sync polls every 100 ms (a sleeper thread re-queues a game-thread task) until `Database::IsMemorySystemReady()` (up to 60 s) **and** `TimelineGate::IsSettled()` (no limit while SkyrimNet's keep/clear prompt is open). Then:
   - Detect the save folder from `SkyrimNet.log` and `DiaryDB::Open()` it.
   - **If the DB didn't open, DPF isn't installed, or the memory system never became ready**, `PauseDiaryBooks()` and stop: with this save's volumes not loaded, every NPC would look new and get a second set of books. Diary events are ignored (logged at debug) until the next load or new game, and the error names the cause.
   - `LoadFromDB()`, then `ReconcileWithTimeline()` (volumes reaching past the loaded save are matched against the history SkyrimNet kept), then `QueueInventoryCheck()`.
   - `DiaryTheftHandler::ReconcileAfterLoad()`: drop theft records made after the loaded save's game time (`stolen_at > now`), then reload the in-memory stolen set the decorator reads.
   - `SetPostLoadSyncReady(true)`, then queue immediate recreation for actors whose book forms were invalid, then `QueueNewEntryRecovery()` and `QueueBatchCatchUpScan()`.

**Save**: SKSE sends `kSaveGame` first, which marks every volume `persisted_in_save` (in DiaryDB and in memory). Then the co-save `SaveCallback` runs: it opens the DB if it isn't open (a new game that never got a `kPostLoadGame`; not during a load's post-load wait, when `SkyrimNet.log` may still name the previous save), and runs `FlushToDB()` (which carries the in-memory persisted flag, so a DB opened only here still gets it). No co-save records are written. See [DATABASE.md](DATABASE.md#co-save-records).

**New game or load**: `RevertCallback` clears in-memory state. `diary.db` on disk is kept on purpose.

---

## One diary entry, end to end

1. SkyrimNet generates an entry. The `snpd_diary_stolen` decorator is evaluated **during** generation, so an NPC whose diary is missing writes about it.
2. SkyrimNet sends ModEvent `SkyrimNet_DiaryCreated`. `SkyrimNetDiaries_EventListener.psc` passes the JSON payload to `SkyrimNetDiaries_Native.UpdateDiaryFromEvent`, which reads `actorFormId` in C++ (a Papyrus `int` can't hold FormIDs of `0x80000000` and up).
3. `UpdateDiaryForFormID` (`PapyrusAPI.cpp`) clears that actor's stolen volumes (the theft has now been written about) and calls `UpdateDiaryForActorInternal` (`VolumeSync.cpp`).
4. `UpdateDiaryForActorInternal` fetches entries and takes one of three paths: create every volume (no volumes yet), start a new volume (the NPC no longer holds the latest one), or update or seal the latest volume in place. See [VOLUMES_AND_SYNC.md](VOLUMES_AND_SYNC.md).
5. New volumes go through `BookManager::CreateDiaryBook` → the serial create queue → `DPF.Create()` → `DPFCreateCallback`, which claims the FormID, configures the form from its template, registers the volume, writes the rendered text to DiaryDB and adds the book to the NPC. See [BOOK_FORMS.md](BOOK_FORMS.md).
6. The player opens the book. `BookTextHook` finds the volume by FormID, calls `RefreshVolumeOnOpen` (which catches entries SkyrimNet added or deleted since the last render), converts the text and passes it to the engine in place of the book's own description. See [BOOK_TEXT.md](BOOK_TEXT.md).

---

## Threading

| Context | What runs there |
|---|---|
| Game (main) thread | SKSE messages, every `AddTask` body, the book hook, event sinks, the `TimelineGate` prompt callback, and **Papyrus natives**: they are registered with `callableFromTasklets = false` (CommonLib's default), so the VM defers each call to the game thread |
| Papyrus VM thread | Only the `DPF.Create()` callback (`DPFCreateCallback::operator()`) |
| Whichever thread queues a message box | The `QueueMessage` hook (`TimelineGate`): it only compares text and wraps a callback |
| Detached `std::thread` | Sleepers that wait, then queue a game-thread task: the post-load readiness poll, `DeferUntilSyncReady` |
| One timer thread (`CantTakeTimer`) | Clears `kCantTake` on each created book 5 s after creation (queues the clear to the game thread) |

Rules: anything touching forms, inventories or references must run on the game thread (`SKSE::GetTaskInterface()->AddTask`). Don't register a native with `callableFromTasklets = true`: it would then run on a VM thread and race `books_`. The DPF callback does only the FormID claim itself and hands the rest to a task. The create queue, the claim table and the actor cache each have their own mutex. `books_` has none and is only touched from the game thread, so keep it that way.

---

## Dependencies

| Dependency | Why |
|---|---|
| SkyrimNet | The source of all diary content. SNPD resolves its exports from SkyrimNet's DLL at runtime (`SkyrimNetPublicAPI.h`), checks `PublicGetVersion`, and does nothing if they are missing. |
| Dynamic Persistent Forms | Creates the runtime book forms. Called only through the Papyrus VM (`DispatchStaticCall("DynamicPersistentForms", "Create", …)`). See [BOOK_FORMS.md](BOOK_FORMS.md#alternatives-evaluated). |
| powerofthree's Tweaks **or** Native EditorID Fix | Templates are found with `LookupByEditorID`, which needs one of these. Don't read a form's own ID with `GetFormEditorID()`: it returns "" for books without Native EditorID Fix. |
| SkyUI | MCM |
| Address Library (SE/AE) or VR Address Library | The book hook and the `QueueMessage` hook. See [DEVELOPMENT.md](DEVELOPMENT.md#engine-touchpoints). |
| Build: CommonLibSSE-NG v9.1.0 (submodule), vcpkg `sqlite3`, `nlohmann-json`, `spdlog`, `fmt`, `minhook` | |

---

## Related docs

- [BOOK_FORMS.md](BOOK_FORMS.md): DPF, the create queue, templates, actor resolution
- [VOLUMES_AND_SYNC.md](VOLUMES_AND_SYNC.md): entries → volumes, recovery scans, save reverts
- [BOOK_TEXT.md](BOOK_TEXT.md): formatting, sanitizing, the hook
- [THEFT.md](THEFT.md): theft, return and handover
- [DATABASE.md](DATABASE.md): DiaryDB and co-save
- [PAPYRUS_AND_API.md](PAPYRUS_AND_API.md): scripts, natives, inter-plugin API
- [DEVELOPMENT.md](DEVELOPMENT.md): build, deploy, logging, engine touchpoints
