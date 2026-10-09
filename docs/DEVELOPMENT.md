# Developing SNPD

How to build, deploy, debug and verify SNPD, and what not to trip over. The repo has the layout of an MO2 mod folder, but the live install is a separate folder (see [Deploy](#deploy)).

There is **no automated test suite**. Every change is checked in game, through the logs (see [Verifying a change in game](#verifying-a-change-in-game)).

---

## Build

Prerequisites: MSVC x64 with C++23, CMake ≥ 3.21 and vcpkg with the `VCPKG_ROOT` environment variable set (`CMakePresets.json` reads the toolchain file from it).

**CommonLib is the `lib/commonlibsse-ng` submodule** (alandtse CommonLibSSE-NG, `ng` branch, pinned at v9.1.0), built with `add_subdirectory(...)`. Clone with `--recursive`, or run `git submodule update --init --recursive` in an existing clone. The `--recursive` matters: CommonLib has a nested `extern/openvr` submodule, and without it the VR code fails with `Cannot open include file: 'openvr.h'`. The first build compiles all of CommonLib and takes several minutes; later builds are incremental. CommonLib is GPL-3.0, which is why SNPD is GPL-3.0-or-later (`LICENSE`: no extension, so GitHub shows it as plain text and recognises the license; as `LICENSE.md` it was rendered as Markdown and reflowed).

v9 changes that SNPD relies on (keep them when updating CommonLib again):
- The entry point is `SKSE_PLUGIN_LOAD(...)`; v9 removed the `SKSEAPI` macro.
- `SKSE::Init(a_skse, { .log = false })`. By default v9 installs its own logger on the same `PhysicalDiaries.log` file, replacing the one `InitializeLog` set up.
- The two MinHook detours (`GetDescription`, `OpenBookMenu`) are installed through `InstallDetour` (`include/Detour.h`). SNPD allocates no SKSE trampoline. (v9's `SKSE::AllocTrampoline` is deprecated and silently allocates nothing without a `TrampolineInterface`, should one ever be needed.)

**Use `Build_Local.ps1`** (repo root, modeled on SkyrimNet's). It builds only the plugin, incrementally; compiles Papyrus with Pyro; and deploys to every configured test instance. It ends with a PASS/FAIL banner, and the same result is written to `%TEMP%\snpd-build-result.json`.

```powershell
.\Build_Local.ps1                 # build + Pyro + deploy to all instances
.\Build_Local.ps1 -noDeploy       # build (+ Pyro) only
.\Build_Local.ps1 -skipScripts    # skip Pyro
.\Build_Local.ps1 -skipEsp        # skip the ESP build and deploy (e.g. while editing it in the CK)
.\Build_Local.ps1 -fresh          # cmake --fresh reconfigure first
```

Machine-specific settings are in the gitignored `Build_Config_Local.ps1`: `$defaultOutputPath`, `$additionalOutputPaths`, `$ckPath`, an optional `$pyroPath`, an optional `$spriggitPath` (the ESP build, see [PLUGIN.md](PLUGIN.md)), and `$defaultThreads`. The environment variables `SNPD_OUTPUT_PATH` and `SNPD_CK_PATH` override it. An explicit configure runs only on a first build or with `-fresh`; otherwise the VS generator reconfigures itself when `CMakeLists.txt` changes. A no-change run takes about 10 s.

The manual equivalent:

```powershell
cmake --preset vs2022-windows                                   # configure into build/
cmake --build build --config Release --target SkyrimNetPhysicalDiaries
```

| Fact | Where |
|---|---|
| Plugin version is in both `CMakeLists.txt` and `vcpkg.json` (2.0.0 as of 2026-09-28) | Keep them in sync when you bump it |
| vcpkg triplet forced to `x64-windows-static`; static MSVC runtime | `CMakeLists.txt` top |
| vcpkg deps: `sqlite3`, `nlohmann-json`, `spdlog`, `fmt`, `minhook` (+ CommonLib's) | `vcpkg.json` |
| Plugin declared with `add_commonlibsse_plugin(... USE_ADDRESS_LIBRARY ...)`, which also generates `SKSEPlugin_Version`/`Query`. Don't declare them by hand. | `CMakeLists.txt`, bottom of `main.cpp` |
| PCH `include/PCH.h` is force-included in every source file. Don't add explicit `RE/…` or `SKSE/…` includes. | `target_precompile_headers` |
| **New `.cpp` files must be added to `sources` by hand**; there is no glob. Re-run the configure step afterwards. | `CMakeLists.txt` |
| The `vs2022-windows` preset uses generator `"Visual Studio 18"` despite its name. There is also `ninja-release`. | `CMakePresets.json` |

Builds are incremental and fast. **Don't pass `/t:Rebuild`**: MSBuild's Rebuild also cleans and rebuilds the referenced CommonLib project (hundreds of files, minutes). To force our own files to recompile, touch them.

## Deploy

The build deploys to a **`Physical Diaries - Dev`** mod folder in each test instance, following SkyrimNet's `SkyrimNet - Dev` convention. Use one MO2 instance per runtime (SE, AE, VR) so a change can be checked on all three. The folders are `$defaultOutputPath` and `$additionalOutputPaths` in `Build_Config_Local.ps1`.

Enable the Dev folder in the MO2 profile you test with, and disable any other Physical Diaries mod in that profile. Don't point a deploy path at the repo itself.

For each folder, `Build_Local.ps1` copies the DLL, the built ESP (`build\esp`, never over a copy edited in that folder: see [PLUGIN.md](PLUGIN.md#build)), and **mirrors** (`robocopy /MIR`) `Scripts`, `Source\Scripts`, `Locales`, `Interface\Translations` and the SkyrimNet plugin `SKSE\Plugins\SkyrimNet\external\zevick.physical-diaries`, so files removed from the repo disappear from the deploy too. `meta.ini` is never touched. If an instance's DLL is locked (its game is running), that instance is reported as failed and the others are still updated.

The Dev folders have the shape of a release install: no `PhysicalDiaries.ini`, so defaults apply until `Config::Save()` writes one into that instance's `overwrite/`.

`cmake --install build --config Release` copies into the **repo's** `SKSE/Plugins` (`CMAKE_INSTALL_PREFIX` is forced to the repo root), which is only useful for packaging a release. Build outputs (`*.dll`, `*.pex`, the `.esp`) are gitignored and not tracked; the ESP's source is `spriggit/` ([PLUGIN.md](PLUGIN.md)).

## Papyrus

Scripts are compiled with **Pyro** from `skyrimse.ppj`, the same way the VS Code papyrus-lang task does it (`.vscode/tasks.json`: "pyro: Compile Project (skyrimse.ppj)", game path = the Creation Kit install). `Build_Local.ps1` runs the extension's `pyro.exe` with `--game-path $ckPath`. Pyro builds incrementally, so "No scripts were compiled." just means nothing changed. The script then **fails the build if any `.psc` has no `.pex`**.

- `skyrimse.ppj` is gitignored and was generated by papyrus-lang's "Generate Project" command. It compiles `Source\Scripts` into `Scripts`, importing the CK's `Data\Source\Scripts` (vanilla, SKSE `StringUtil`, SkyUI `SKI_ConfigBase`, `SkyrimNetApi`).
- **A script that exists only in an import folder compiles fine for anything that uses it, but is never built itself.** Every script SNPD owns must live in `Source/Scripts/`, and its `.pex` must be in `Scripts/`. This is how `SkyrimNetDiaries_API.pex` was missing from every release up to v1.1.0. See [PAPYRUS_AND_API.md](PAPYRUS_AND_API.md#adding-or-changing-a-native-all-four-steps-every-time).
- Release archives must include each script's `.pex` with its `.psc`, and `SKSE/Plugins/SkyrimNet/external/zevick.physical-diaries/` (without it NPCs silently never notice thefts): `Build_Release.ps1` stages and checks both ([Releases](#releases)).

## Releases

`.\Build_Release.ps1` makes the archive players install, `build\release\Physical Diaries <version>.zip` (the version from `CMakeLists.txt`'s `project(… VERSION …)`), in the game's `Data` layout: there are no install options, so no FOMOD (Ink & Quill's release has one, for its two `book.swf` variants).

1. It refuses uncommitted changes (a release is a commit); `-allowDirty` makes a test release, marked `-dirty`.
2. It builds with `Build_Local.ps1 -noDeploy` (plugin, Papyrus, ESP); `-skipBuild` packs the last build as it is.
3. It stages `build\release\stage`: `SKSE/Plugins/PhysicalDiaries.dll`, `Physical Diaries.esp`, each `Source/Scripts` script's `.pex` (not the whole folder: a leftover `.pex` under an old name must not ship) and its `.psc`, `Interface/Translations`, `SKSE/Plugins/PhysicalDiaries/Locales` and the SkyrimNet plugin `SKSE/Plugins/SkyrimNet/external/zevick.physical-diaries`.
4. It checks the stage: every MCM translation UTF-16 LE with a BOM, named after the ESP and with English's keys; every `$SNPD_` key the MCM scripts use in English (a key with an argument, `"$KEY{" + value + "}"`, as `$KEY{}`); every locale `.ini` with English's `[section]` keys; the SkyrimNet manifest naming the ESP; no file under 1.x's names.
5. It zips the stage. The outcome is printed and written to `%TEMP%\snpd-release-result.json`.

## Logging

`Documents/My Games/Skyrim Special Edition/SKSE/PhysicalDiaries.log`, **truncated at each launch** (`basic_file_sink_mt(path, true)`), so copy it before restarting if you need it. Level `info`, or `debug` with `[General] DebugLog = 1` (also switchable live from the MCM). Flushed on every `info`-or-higher line.

Levels: `debug` for routine tracing, `info` for state changes worth seeing in a user's log, `warn`/`error` for real problems.

| Prefix | Area |
|---|---|
| `[BookTextHook]` | `Opening diary` at `info` on every diary open; the per-call entry line only with `DebugLog` on |
| `[DynamicForms]` | Book-form records saved and loaded, forms the engine renumbered, world copies rebuilt after a load |
| `[BookForms]` | Books filled in at load, retired books swept (per-copy lines at debug) |
| `[TimelineGate]`, `[Timeline]` | Waiting for SkyrimNet's keep/clear check, and reconciling volumes with the history it kept or cleared |
| `[LoadFromDB]`, `[FindActorForBook]` | Matching DiaryDB's volumes against the save's books, and NPC lookup |
| `[Recovery]`, `QueueBatchCatchUpScan`, `DiscoveryBatch`, `CatchUp` | Load-time sync (see [VOLUMES_AND_SYNC.md](VOLUMES_AND_SYNC.md)) |
| `[Restore]` | At load: a journal with entries in SkyrimNet but no book in this save, made again |
| `[LegacyFiles]` | 1.x's INI (at startup) and a save's DiaryDB (when it's loaded) copied to the new names ([ARCHITECTURE.md](ARCHITECTURE.md#names)) |
| `[Migrate]` | Once per DiaryDB, at load: diary volumes re-cut by SkyrimNet entry id ([DATABASE.md](DATABASE.md#schema-changes)) |
| `[Regen]` | MCM Regenerate: each volume re-rendered |
| `[DiaryTheftHandler]` | Theft and return detection, the stolen-diary decorator |
| `[InterPluginAPI]` | Requests from other plugins |
| `[SNPD]` | `RefreshVolumeOnOpen` re-renders |
| `[BlankJournals]` | At `kDataLoaded`: added to merchants' stock, or the recipes hidden (writing off) |
| `[WritingMode]` | At `kPostLoad`: whether Ink & Quill is there (and new enough) |
| `[NpcDiaries]` | At `kDataLoaded`: ready (on/off). Each day's run: the newest event against the game's time and the milliseconds spent asking SkyrimNet (`newest event …, now … (… ms asking SkyrimNet)`), a summary (quiet, not loaded, filtered, wrote today, written up, candidates, daily writers and how many of them wrote today) and each pick with its score; with Debug Logging, each actor's last event, window start and activity; `Days A-B: a time skip passed the writing hour` (sleep, wait, fast travel, carriages); `Day N: skipped, SkyrimNet's AI is off` (or diaries); SkyrimNet diary-config changes from the MCM; daily writers starting and stopping. Every actor's numbers at debug ([NPC_DIARIES.md](NPC_DIARIES.md)) |
| `[BookEditor]` | Journals as Ink & Quill's client: sessions begun, saves and tear-outs (and SkyrimNet's answers), new entries, blank journals' sessions. The editor's own lines (edit mode, ink, blood, `Marked text: N runs; layout …`) are in `InkAndQuill.log` |
| `[Physical Diaries]`, `[Theft Reconciliation]` | Theft |
| `[DiaryDB]`, `[BookManager]`, `[Localization]`, `[PapyrusAPI]` | As named |

Script binding problems (missing `.pex`, missing registration) appear **only in the Papyrus log**, not this one.

SkyrimNet's own log (`SkyrimNet.log`, same folder) holds the `Using save ID:` line SNPD relies on.

## Verifying a change in game

1. Turn on `DebugLog`, deploy, launch.
2. Check startup: `Installed GetDescription hook`, `Installed OpenBookMenu hook`, `Diary template books verified (all 4 resolved)`, `SkyrimNet API ready`, `DetectSaveFolderFromLog: detected save folder '…'`, `[DiaryDB] Opened`, `[DynamicForms] Loaded N of N form record(s)`, `[LoadFromDB] Loaded N actors`.
3. Make an entry happen (talk to an NPC until SkyrimNet writes a diary entry, or use the SkyrimNet dashboard), then watch for `Added '…' (0xFF…) to …'s inventory`.
4. Open the book (from an NPC via pickpocket, or your own diary): `[BookTextHook] Opening diary`.
5. For anything touching persistence, cover **save → reload**, **reload without saving**, **load an older save** (try both SkyrimNet KEEP and CLEAR) and **a second character**. Most past bugs lived there.
6. Test on VR if you touched the hook, form creation, the co-save record or anything that calls `GetDescription`.

---

## Engine touchpoints

Check these whenever CommonLib or the game runtime changes.

| Touchpoint | Where | Notes |
|---|---|---|
| Form factory | `DynamicForms::Create` | `IFormFactory::GetConcreteFormFactoryByType<T>()->Create()`, then `formFlags` (`kTemporary` cleared) and `AddChange(kFlags)` |
| Created-reference load | (no code: a constraint) | A world copy keeps its base's raw FormID and the builder (AE `0x14060EE40`) only checks it is some bound object, so SNPD never removes a book form from the save. Recheck after a runtime update; see [BOOK_FORMS.md](BOOK_FORMS.md#the-engine-behaviour-this-rests-on) fact 8. |
| Save/load behaviour | `DynamicForms`, `LoadFromDB` | The engine facts in [BOOK_FORMS.md](BOOK_FORMS.md#the-engine-behaviour-this-rests-on) (IDs listed there): recheck them after a runtime update |
| `TESObjectBOOK` fields | `ConfigureDiaryForm` | `data.type`, `data.flags`, `inventoryModel`, `itemCardDescription`, world model (`SetModel`), `boundData`, `pickupSound`, `putdownSound`, keywords, `weight`, `value`; `teaches` is never written |
| `TESDescription::GetDescription` entry hook | `GetDescriptionHook::Install` (`BookTextHook.cpp`) | `RELOCATION_ID(14399, 14552)`, VR reuses the SE id (VR `0x1A01B0`). MinHook, because other plugins (e.g. Description Framework) hook it too. The book menu's three callers pass `book + 0xA8` with no parent; recheck that after a runtime update (see [BOOK_TEXT.md](BOOK_TEXT.md#delivery-the-getdescription-and-openbookmenu-hooks)). |
| `BookMenu::OpenBookMenu` entry hook | `OpenBookMenuHook::Install` (`BookTextHook.cpp`) | `RELOCATION_ID(50122, 51053)`, VR reuses the SE id. MinHook (Physical Letters hooks it too). Nine arguments on every runtime: VR has an extra `NiAVObject*` that must be forwarded ([BOOK_TEXT.md](BOOK_TEXT.md#delivery-the-getdescription-and-openbookmenu-hooks)). |
| Book menu input | `BookEditor` (`InputSink`) | A sink prepended to `BSInputDeviceManager` (it must run before `MenuControls`) for the tear-out and new-entry keys; `ButtonEvent::SetUserEvent` to blank them; `UI::IsMenuOpen` (both keys act only in the book menu); `BookMenu::GetTargetForm`. The rest of the book menu's input is Ink & Quill's. |
| Leveled list in memory | `BlankJournals::AddToMerchants` | Appends to `TESLeveledList::entries` of Skyrim.esm `LItemMiscVendorMiscItems75` (`0x09AF0A`) and updates `numEntries`, which is a `uint8`: a list already at 255 entries is left alone. |
| Recipe workbench | `BlankJournals::OnDataLoaded` | `BGSConstructibleObject::benchKeyword` set to null hides a recipe (writing off). |
| Player flags | `NpcDiaries` (`MenuSink`) | `PlayerCharacter::GetPlayerFlags().fastTraveling`, read as the loading screen opens: set for fast travel and carriages, not doors (logged on AE 2026-10-01) |
| Event sinks | `DiaryTheftHandler`, `RetiredBookSweeper` (`BookCreation.cpp`), `NpcDiaries` (`MenuOpenCloseEvent`: Sleep/Wait, loading screens) | `TESContainerChangedEvent`, `MenuOpenCloseEvent`, `TESCellAttachDetachEvent` |
| Retired-book sweep and world copies | `SweepRetiredBooks`, `RebuildLoadedWorldCopies` | `TES::ForEachReference`, `GetInventory(filter, noInit = true)`, `TESFaction::vendorData.merchantContainer`, `TESObjectREFR::Disable`/`Enable`/`SetDelete`/`RemoveItem` |
| Avoided on VR | — | `BSPointerHandle::get()` (`RELOCATION_ID(12785, 12922)`) is missing from the VR Address Library and crashes; use `Actor::LookupByHandle` (12204/12332) if a handle ever needs resolving. `MenuTopicManager::speaker` likewise. |

One DLL serves SE, AE and VR. Don't add a runtime-version gate that turns a feature off; branch on `REL::Module::IsVR()` at the specific call site.

---

## Conventions

- Game state (forms, inventories, references) is touched only on the game thread, via `SKSE::GetTaskInterface()->AddTask`. Papyrus natives run on the game thread (registered non-tasklet; keep it that way). See [ARCHITECTURE.md](ARCHITECTURE.md#threading).
- Wrap SkyrimNet-API and DB work in `try`/`catch`. An exception crossing the SKSE boundary takes the game down.
- Identify diaries by book FormID (`GetBookForFormID`) and NPCs by SkyrimNet UUID. Never by name (names are localized and SkyrimNet shares names) and never by a bare stored FormID.
- User-visible text goes through `Localization`. See [LOCALIZATION.md](LOCALIZATION.md).
- **C++ source files stay under 1000 lines** : split one by responsibility when it grows past that (as `BookEditor.cpp` was into `EditorWrites.cpp` and `EditorJournals.cpp`).
- **Comments are at most 2 lines.** An explanation that needs more goes in these docs, and the comment can name the section.
- The root `*.md` notes (`CODE_REVIEW.md`, `RESEARCH_FINDINGS.md`, …) are historical. Check them against the code before trusting them.
- **Watch your editor.** In mid-2026 a VS Code restore saved stale buffers from March over `BookManager.cpp`, `PapyrusAPI.cpp` and `main.cpp`. It silently reverted committed fixes and left `PapyrusAPI.cpp` uncompilable, and nothing showed until `git diff --stat` was far larger than expected. If a diff is bigger than your change, stop and look before building on it.
