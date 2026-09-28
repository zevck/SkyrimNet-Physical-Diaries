# SkyrimNet Physical Diaries

SKSE plugin (CommonLibSSE-NG, C++23; one DLL for SE, AE and VR) that turns the diary entries **SkyrimNet** writes for NPCs into real book items. NPCs carry them, they can be pickpocketed, and the NPC notices the theft the next time they write.

**Full developer docs: [docs/INDEX.md](docs/INDEX.md).** Start with [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md). The docs are the source of truth for how the code works. Keep them current: a change that makes a doc wrong fixes the doc in the same change.

## Ground rules

- SNPD **presents** SkyrimNet's data; it never writes or changes diary entries. DiaryDB holds SNPD's own bookkeeping plus a render cache.
- Identify books by **base FormID** (`BookManager::GetBookForFormID`) and NPCs by **SkyrimNet UUID**. Never by name (localized; same-named NPCs share an identity) and never by a stored FormID without a UUID back-check.
- Every persistence change must survive: save → reload, reload without saving, loading an older save with SkyrimNet **KEEP and CLEAR**, and a second character. See [docs/VOLUMES_AND_SYNC.md](docs/VOLUMES_AND_SYNC.md).
- Book forms are runtime forms the engine saves itself (`DynamicForms`). Never pick a FormID yourself; a volume that goes away is retired (`BookManager::RetireBook`): its form stays in the save, never removed, or a world copy of it crashes the game on load. See [docs/BOOK_FORMS.md](docs/BOOK_FORMS.md).
- Game state is touched only on the game thread (`SKSE::GetTaskInterface()->AddTask`). Papyrus natives run on the game thread (registered non-tasklet).
- A Papyrus change is done only when the `.pex` is compiled into `Scripts/` **and** shipped. See [docs/PAPYRUS_AND_API.md](docs/PAPYRUS_AND_API.md).
- **Build and deploy with `.\Build_Local.ps1`**: incremental plugin build, Pyro (`skyrimse.ppj`, same as the VS Code task), and deploy to the `Physical Diaries - Dev` mod folder in each test instance (one per runtime: SE, AE, VR). Paths are in the gitignored `Build_Config_Local.ps1`. PASS/FAIL is also written to `%TEMP%\snpd-build-result.json`. Never `/t:Rebuild`: it rebuilds all of CommonLib. See [docs/DEVELOPMENT.md](docs/DEVELOPMENT.md#build).

## Open work (as of 2026-09-28)

- 2.0.0 replaced Dynamic Persistent Forms with engine-saved runtime forms ([docs/BOOK_FORMS.md](docs/BOOK_FORMS.md)). Run on AE; the engine behaviour was checked in the VR binary, but it hasn't been run in game on VR yet ([docs/KNOWN_ISSUES.md](docs/KNOWN_ISSUES.md)).

Ask before committing.

## Watch out

If `git diff --stat` is bigger than the change you made, stop. In mid-2026 VS Code saved stale March-era buffers over three source files, silently reverting committed fixes.
