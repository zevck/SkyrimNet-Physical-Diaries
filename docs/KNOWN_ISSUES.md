# Known Issues (backlog)

Found during the 2026-09-26 cleanup and while writing these docs, mostly by reading code. Unless marked **verified**, an item has not been reproduced in game: confirm it with a log before fixing (see [DEVELOPMENT.md](DEVELOPMENT.md#verifying-a-change-in-game)). Items are numbered for reference. Remove an item when it is fixed.

Fixed on 2026-09-26, for reference: `SkyrimNetDiaries_API.pex` had never shipped, so the theft decorator always returned None (**verified**, user report). Template lookup in `CreateDiaryBook` now uses `LookupByEditorID`, so powerofthree's Tweaks alone is enough (**verified**: before the change every creation failed without Native EditorID Fix). Also removed that day: the in-game `ExtraUniqueID` probe, the unused `DPF_API` shim, the `PhysicalDiaryAPI` and `GetStolenFaction` natives, a log-only duplicate container sink and the `SNDC` co-save cache it fed, Dynamic Book Framework-era `.txt` cleanup, and about a dozen uncalled functions.

## Likely bugs (user-facing)

1. **Theft and return detection do nothing in 8 of 9 languages.** `ContainerChangeHandler::ProcessEvent` (`DiaryTheftHandler.cpp`) skips any book whose name lacks `"Diary"`/`"diary"` before its real check (`GetBookForFormID`). Titles are localized (`Tagebuch`, `Journal`, `Diario`, `Dziennik`, `Дневник`, `日記`…), so only English passes. Fix: drop the name filter; `GetBookForFormID` is the authoritative test. (The filter was probably a cheap early exit; see item 12.)
2. **Dates drift after the first in-game month.** `FormatGameDate` / `FormatGameDateShort` (`main.cpp`) count every month as 30 days. Skyrim's run 28–31, so from 17 Last Seed, fourteen days later prints "1 Hearthfire" where the game says "31 Last Seed". Weekdays are right. See [BOOK_TEXT.md](BOOK_TEXT.md#dates).
3. **A volume whose entries were all deleted keeps its old text.** `RefreshVolumeOnOpen` returns early when the live count is 0 and cached text exists (to protect imported or test books), which skips the "all entries removed" re-render after a SkyrimNet CLEAR revert. Needs a real signal, such as a "never had API entries" flag, instead of treating a count of 0 as imported. See [VOLUMES_AND_SYNC.md](VOLUMES_AND_SYNC.md#save-reverts-the-keep--clear-fork).
4. **Save-folder detection can pick up the previous save.** `kPostLoadGame` reads `SkyrimNet.log` and opens `diary.db` **before** waiting for SkyrimNet's memory system. If SkyrimNet has not logged the new save ID yet, SNPD opens the previous save's DB and nothing re-checks. Today a mismatch mostly fails safe (the other save's book FormIDs don't resolve, so rows are deleted and rebuilt). It would not fail safe with deterministic FormIDs (DPF RE). Fix: detect again after the readiness wait and reopen if the folder changed.
5. **`«` and `»` are converted to Windows-1251 in every language.** `Utf8ToWin1251` runs on all books. It passes other non-Cyrillic UTF-8 through, but turns guillemets into single bytes, which likely render wrongly in French and other non-Cyrillic text. Limit the conversion to Cyrillic locales, or at least exclude these two.
   Separately, `„` (U+201E) and `№` (U+2116) are neither sanitized (`SanitizeBookText`) nor converted (`Utf8ToWin1251` passes 3-byte UTF-8 through), so they can still desync Russian pagination. Add them to `SanitizeBookText`, or decode 3-byte sequences in the converter.
6. **Reset leaves books in unloaded cells as untracked orphans.** `ResetAllDiariesInternal` removes books only from references in loaded cells and (deliberately) does not dispose the forms. A book lying in a far-away container stays in the game, untracked, and would open showing its template's own text.
7. **Inter-plugin API header overstates.** `SkyrimNetPhysicalDiariesAPI.h` says each dispatch "queries the live SkyrimNet database directly"; the handler returns `cachedBookText`. It also suggests callers pre-filter by an English title pattern and an empty `itemCardDescription`, which do not hold for localized names. Fix the header comments.

## Robustness and performance

8. **One detached thread per created book** (`DPFCreateCallback`: sleep 5 s, then clear `kCantTake`). A catch-up scan creating hundreds of volumes parks hundreds of sleeping threads. Use one timer or a game-thread deadline queue.
9. **The create queue can stall.** In `PumpDiaryCreateQueue`, if `DispatchStaticCall` fails, the in-flight flag is cleared but nothing pumps the queue again, so the remaining requests wait until the next `CreateDiaryBook` call.
10. **"Sealed" means three things.** Reaching `EntriesPerVolume` in `UpdateDiaryForActorInternal`; "a newer volume exists" in `RefreshVolumeOnOpen`; `endTime > 0` in `QueueSealedVolumeRecovery`. `endTime` is set on every volume, so the recovery's "open volume" branch (`endTime == 0`) only runs for legacy rows. It still behaves correctly, but the naming misleads.
11. **Misleading log line.** "Additional entries will be in next volume when diary is stolen/returned" (`DPFCreateCallback`): overflow volumes come from chunking, not theft.
12. **`GetBookForFormID` is a linear scan** over every volume, and it runs on every book container change and every book open. Keep a FormID → volume index next to `books_`.

## Tech debt

13. **The co-save does no real work.** `SNDB` is a two-zero sentinel. `SNDF` is written and read, but the value is thrown away at `kPostLoadGame`. Removing the co-save changes the save format; do it on purpose. See [DATABASE.md](DATABASE.md#co-save-records).
14. **CommonLib is not in the repo** (`.resources/` is gitignored) and is 4.5.0. STFU uses alandtse CommonLibSSE-NG v9.1.0 as a pinned submodule. See [DEVELOPMENT.md](DEVELOPMENT.md#build) and the [engine touchpoints](DEVELOPMENT.md#engine-touchpoints) to re-check.
15. **The version says 1.0.0** in `CMakeLists.txt` and `vcpkg.json`, but v1.1.0 has shipped.
16. **Unused ESP record** `SNPD_DiaryStolenFaction` (from a faction-based theft design). ESP edit.
17. **Uncommitted VR work to finish:** `ClearBogusSourceFiles` / `SanitizeLoadedBookForms` (`BookManager.cpp`) and `useDefaultPos` (`BookTextHook.cpp`) need a VR test. Also find where DPF's `0x1` comes from.
18. **`main.cpp` is 1,800 lines** of unrelated subsystems: lifecycle and messaging, entry → volume sync, text formatting and sanitizing, catch-up and recovery scans, reset, and the inter-plugin API handlers. Natural files to split into.
19. **Hand-written inline hook.** `BookTextHook::Install` decodes the target's prologue itself. A detour library would be sturdier across runtime updates.
20. **The event listener parses JSON by string search** for `actorFormId`. It works while SkyrimNet's payload shape stays the same.
21. `GetDiaryTheftStatus` always returns `"chronicled": false`, left over from an older design. Public API, so change it carefully.

## Open decision

22. **Stay on DPF, move to DPF RE, or write our own.** DPF's two known bugs (unsafe allocator, duplicate recycled records) are handled (see [BOOK_FORMS.md](BOOK_FORMS.md#the-two-dpf-bugs-this-pipeline-works-around)). DPF RE would make book FormIDs deterministic, but has open questions (whole-registry rewrite on every create, thread safety, whether forms exist in time for `.ess` inventory). Per-instance identity and an ESL pool were tested and rejected. See [BOOK_FORMS.md](BOOK_FORMS.md#alternatives-evaluated).
