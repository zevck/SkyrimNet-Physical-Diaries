# Developing SNPD

How to build, deploy, debug and verify SNPD, and what not to trip over. The repo has the layout of an MO2 mod folder, but the live install is a separate folder (see [Deploy](#deploy)).

There is **no automated test suite**. Every change is checked in game, through the logs (see [Verifying a change in game](#verifying-a-change-in-game)).

---

## Build

Prerequisites: MSVC x64 with C++23, CMake ≥ 3.21 and vcpkg with the `VCPKG_ROOT` environment variable set (`CMakePresets.json` reads the toolchain file from it).

**CommonLib is the `lib/commonlibsse-ng` submodule** (alandtse CommonLibSSE-NG, `ng` branch, pinned at v9.1.0), built with `add_subdirectory(...)`. Clone with `--recursive`, or run `git submodule update --init --recursive` in an existing clone. The `--recursive` matters: CommonLib has a nested `extern/openvr` submodule, and without it the VR code fails with `Cannot open include file: 'openvr.h'`. The first build compiles all of CommonLib and takes several minutes; later builds are incremental. CommonLib is GPL-3.0, which is why SNPD is GPL-3.0-or-later (`LICENSE.md`).

v9 changes that SNPD relies on (keep them when updating CommonLib again):
- The entry point is `SKSE_PLUGIN_LOAD(...)`; v9 removed the `SKSEAPI` macro.
- `SKSE::Init(a_skse, { .log = false })`. By default v9 installs its own logger on the same `SkyrimNetPhysicalDiaries.log` file, replacing the one `InitializeLog` set up.
- Both engine hooks (`GetDescription`, `QueueMessage`) are MinHook detours installed through `InstallDetour` (`include/Detour.h`). SNPD allocates no SKSE trampoline. (v9's `SKSE::AllocTrampoline` is deprecated and silently allocates nothing without a `TrampolineInterface`, should one ever be needed.)

**Use `Build_Local.ps1`** (repo root, modeled on SkyrimNet's). It builds only the plugin, incrementally; compiles Papyrus with Pyro; and deploys to every configured test instance. It ends with a PASS/FAIL banner, and the same result is written to `%TEMP%\snpd-build-result.json`.

```powershell
.\Build_Local.ps1                 # build + Pyro + deploy to all instances
.\Build_Local.ps1 -noDeploy       # build (+ Pyro) only
.\Build_Local.ps1 -skipScripts    # skip Pyro
.\Build_Local.ps1 -skipSwf        # skip the SWF build
.\Build_Local.ps1 -skipEsp        # skip the ESP build and deploy (e.g. while editing it in the CK)
.\Build_Local.ps1 -fresh          # cmake --fresh reconfigure first
```

Machine-specific settings are in the gitignored `Build_Config_Local.ps1`: `$defaultOutputPath`, `$additionalOutputPaths`, `$ckPath`, an optional `$pyroPath`, `$ffdecPath` (the SWF build), an optional `$spriggitPath` (the ESP build, see [PLUGIN.md](PLUGIN.md)), an optional `$swfVariant` (deploy that SWF variant instead of the vanilla one, e.g. `convenient-reading`), and `$defaultThreads`. The environment variables `SNPD_OUTPUT_PATH` and `SNPD_CK_PATH` override it. An explicit configure runs only on a first build or with `-fresh`; otherwise the VS generator reconfigures itself when `CMakeLists.txt` changes. A no-change run takes about 10 s.

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

For each folder, `Build_Local.ps1` copies the DLL, the built ESP (`build\esp`, never over a copy edited in that folder: see [PLUGIN.md](PLUGIN.md#build)) and `SkyrimNet Physical Diaries_DESC.ini` (to the folder's root, for Description Framework: see [EDITING.md](EDITING.md#quill-and-ink)), and **mirrors** (`robocopy /MIR`) `Scripts`, `Source\Scripts`, `Locales`, `Interface\Translations` and the SkyrimNet plugin `SKSE\Plugins\SkyrimNet\external\zevick.physical-diaries`, so files removed from the repo disappear from the deploy too. `meta.ini` is never touched. If an instance's DLL is locked (its game is running), that instance is reported as failed and the others are still updated.

The Dev folders have the shape of a release install: no `SkyrimNetPhysicalDiaries.ini`, so defaults apply until `Config::Save()` writes one into that instance's `overwrite/`.

`cmake --install build --config Release` copies into the **repo's** `SKSE/Plugins` (`CMAKE_INSTALL_PREFIX` is forced to the repo root), which is only useful for packaging a release. Build outputs (`*.dll`, `*.pex`, `Interface/*.swf`, the `.esp`) are gitignored and not tracked; the ESP's source is `spriggit/` ([PLUGIN.md](PLUGIN.md)).

## Papyrus

Scripts are compiled with **Pyro** from `skyrimse.ppj`, the same way the VS Code papyrus-lang task does it (`.vscode/tasks.json`: "pyro: Compile Project (skyrimse.ppj)", game path = the Creation Kit install). `Build_Local.ps1` runs the extension's `pyro.exe` with `--game-path $ckPath`. Pyro builds incrementally, so "No scripts were compiled." just means nothing changed. The script then **fails the build if any `.psc` has no `.pex`**.

- `skyrimse.ppj` is gitignored and was generated by papyrus-lang's "Generate Project" command. It compiles `Source\Scripts` into `Scripts`, importing the CK's `Data\Source\Scripts` (vanilla, SKSE `StringUtil`, SkyUI `SKI_ConfigBase`, `SkyrimNetApi`).
- **A script that exists only in an import folder compiles fine for anything that uses it, but is never built itself.** Every script SNPD owns must live in `Source/Scripts/`, and its `.pex` must be in `Scripts/`. This is how `SkyrimNetDiaries_API.pex` was missing from every release up to v1.1.0. See [PAPYRUS_AND_API.md](PAPYRUS_AND_API.md#adding-or-changing-a-native-all-four-steps-every-time).
- Release archives must include the built `SkyrimNet Physical Diaries.esp` (`build\esp`), `SkyrimNet Physical Diaries_DESC.ini` (Description Framework's inkwell descriptions, at the root), `Scripts/*.pex`, `Source/Scripts/*.psc`, `Interface/*.swf` (built from `swf/`, see [SWF](#swf)) and `SKSE/Plugins/SkyrimNet/external/zevick.physical-diaries/` (without it NPCs silently never notice thefts). Compare the archive's list with the folders before publishing.

## SWF

SWFs are built with **JPEXS Free Flash Decompiler**'s command-line tool (`ffdec-cli.exe`, path in `$ffdecPath` in `Build_Config_Local.ps1`). Each `swf/<name>/` holds one ActionScript source and one or more base movies:

- `<name>.xml`: the default base, exported with `ffdec-cli -swf2xml`; builds the shipped `Interface/<name>.swf`. Everything but code is readable and diffable here (text fields, sprites, placements). Its scripts are **compiled bytecode** (`actionBytes`), so never edit code in it.
- `<name>.<variant>.xml`: another base, for compatibility with another UI mod (a FOMOD option); builds `build/variants/<variant>/Interface/<name>.swf`. The same scripts go into every base, so the scripts must work on all of them.
- `scripts/`: our ActionScript 2 source, laid out the way `ffdec-cli -export script` writes it (`scripts/__Packages/<Class>.as`). Each file replaces the class of the same name in the base.
- The build: `ffdec-cli -xml2swf` the base → `build/swf/`, then `ffdec-cli -importScript` `swf/<name>/` into it, then `ffdec-cli -decompress` into the output: SWFs ship uncompressed so the plugin can find `book.swf`'s marker as plain text ([EDITING.md](EDITING.md#writing-mode)). The build then fails if an output doesn't start with `FWS` or lacks `BOOKMENU_WRITING_INTERFACE=`. `Build_Local.ps1` rebuilds an output only when its base or a script is newer (`-skipSwf` skips it). The deploy copies `Interface/*.swf`, then the `$swfVariant` variant's SWFs over them if that is set. It copies SWFs even when the game has the DLL locked, so a SWF-only change can be tried in the running game by reopening the book.
- To read what a SWF contains: `ffdec-cli -export script <outFolder> <file.swf>` (decompiled, so no comments, locals renamed `_loc2_`, hex constants in decimal).
- To change a base (not code): edit its XML, or open the built SWF in the JPEXS GUI, make the change, and `ffdec-cli -swf2xml` it back.

`swf/book/` is the book menu (`book.swf`), with SNPD's edit mode in `BookMenu.as`:

- `book.xml`: vanilla `book.swf` (from `Skyrim - Interface.bsa`). The default.
- `book.convenient-reading.xml`: **Convenient Reading**'s `book.swf` by uranreactor, which includes Fhaarkas's **No More Laser-Printed Books** edits (ink-coloured text with a glow and overlay blend, no page numbers). Both grant asset use with credit, so credit both. For users who keep Convenient Reading: its `bookmenu.swf`, animations and sounds are separate files that keep working, since neither SWF loads the other.
- `BookMenu.as` is Convenient Reading's `BookMenu` (which reads `iBookFontSize`/`iNoteFontSize` from `Convenient Reading.ini`) plus the edit mode. Without the ini it keeps the page text field's own size, so on the vanilla base books look vanilla.

Decompiled vanilla, Convenient Reading and `textentrymenu` scripts for reference are kept locally in the gitignored `.resources/action scripts/`.

---

## Logging

`Documents/My Games/Skyrim Special Edition/SKSE/SkyrimNetPhysicalDiaries.log`, **truncated at each launch** (`basic_file_sink_mt(path, true)`), so copy it before restarting if you need it. Level `info`, or `debug` with `[General] DebugLog = 1` (also switchable live from the MCM). Flushed on every `info`-or-higher line.

Levels: `debug` for routine tracing, `info` for state changes worth seeing in a user's log, `warn`/`error` for real problems.

| Prefix | Area |
|---|---|
| `[BookTextHook]` | `Opening diary` at `info` on every diary open; the per-call entry line only with `DebugLog` on |
| `[DynamicForms]` | Book-form records saved and loaded, forms the engine renumbered, world copies rebuilt after a load |
| `[BookForms]` | Books filled in at load, retired books swept (per-copy lines at debug) |
| `[TimelineGate]`, `[Timeline]` | Waiting for SkyrimNet's keep/clear prompt, and reconciling volumes with the history it kept or cleared |
| `[LoadFromDB]`, `[FindActorForBook]` | Matching DiaryDB's volumes against the save's books, and NPC lookup |
| `[Recovery]`, `QueueBatchCatchUpScan`, `DiscoveryBatch`, `CatchUp` | Load-time sync (see [VOLUMES_AND_SYNC.md](VOLUMES_AND_SYNC.md)) |
| `[SNPD]` | `RefreshVolumeOnOpen` re-renders |
| `[BlankJournals]` | At `kDataLoaded`: added to merchants' stock, or the recipes hidden (writing off) |
| `[WritingTools]` | Ink used (the uses an inkwell has left) and inkwells running dry |
| `[WritingMode]` | At `kDataLoaded`: whether player writing is on, and why not |
| `[BookEditor]` | The diary editor: entering and leaving edit mode, saves and tear-outs (and SkyrimNet's answers), held-back closes; plus the diagnostics listed in [EDITING.md](EDITING.md#diagnostics-to-remove) |
| `[Physical Diaries]`, `[Theft Reconciliation]` | Theft |
| `[DiaryDB]`, `[BookManager]`, `[Localization]`, `[PapyrusAPI]` | As named |

Script binding problems (missing `.pex`, missing registration) appear **only in the Papyrus log**, not this one.

SkyrimNet's own log (`SkyrimNet.log`, same folder) holds the `Using save ID:` line SNPD relies on.

## Verifying a change in game

1. Turn on `DebugLog`, deploy, launch.
2. Check startup: `Installed GetDescription hook`, `Diary template books verified (all 4 resolved)`, `SkyrimNet API ready`, `DetectSaveFolderFromLog: detected save folder '…'`, `[DiaryDB] Opened`, `[DynamicForms] Loaded N of N form record(s)`, `[LoadFromDB] Loaded N actors`.
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
| `TESDescription::GetDescription` entry hook | `GetDescriptionHook::Install` (`BookTextHook.cpp`) | `RELOCATION_ID(14399, 14552)`, VR reuses the SE id (VR `0x1A01B0`). MinHook, because other plugins (e.g. Description Framework) hook it too. The book menu's three callers pass `book + 0xA8` with no parent; recheck that after a runtime update (see [BOOK_TEXT.md](BOOK_TEXT.md#delivery-the-getdescription-hook)). |
| `MessageBoxData::QueueMessage` entry hook | `TimelineGate::Install` | `RELOCATION_ID(51422, 52271)`, VR reuses the SE id. A MinHook detour, because SkyrimNet (also MinHook) hooks the same function; MinHook copes with a prologue another plugin has already patched. `IMessageBoxCallback::Run(std::uint8_t)` and `MessageBoxData::{bodyText, callback, buttonPressOffset}` are relied on. |
| `BookMenu::ProcessMessage` vtable hook | `BookEditor::Register` (writing mode only) | `VTABLE_BookMenu[0]`, vfunc 4, the same on SE, AE and VR. Relies on the UI's handling of the result: `kIgnore` on a `kHide` keeps the menu open (and the menu resends the hide every frame), `kHandled` lets the UI remove it; the gamepad's B arrives first as a `kUserEvent` "Cancel" (`BSUIMessageData::fixedStr`) that the menu answers with its close animation. See [EDITING.md](EDITING.md#closing). |
| `BookMenu::AdvanceMovie` vtable hook | `BookEditor::Register` (writing mode only) | `VTABLE_BookMenu[0]`, vfunc 5 (per frame while the book is open): starts a new entry in a volume the editor just opened, once `SetBookText` has run. `BookMenu::OpenMenuFromBaseForm` opens that volume. |
| Book menu input and movie | `BookEditor` (`InputSink`, `BookMovie`) | A sink prepended to `BSInputDeviceManager` (it must run before `MenuControls`); `ButtonEvent::SetUserEvent` to blank keys; the book menu reads its page keys by key code; `ControlMap::AllowTextInput`; `BookMenu::GetRuntimeData().book` (the `book.swf` movie), `BookMenu::GetTargetForm`; `UI::GameIsPaused`; `MessageBoxMenu::QueueMessage` with `cancelButtonIndex`. |
| `BookMenu` globals | `SetBookMenuBook` (`EditorJournals.cpp`) | The book the open menu shows: the base form (IDs 519295 / 405835, VR `0x3011200`) and the inventory item's `ExtraDataList*` (519294 / 405834, VR `0x30111F8`: not in VR's address library, found in the binary 8 bytes before the form's, as on SE; not yet run on VR). Written to turn an open blank journal into a journal ([EDITING.md](EDITING.md#reading-a-blank-journal)). |
| Leveled list in memory | `BlankJournals::AddToMerchants` | Appends to `TESLeveledList::entries` of Skyrim.esm `LItemMiscVendorMiscItems75` (`0x09AF0A`) and updates `numEntries`, which is a `uint8`: a list already at 255 entries is left alone. |
| Recipe workbench | `BlankJournals::OnDataLoaded` | `BGSConstructibleObject::benchKeyword` set to null hides a recipe (writing off). |
| Health | `BledFor` (`BookEditor.cpp`) | `ActorValueOwner::GetPermanentActorValue` / `GetActorValue` / `DamageActorValue(kHealth)` for writing in blood. |
| Inventory items | `WritingTools::UseInk` | `TESObjectREFR::RemoveItem` and `AddObjectToContainer` swap one inkwell for the next; no inventory refresh (each one tried broke the menu: see [EDITING.md](EDITING.md#quill-and-ink)). |
| Event sinks | `DiaryTheftHandler`, `RetiredBookSweeper` (`BookCreation.cpp`), `BookEditor` | `TESContainerChangedEvent`, `MenuOpenCloseEvent`, `TESCellAttachDetachEvent` |
| Retired-book sweep and world copies | `SweepRetiredBooks`, `RebuildLoadedWorldCopies` | `TES::ForEachReference`, `GetInventory(filter, noInit = true)`, `TESFaction::vendorData.merchantContainer`, `TESObjectREFR::Disable`/`Enable`/`SetDelete`/`RemoveItem` |
| Avoided on VR | — | `BSPointerHandle::get()` (`RELOCATION_ID(12785, 12922)`) is missing from the VR Address Library and crashes; use `Actor::LookupByHandle` (12204/12332) if a handle ever needs resolving. `MenuTopicManager::speaker` likewise. |

One DLL serves SE, AE and VR. Don't add a runtime-version gate that turns a feature off; branch on `REL::Module::IsVR()` at the specific call site.

---

## Conventions

- Game state (forms, inventories, references) is touched only on the game thread, via `SKSE::GetTaskInterface()->AddTask`. Papyrus natives run on the game thread (registered non-tasklet; keep it that way). See [ARCHITECTURE.md](ARCHITECTURE.md#threading).
- Wrap SkyrimNet-API and DB work in `try`/`catch`. An exception crossing the SKSE boundary takes the game down.
- Identify diaries by book FormID (`GetBookForFormID`) and NPCs by SkyrimNet UUID. Never by name (names are localized and SkyrimNet shares names) and never by a bare stored FormID.
- User-visible text goes through `Localization`. See [LOCALIZATION.md](LOCALIZATION.md).
- **C++ source files stay under 1000 lines** (the SWF's ActionScript isn't held to it): split one by responsibility when it grows past that (as `BookEditor.cpp` was into `EditorWrites.cpp` and `EditorJournals.cpp`).
- **Comments are at most 2 lines.** An explanation that needs more goes in these docs, and the comment can name the section.
- The root `*.md` notes (`CODE_REVIEW.md`, `RESEARCH_FINDINGS.md`, …) are historical. Check them against the code before trusting them.
- **Watch your editor.** In mid-2026 a VS Code restore saved stale buffers from March over `BookManager.cpp`, `PapyrusAPI.cpp` and `main.cpp`. It silently reverted committed fixes and left `PapyrusAPI.cpp` uncompilable, and nothing showed until `git diff --stat` was far larger than expected. If a diff is bigger than your change, stop and look before building on it.
