# SkyrimNet Physical Diaries Architecture

SkyrimNet Physical Diaries (SNPD) is an SKSE plugin (`SkyrimNetPhysicalDiaries.dll`) plus five small Papyrus scripts and an ESP. It turns the diary entries that [SkyrimNet](https://github.com/MinLL/SkyrimNet-GamePlugin) writes for NPCs into real book items. SkyrimNet already stores those entries, but only as AI context and dashboard text. SNPD reads them through SkyrimNet's public API, splits them into volumes, gives each volume a book form, puts the book in the NPC's inventory, and supplies the text when the player opens it.

**SNPD is a presentation and interaction layer over SkyrimNet's data.** It never writes diary content and never changes SkyrimNet's entries. It owns only the book forms and its own SQLite state. Any design that puts authoritative diary content in SNPD's storage is wrong.

This document gives the map. Follow the links in [Related docs](#related-docs) for detail.

---

## Repo layout

The repo has the layout of an MO2 mod folder. Builds are deployed as a `Physical Diaries - Dev` mod folder into three test instances: MO2 (AE), FUS (VR) and Nolvus (SE). See [DEVELOPMENT.md](DEVELOPMENT.md#deploy).

| Path | What lives there |
|---|---|
| `src/` | C++ sources |
| `include/` | Headers, one per component, plus `PCH.h` (force-included: `RE/Skyrim.h`, `SKSE/SKSE.h`, spdlog), `SkyrimNetPublicAPI.h` (vendored SkyrimNet API loader) and `SkyrimNetPhysicalDiariesAPI.h` (SNPD's own inter-plugin API, for other mods) |
| `.resources/CommonLibVR-4.5.0/` | Vendored CommonLibSSE-NG 4.5.0. **Gitignored** (`/.resources`), so a fresh clone cannot build. See [DEVELOPMENT.md](DEVELOPMENT.md#build). |
| `Source/Scripts/*.psc` → `Scripts/*.pex` | Papyrus: `SkyrimNetDiaries_API`, `_Decorators`, `_EventListener`, `_MCM`, `_Native`. See [PAPYRUS_AND_API.md](PAPYRUS_AND_API.md). |
| `SkyrimNet Physical Diaries.esp` | The four template books (`SkyrimNetDiaryTemplate`, `…2`, `…3`, `…N`), the MCM quest and the event-listener quest. Also `SNPD_DiaryStolenFaction`, which nothing uses any more (see [KNOWN_ISSUES.md](KNOWN_ISSUES.md)). |
| `SKSE/Plugins/SkyrimNetPhysicalDiaries.dll` | Deployed DLL. Tracked in git even though `.gitignore` lists `*.dll` (it was committed before the rule). |
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
 SkyrimNetDiaries_EventListener.psc ──► SkyrimNetDiaries_Native.UpdateDiaryForActor(formId)
        │                                         (PapyrusAPI.cpp)
        ▼
 main.cpp  UpdateDiaryForActorInternal ──► Database.cpp ──► SkyrimNet public API (JSON)
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
| Entry point, lifecycle | `src/main.cpp` | `SKSEPlugin_Load`, SKSE message handler (`OnMessage`), co-save callbacks, save-folder detection, entry → volume logic, text formatting, catch-up and recovery scans, MCM reset, inter-plugin API responses |
| Book forms and volumes | `src/BookManager.cpp`, `include/BookManager.h` | DPF creation queue and FormID claim table, template choice, actor resolution, inventory placement, the in-memory volume registry (`books_`), load from DB, refresh on open. See [BOOK_FORMS.md](BOOK_FORMS.md). |
| Persistence | `src/DiaryDB.cpp`, `include/DiaryDB.h` | Per-save SQLite: volumes, actor templates, stolen volumes. See [DATABASE.md](DATABASE.md). |
| SkyrimNet client | `src/Database.cpp`, `include/Database.h`, `include/SkyrimNetPublicAPI.h` | Loads SkyrimNet's exported functions, parses diary JSON, UUID ↔ FormID, names, bio template names |
| Text injection | `src/BookTextHook.cpp`, `include/BookTextHook.h` | Hook on `BookMenu::OpenBookMenu`: substitutes diary text, VR world-open workaround, SEH guard, UTF-8 → Win-1251. See [BOOK_TEXT.md](BOOK_TEXT.md). |
| Theft | `src/DiaryTheftHandler.cpp`, `include/DiaryTheftHandler.h` | Container-change and menu sinks that record theft, returns and willing handovers. See [THEFT.md](THEFT.md). |
| Papyrus natives | `src/PapyrusAPI.cpp`, `include/PapyrusAPI.h` | MCM getters and setters, the theft API, `UpdateDiaryForActor` |
| Localization | `src/Localization.cpp`, `include/Localization.h` | Language detection, locale `.ini`, GMST month and day names, title and date formats. See [LOCALIZATION.md](LOCALIZATION.md). |
| Settings | `include/Config.h` (header-only) | INI load and save. See [CONFIG_AND_MCM.md](CONFIG_AND_MCM.md). |

---

## Startup and load sequence

**`SKSEPlugin_Load`** (`main.cpp`), in order:
1. `InitializeLog()`, then `SKSE::Init`.
2. `Config::Load()` followed at once by `Config::Save()`, so MO2 copies the INI into `overwrite/` and user settings survive mod updates. Debug level is applied from `[General] DebugLog`.
3. Register `OnMessage`; register co-save callbacks under the unique ID `'SNDB'`.
4. `BookManager::Initialize(...)` with the four template EditorIDs.
5. `DiaryTheftHandler::Register()` (event sinks), `Localization::Initialize()`, `BookTextHook::Install()`, `PapyrusAPI::Register()`.

**`kDataLoaded`**: warn with a message box if `Dynamic Persistent Forms.esp` is missing; verify all four templates resolve by EditorID and show a message box naming the likely causes if not; `Localization::ReadGMSTs()`.

**`kPostLoadGame`**:
1. `BookManager::SanitizeLoadedBookForms()`: clear the invalid `sourceFiles` pointer DPF leaves on some clones (VR). Runs first and does not depend on SkyrimNet.
2. `Database::InitializeAPI()`. If SkyrimNet is not loaded, stop here.
3. Detect the save folder from `SkyrimNet.log` and `DiaryDB::Open()` it. **This happens before the readiness wait below**, which is a known risk (see [KNOWN_ISSUES.md](KNOWN_ISSUES.md)).
4. `BookManager::ClearActorCache()` (also clears the FormID claim table).
5. A self-requeuing task polls `Database::IsMemorySystemReady()` for up to 300 game-thread ticks. When it is ready:
   - Re-register the `snpd_diary_stolen` decorator through the Papyrus VM. SkyrimNet drops all decorator registrations on load, and the Papyrus `OnInit` that also registers it runs only on a new game.
   - `LoadFromDB()`, then `QueueInventoryCheck()`.
   - Theft reconciliation: clear stolen volumes for any actor whose `last_known_game_time` is later than the current game time (the player loaded an earlier save).
   - Queue immediate recreation for actors whose book forms were invalid, then `QueueSealedVolumeRecovery()` and `QueueBatchCatchUpScan()`.

**Save**: the co-save `SaveCallback` opens the DB if a new game never got a `kPostLoadGame`, runs `FlushToDB()`, stamps `last_known_game_time`, and writes the `SNDB` sentinel and `SNDF` folder records. `kSaveGame` then marks every volume `persisted_in_save`. See [DATABASE.md](DATABASE.md#co-save-records).

**New game or load**: `RevertCallback` clears in-memory state. `diary.db` on disk is kept on purpose.

---

## One diary entry, end to end

1. SkyrimNet generates an entry. The `snpd_diary_stolen` decorator is evaluated **during** generation, so an NPC whose diary is missing writes about it.
2. SkyrimNet sends ModEvent `SkyrimNet_DiaryCreated`. `SkyrimNetDiaries_EventListener.psc` pulls `actorFormId` out of the JSON with string search and calls `SkyrimNetDiaries_Native.UpdateDiaryForActor`.
3. `UpdateDiaryForActorWrapper` (`PapyrusAPI.cpp`) clears that actor's stolen volumes (the theft has now been written about) and calls `UpdateDiaryForActorInternal` (`main.cpp`).
4. `UpdateDiaryForActorInternal` fetches entries and takes one of three paths: create every volume (no volumes yet), start a new volume (the NPC no longer holds the latest one), or update or seal the latest volume in place. See [VOLUMES_AND_SYNC.md](VOLUMES_AND_SYNC.md).
5. New volumes go through `BookManager::CreateDiaryBook` → the serial create queue → `DPF.Create()` → `DPFCreateCallback`, which claims the FormID, configures the form from its template, registers the volume, writes the rendered text to DiaryDB and adds the book to the NPC. See [BOOK_FORMS.md](BOOK_FORMS.md).
6. The player opens the book. `BookTextHook` finds the volume by FormID, calls `RefreshVolumeOnOpen` (which catches entries SkyrimNet added or deleted since the last render), converts the text and passes it to the engine in place of the book's own description. See [BOOK_TEXT.md](BOOK_TEXT.md).

---

## Threading

| Context | What runs there |
|---|---|
| Game (main) thread | SKSE messages, every `AddTask` body, the book hook, event sinks |
| Papyrus VM thread | Native functions, and the `DPF.Create()` callback (`DPFCreateCallback::operator()`) |
| Detached `std::thread` | One per created book: sleeps 5 s, then queues the `kCantTake` clear |

Rules: anything touching forms, inventories or references must run on the game thread (`SKSE::GetTaskInterface()->AddTask`). The DPF callback does only the FormID claim itself and hands the rest to a task. The create queue, the claim table and the actor cache each have their own mutex. `books_` has none and is only touched from the game thread, so keep it that way.

---

## Dependencies

| Dependency | Why |
|---|---|
| SkyrimNet | The source of all diary content. SNPD resolves its exports from SkyrimNet's DLL at runtime (`SkyrimNetPublicAPI.h`), checks `PublicGetVersion`, and does nothing if they are missing. |
| Dynamic Persistent Forms | Creates the runtime book forms. Called only through the Papyrus VM (`DispatchStaticCall("DynamicPersistentForms", "Create", …)`). See [BOOK_FORMS.md](BOOK_FORMS.md#alternatives-evaluated). |
| powerofthree's Tweaks **or** Native EditorID Fix | Templates are found with `LookupByEditorID`, which needs one of these. Don't read a form's own ID with `GetFormEditorID()`: it returns "" for books without Native EditorID Fix. |
| SkyUI | MCM |
| Address Library (SE/AE) or VR Address Library | The book hook. See [DEVELOPMENT.md](DEVELOPMENT.md#engine-touchpoints). |
| Build: CommonLibSSE-NG 4.5.0 (vendored), vcpkg `sqlite3`, `nlohmann-json`, `spdlog`, `fmt` | |

---

## Related docs

- [BOOK_FORMS.md](BOOK_FORMS.md): DPF, the create queue, templates, actor resolution
- [VOLUMES_AND_SYNC.md](VOLUMES_AND_SYNC.md): entries → volumes, recovery scans, save reverts
- [BOOK_TEXT.md](BOOK_TEXT.md): formatting, sanitizing, the hook
- [THEFT.md](THEFT.md): theft, return and handover
- [DATABASE.md](DATABASE.md): DiaryDB and co-save
- [PAPYRUS_AND_API.md](PAPYRUS_AND_API.md): scripts, natives, inter-plugin API
- [DEVELOPMENT.md](DEVELOPMENT.md): build, deploy, logging, engine touchpoints
