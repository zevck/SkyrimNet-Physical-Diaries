# The plugin (ESP)

`SkyrimNet Physical Diaries.esp` holds the records the DLL can't make: the template books every diary copies, and the quest that runs the scripts. Its source is text in git, written with [Spriggit](https://github.com/Mutagen-Modding/Spriggit); the `.esp` itself is never committed. This is the same setup as Physical Letters (its `docs/PLUGIN.md`) and SkyrimNet (its `docs/skyrim_plugins.md`).

Converted from the binary ESP on 2026-09-29. A round trip (YAML → ESP) gives the same records byte for byte; only the header's next free FormID changed (`0xD68` → `0xD63`, Spriggit recomputes it from the highest record).

## Layout

`spriggit/SkyrimNetPhysicalDiaries/`:

| File | What |
|---|---|
| `spriggit-meta.json` | Spriggit package and version (`Spriggit.Yaml.Skyrim` 0.41.0), game release, file name |
| `RecordData.yaml` | The header: ESL-flagged (`Small`), author, master `Skyrim.esm` |
| `<RecordType>/…` | One folder per record type, one file per record |

## Records

| FormID | Type | EditorID | What |
|---|---|---|---|
| `0x800` | Book | `SkyrimNetDiaryTemplate2` | Template diary (a journal model); see [BOOK_FORMS.md](BOOK_FORMS.md) |
| `0x801` | Book | `SkyrimNetDiaryTemplate3` | Template diary |
| `0x802` | Book | `SkyrimNetDiaryTemplateN` | The Nightingales' template diary |
| `0x803` | Faction | `SNPD_DiaryStolenFaction` | Unused, from an earlier theft design; harmless, left in place ([THEFT.md](THEFT.md)) |
| `0x804` | Quest | `SNPD_Quest` | Starts with the game. Scripts `SkyrimNetDiaries_EventListener` and `SkyrimNetDiaries_MCM`; player alias with `SKI_PlayerLoadGameAlias` ([PAPYRUS_AND_API.md](PAPYRUS_AND_API.md)) |
| `0x805`, `0x806`, `0x807` | Book | `SNPD_BlankJournal1`, `2`, `3` | Blank Journal, one per template look (`JournalLowPoly01`–`03`; not the Nightingale one). 20 gold, `VendorItemBook`. Named from the locale file (`[Format] BlankJournal`) at load |
| `0x808` | LeveledItem | `SNPD_LItemBlankJournal` | One of the three at random. The DLL adds it to Skyrim.esm's `LItemMiscVendorMiscItems75` in memory ([EDITING.md](EDITING.md#blank-journals)) |
| `0x809`, `0x80A`, `0x80B` | ConstructibleObject | `SNPD_RecipeBlankJournal1`, `2`, `3` | Tanning rack: 1 Leather + 2 Roll of Paper → one Blank Journal of that look. Hidden (no workbench) while writing is off |
| `0x80C` | Book | `SNPD_BlankJournalN` | Blank Journal in the Nightingale look (`TG05JournalLowPoly01`). Not sold |
| `0x80D` | ConstructibleObject | `SNPD_RecipeBlankJournalN` | Its tanning-rack recipe, the same materials, once the player is a Nightingale: `GetStageDone` TG08A ("Trinity Restored", `057F99`) stage 57 (the Oath) OR stage 200 (completed). Hidden while writing is off |
| `0x80E` | FormList | `SNPD_Quills` | What counts as a quill for writing: `Quill01`, `FVDQuill` (the Quill of Gemination) ([EDITING.md](EDITING.md#quill-and-ink)) |
| `0x80F` | FormList | `SNPD_Inkwells` | What counts as a full inkwell: `Inkwell01` |
| `0x810`–`0x818` | MiscItem | `SNPD_Inkwell1` to `SNPD_Inkwell9` | Partly used inkwells, one per uses left: clones of `Inkwell01` (bounds, model, value 1, weight 0.3), renamed to the game's inkwell name at load. Described by `SkyrimNet Physical Diaries_DESC.ini` when Description Framework is installed (which also calls the vanilla `Inkwell01` "Full.") |
| `0xD62` | Book | `SkyrimNetDiaryTemplate` | Template diary |

The DLL finds the templates by EditorID (`kJournalTemplates`, `kNightingaleTemplate` in `BookManager.h`), so a template's FormID can change, its EditorID can't.

The vanilla masters dumped with Spriggit in the same format (`skyrim-esm-yaml` and the others) are the reference when writing or reviewing records.

## Tools

`utilities/Spriggit.ps1` pins the CLI: Spriggit 0.41.0, from its GitHub release, SHA-256 checked. The CLI is `$spriggitPath` from `Build_Config_Local.ps1` if set (for example SkyrimNet's copy in `SkyrimNet-Dev\external`), otherwise `external\SpriggitCLI-0.41.0`, downloaded on first use (gitignored). The CLI fetches the `Spriggit.Yaml.Skyrim` package from NuGet the first time.

## Build

`Build_Local.ps1` runs `convert-to-plugin` into `build\esp\SkyrimNet Physical Diaries.esp` when any source file is newer than it, and deploys it with the DLL. `-skipEsp` skips both.

**The deploy never overwrites an edited plugin.** If the `.esp` in a deploy folder is newer than the build, someone edited it there, and the deploy reports that instance as failed instead of copying over it.

## Editing

**Edit the YAML.** Records are written and reviewed as text, and `Build_Local.ps1` builds the `.esp` from them. The vanilla dumps show how any record type looks.

If a record is easier to make in the Creation Kit or xEdit, edit the deployed `.esp` there, then:

1. `.\utilities\esp_to_spriggit.ps1` converts the newest copy in the deploy folders back into `spriggit/SkyrimNetPhysicalDiaries` (`-EspPath` for another file), with the pinned package and version.
2. Review `git diff spriggit`, then build: the build is newer than the deployed copy again, so the deploy goes ahead.

**FormIDs:** the plugin is ESL-flagged, so its records use `0x800` to `0xFFF`. Records written by hand take the next free one from `0x819`; the CK or xEdit assign their own (from `0xD63`).

There is no SEQ file: the quest has no dialogue.
