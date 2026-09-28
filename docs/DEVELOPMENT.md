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
- `BookTextHook::Install` allocates its trampoline itself (SKSE's branch pool, else `trampoline.create`). v9's `SKSE::AllocTrampoline` is deprecated and silently allocates nothing when SKSE provides no `TrampolineInterface`, which would crash on the hook write.

**Use `Build_Local.ps1`** (repo root, modeled on SkyrimNet's). It builds only the plugin, incrementally; compiles Papyrus with Pyro; and deploys to every configured test instance. It ends with a PASS/FAIL banner, and the same result is written to `%TEMP%\snpd-build-result.json`.

```powershell
.\Build_Local.ps1                 # build + Pyro + deploy to all instances
.\Build_Local.ps1 -noDeploy       # build (+ Pyro) only
.\Build_Local.ps1 -skipScripts    # skip Pyro
.\Build_Local.ps1 -fresh          # cmake --fresh reconfigure first
```

Machine-specific settings are in the gitignored `Build_Config_Local.ps1`: `$defaultOutputPath`, `$additionalOutputPaths`, `$ckPath`, an optional `$pyroPath`, and `$defaultThreads`. The environment variables `SNPD_OUTPUT_PATH` and `SNPD_CK_PATH` override it. An explicit configure runs only on a first build or with `-fresh`; otherwise the VS generator reconfigures itself when `CMakeLists.txt` changes. A no-change run takes about 10 s.

The manual equivalent:

```powershell
cmake --preset vs2022-windows                                   # configure into build/
cmake --build build --config Release --target SkyrimNetPhysicalDiaries
```

| Fact | Where |
|---|---|
| Plugin version is `1.0.0` in both `CMakeLists.txt` and `vcpkg.json`, though v1.1.0 has shipped | Keep them in sync when you bump it |
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

For each folder, `Build_Local.ps1` copies the DLL and ESP, and **mirrors** (`robocopy /MIR`) `Scripts`, `Source\Scripts`, `Locales` and `Interface\Translations`, so files removed from the repo disappear from the deploy too. `meta.ini` is never touched. If an instance's DLL is locked (its game is running), that instance is reported as failed and the others are still updated.

The Dev folders have the shape of a release install: no `SkyrimNetPhysicalDiaries.ini`, so defaults apply until `Config::Save()` writes one into that instance's `overwrite/`.

`cmake --install build --config Release` copies into the **repo's** `SKSE/Plugins` (`CMAKE_INSTALL_PREFIX` is forced to the repo root), which is only useful for packaging a release. Build outputs (`*.dll`, `*.pex`) are gitignored and not tracked; the ESP is tracked.

## Papyrus

Scripts are compiled with **Pyro** from `skyrimse.ppj`, the same way the VS Code papyrus-lang task does it (`.vscode/tasks.json`: "pyro: Compile Project (skyrimse.ppj)", game path = the Creation Kit install). `Build_Local.ps1` runs the extension's `pyro.exe` with `--game-path $ckPath`. Pyro builds incrementally, so "No scripts were compiled." just means nothing changed. The script then **fails the build if any `.psc` has no `.pex`**.

- `skyrimse.ppj` is gitignored and was generated by papyrus-lang's "Generate Project" command. It compiles `Source\Scripts` into `Scripts`, importing the CK's `Data\Source\Scripts` (vanilla, SKSE `StringUtil`, SkyUI `SKI_ConfigBase`, `SkyrimNetApi`).
- **A script that exists only in an import folder compiles fine for anything that uses it, but is never built itself.** Every script SNPD owns must live in `Source/Scripts/`, and its `.pex` must be in `Scripts/`. This is how `SkyrimNetDiaries_API.pex` was missing from every release up to v1.1.0. See [PAPYRUS_AND_API.md](PAPYRUS_AND_API.md#adding-or-changing-a-native-all-four-steps-every-time).
- Release archives must include `Scripts/*.pex` and `Source/Scripts/*.psc`. Compare the archive's list with the folders before publishing.

---

## Logging

`Documents/My Games/Skyrim Special Edition/SKSE/SkyrimNetPhysicalDiaries.log`, **truncated at each launch** (`basic_file_sink_mt(path, true)`), so copy it before restarting if you need it. Level `info`, or `debug` with `[General] DebugLog = 1` (also switchable live from the MCM). Flushed on every `info`-or-higher line.

Levels: `debug` for routine tracing, `info` for state changes worth seeing in a user's log, `warn`/`error` for real problems.

| Prefix | Area |
|---|---|
| `[BookTextHook]` | `Opening diary` at `info` on every diary open; the per-call entry line only with `DebugLog` on |
| `[DPF]` | Creation, FormID collisions, cancelled creations, invalid `sourceFiles` cleared on load (VR) |
| `[TimelineGate]`, `[Timeline]` | Waiting for SkyrimNet's keep/clear prompt, and reconciling volumes with the history it kept or cleared |
| `[LoadFromDB]`, `[EnsureInventory]`, `[FindActorForBook]` | Load-time validation and NPC lookup |
| `[Recovery]`, `QueueBatchCatchUpScan`, `DiscoveryBatch`, `CatchUp` | Load-time sync (see [VOLUMES_AND_SYNC.md](VOLUMES_AND_SYNC.md)) |
| `[SNPD]` | `RefreshVolumeOnOpen` re-renders |
| `[Physical Diaries]`, `[Theft Reconciliation]` | Theft |
| `[DiaryDB]`, `[BookManager]`, `[Localization]`, `[PapyrusAPI]` | As named |

Script binding problems (missing `.pex`, missing registration) appear **only in the Papyrus log**, not this one.

SkyrimNet's own log (`SkyrimNet.log`, same folder) holds the `Using save ID:` line SNPD relies on.

## Verifying a change in game

1. Turn on `DebugLog`, deploy, launch.
2. Check startup: `Installed OpenBookMenu hook`, `Diary template books verified (all 4 resolved)`, `SkyrimNet API ready`, `DetectSaveFolderFromLog: detected save folder '…'`, `[DiaryDB] Opened`, `[LoadFromDB] Loaded N actors`.
3. Make an entry happen (talk to an NPC until SkyrimNet writes a diary entry, or use the SkyrimNet dashboard), then watch for `Queued DPF.Create()` → `DPF created book FormID` → `Added '…' to …'s inventory`.
4. Open the book (from an NPC via pickpocket, or your own diary): `[BookTextHook] Opening diary`.
5. For anything touching persistence, cover **save → reload**, **reload without saving**, **load an older save** (try both SkyrimNet KEEP and CLEAR) and **a second character**. Most past bugs lived there.
6. Test on VR if you touched the hook, form creation or anything that calls `GetDescription`.

---

## Engine touchpoints

Check these whenever CommonLib or the game runtime changes.

| Touchpoint | Where | Notes |
|---|---|---|
| Global form map | `BookManager::SanitizeLoadedBookForms` | `TESForm::GetAllForms()` (same relocation IDs `LookupByID` uses) |
| `BookMenu::OpenBookMenu` entry hook | `BookTextHook::Install` | `RELOCATION_ID(50122, 51053)` = **(SE, AE)**. VR reuses the SE id through the VR Address Library (`RelocationID` sets `_vrID = a_seID`). A hand-written 5-byte `write_branch` detour; the "call original" stub is built by decoding the prologue (`EB`, `E9`, `FF 25`, plain prologues with a REX PUSH/POP check). If the prologue changes shape, this breaks. A detour library would be sturdier. |
| `OpenBookMenu` signature | `OpenBookMenuHook::func_t` | `(const BSString&, const ExtraDataList*, TESObjectREFR*, TESObjectBOOK*, const NiPoint3&, const NiMatrix3&, float, bool)`, plus a ninth `NiAVObject*` **on VR only** (CommonLib doesn't declare it; see [BOOK_TEXT.md](BOOK_TEXT.md#delivery-the-openbookmenu-hook)) |
| `TESObjectBOOK` fields | `ConfigureDiaryForm`, `CompleteCreation`, `ClearBogusSourceFiles` | `data.type`, `data.flags` (`kCantTake`), `inventoryModel`, `itemCardDescription`, `sourceFiles`, `weight`, `value`, `teaches` (never written) |
| Papyrus VM dispatch | `PumpDiaryCreateQueue`, decorator registration | `DispatchStaticCall` + `IStackCallbackFunctor` (`CanSave`, `SetObject`, `operator()`) |
| `MessageBoxData::QueueMessage` entry hook | `TimelineGate::Install` | `RELOCATION_ID(51422, 52271)`, VR reuses the SE id. A MinHook detour, because SkyrimNet (also MinHook) hooks the same function; MinHook copes with a prologue another plugin has already patched. `IMessageBoxCallback::Run(std::uint8_t)` and `MessageBoxData::{bodyText, callback, buttonPressOffset}` are relied on. |
| Event sinks | `DiaryTheftHandler` | `TESContainerChangedEvent`, `MenuOpenCloseEvent` |
| Avoided on VR | — | `BSPointerHandle::get()` (`RELOCATION_ID(12785, 12922)`) is missing from the VR Address Library and crashes; use `Actor::LookupByHandle` (12204/12332) if a handle ever needs resolving. `MenuTopicManager::speaker` likewise. |

One DLL serves SE, AE and VR. Don't add a runtime-version gate that turns a feature off; branch on `REL::Module::IsVR()` at the specific call site.

---

## Conventions

- Game state (forms, inventories, references) is touched only on the game thread, via `SKSE::GetTaskInterface()->AddTask`. The DPF callback arrives on a VM thread; Papyrus natives run on the game thread (registered non-tasklet; keep it that way). See [ARCHITECTURE.md](ARCHITECTURE.md#threading).
- Wrap SkyrimNet-API and DB work in `try`/`catch`. An exception crossing the SKSE boundary takes the game down.
- Identify diaries by book FormID (`GetBookForFormID`) and NPCs by SkyrimNet UUID. Never by name (names are localized and SkyrimNet shares names) and never by a bare stored FormID.
- User-visible text goes through `Localization`. See [LOCALIZATION.md](LOCALIZATION.md).
- The root `*.md` notes (`CODE_REVIEW.md`, `RESEARCH_FINDINGS.md`, …) are historical. Check them against the code before trusting them.
- **Watch your editor.** In mid-2026 a VS Code restore saved stale buffers from March over `BookManager.cpp`, `PapyrusAPI.cpp` and `main.cpp`. It silently reverted committed fixes and left `PapyrusAPI.cpp` uncompilable, and nothing showed until `git diff --stat` was far larger than expected. If a diff is bigger than your change, stop and look before building on it.
