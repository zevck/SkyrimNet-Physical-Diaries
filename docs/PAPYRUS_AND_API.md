# Papyrus Scripts and APIs

SNPD's Papyrus surface (scripts and native functions), the theft API other mods can call, and the SKSE-message API other plugins can use to read diary text.

Code: `Source/Scripts/*.psc` → `Scripts/*.pex`, `src/PapyrusAPI.cpp`, `include/SkyrimNetPhysicalDiariesAPI.h`, the `SNPD_QUERY_*` cases in `OnMessage` (`src/main.cpp`).

---

## Scripts

| Script | Attached to | Role |
|---|---|---|
| `SkyrimNetDiaries_EventListener` | Quest in the ESP (`extends Quest`) | `OnInit`: registers for ModEvent `SkyrimNet_DiaryCreated` and registers the `snpd_diary_stolen` decorator (new game only; C++ re-registers it on every load). `OnDiaryCreated`: pulls `actorFormId` out of the JSON with `StringUtil` and calls `SkyrimNetDiaries_Native.UpdateDiaryForActor`. |
| `SkyrimNetDiaries_Decorators` | — (global functions) | `IsDiaryStolen(Actor)`: the function the decorator points at; forwards to the native |
| `SkyrimNetDiaries_API` | — (native declarations) | Public theft API (below) |
| `SkyrimNetDiaries_Native` | — (native declarations) | `UpdateDiaryForActor(int formId)` |
| `SkyrimNetDiaries_MCM` | MCM quest (`SKI_ConfigBase`) | Settings and Maintenance pages. See [CONFIG_AND_MCM.md](CONFIG_AND_MCM.md). |

## Native functions

All registered in `PapyrusAPI::RegisterFunctions`.

| Papyrus class.function | C++ | Called from |
|---|---|---|
| `SkyrimNetDiaries_Native.UpdateDiaryForActor(int)` | `UpdateDiaryForActorWrapper`: clears stolen volumes, then `UpdateDiaryForActorInternal` | EventListener |
| `SkyrimNetDiaries_API.IsDiaryStolen(Actor) → String` | `"true"` / `"false"` | Decorators |
| `SkyrimNetDiaries_API.GetDiaryTheftStatus(Actor) → String` | JSON; see [THEFT.md](THEFT.md) | Public API only |
| `SkyrimNetDiaries_API.SetTheftCleared(Actor)` | Clears stolen volumes | Public API only |
| `SkyrimNetDiaries_MCM.*` (16 getters and setters, `RegenerateTextsOnly`, `ResetAllDiaries`) | `MCM_*` | MCM |

`GetDiaryTheftStatus` and `SetTheftCleared` have no callers inside SNPD. They are public API for other mods: keep them.

### Adding or changing a native (all four steps, every time)

1. Write the C++ function and register it in `RegisterFunctions` under the right class name.
2. Declare it `Native` in the matching `.psc`.
3. **Compile the `.pex`** and make sure it lands in `Scripts/` (see [DEVELOPMENT.md](DEVELOPMENT.md#papyrus)).
4. **Ship both `.psc` and `.pex`** (check the release archive).

If any step is skipped, the build is still clean and the failure appears only at runtime, as Papyrus log errors ("Cannot open store for class…", "could find no matching static function…") with the call returning None. That is how the theft feature went unnoticed as broken from v1.0.0 until 2026-09-26: `SkyrimNetDiaries_API.psc` lived in a folder the compiler searched for imports but the project never built from, so `Decorators` compiled and `SkyrimNetDiaries_API.pex` was never produced. Removing a native is the same in reverse: drop the registration **and** the declaration, and recompile.

## Inter-plugin API (SKSE messaging)

For other SKSE plugins, such as TTS or reading mods, that want a diary's text. Declared in `include/SkyrimNetPhysicalDiariesAPI.h` (`SNPD_API_VERSION = 3`). All three queries are synchronous: the struct is filled in before `Dispatch` returns.

| Message | Struct | Returns |
|---|---|---|
| `'SNPD'` `SNPD_QUERY_BOOK` | `SNPDBookQuery` | Whole rendered volume (font-tagged) plus entry count, volume number, total volumes |
| `'SNPE'` `SNPD_QUERY_ENTRY` | `SNPDEntryQuery` | One entry by index (−1 = last) |
| `'SNPA'` `SNPD_QUERY_ALL_ENTRIES` | `SNPDAllEntriesQuery` | Every entry, packed as null-separated strings, plus a count of any that did not fit |

Result codes: `Success`, `NoEntries` (the volume is the "all entries removed" page, detected by `<!-- SNPD_EMPTY -->`), `NotADiary`, `IndexOutOfRange`. Buffers are fixed-size arrays in the structs; text is cut off to fit.

Implementation notes:
- Answers come from **`cachedBookText`** (in memory), not from SkyrimNet. The header says "each Dispatch queries the live SkyrimNet database directly", which is wrong (see [KNOWN_ISSUES.md](KNOWN_ISSUES.md)).
- Entry queries split the rendered text on `"[pagebreak]\n\n"` and assume entries start at page 2. Changing the layout in `FormatDiaryEntries` breaks them. See [BOOK_TEXT.md](BOOK_TEXT.md#rendering-formatdiaryentries).
- The header suggests that callers pre-filter candidate books by name (`"*'s Diary*"`) and by an empty `itemCardDescription`. The name check fails for localized titles. Callers should just send the query.

## ModEvents

| Event | Direction | Payload |
|---|---|---|
| `SkyrimNet_DiaryCreated` | SkyrimNet → SNPD | `strArg` = JSON containing `actorFormId` |
| `PhysicalDiary_Shared` | SNPD → anyone | The player took a diary from an NPC through the dialogue trade menu (e.g. a follower's inventory), not by stealing: `strArg` book name, `numArg` actor FormID (float), sender = actor. See [THEFT.md](THEFT.md). |
