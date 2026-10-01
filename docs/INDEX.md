# SkyrimNet Physical Diaries Documentation Index

Developer docs for SkyrimNet Physical Diaries (SNPD): an SKSE plugin that turns the diary entries SkyrimNet writes for NPCs into real, readable, stealable books. They are written for coding agents and the author. The user-facing doc is the repo-root [README.md](../README.md).

New here? Read [ARCHITECTURE.md](ARCHITECTURE.md) first, then [DEVELOPMENT.md](DEVELOPMENT.md).

## By Task (What Are You Trying To Do?)

| Task | Start Here | Also See |
|------|------------|----------|
| Understand how the pieces fit together | [ARCHITECTURE.md](ARCHITECTURE.md) | [BOOK_FORMS.md](BOOK_FORMS.md), [VOLUMES_AND_SYNC.md](VOLUMES_AND_SYNC.md) |
| Build, deploy, and check a change in game | [DEVELOPMENT.md](DEVELOPMENT.md) | |
| Compile or add a Papyrus script or native | [PAPYRUS_AND_API.md](PAPYRUS_AND_API.md#adding-or-changing-a-native-all-four-steps-every-time) | [DEVELOPMENT.md](DEVELOPMENT.md#papyrus) |
| Update CommonLib or support a new runtime | [DEVELOPMENT.md](DEVELOPMENT.md#engine-touchpoints) | [BOOK_TEXT.md](BOOK_TEXT.md#delivery-the-getdescription-hook), [BOOK_FORMS.md](BOOK_FORMS.md#the-engine-behaviour-this-rests-on) |
| Debug a missing, empty or wrong book | [BOOK_FORMS.md](BOOK_FORMS.md) | [DEVELOPMENT.md](DEVELOPMENT.md#logging), [VOLUMES_AND_SYNC.md](VOLUMES_AND_SYNC.md) |
| Debug a missing, blank or wrong book after a load | [BOOK_FORMS.md](BOOK_FORMS.md#load) | [DATABASE.md](DATABASE.md#co-save-records) |
| Change how entries are split into volumes | [VOLUMES_AND_SYNC.md](VOLUMES_AND_SYNC.md) | [DATABASE.md](DATABASE.md) |
| Work on save reverts (SkyrimNet KEEP / CLEAR) | [VOLUMES_AND_SYNC.md](VOLUMES_AND_SYNC.md#save-reverts-the-keep--clear-fork) | [THEFT.md](THEFT.md#save-reverts) |
| Change how the text looks or is cleaned up | [BOOK_TEXT.md](BOOK_TEXT.md) | [CONFIG_AND_MCM.md](CONFIG_AND_MCM.md) |
| Work on the player editing their diary in the book menu | [EDITING.md](EDITING.md) | [DEVELOPMENT.md](DEVELOPMENT.md#swf) |
| Work on NPCs writing diaries on their own | [NPC_DIARIES.md](NPC_DIARIES.md) | [PLUGIN.md](PLUGIN.md), [CONFIG_AND_MCM.md](CONFIG_AND_MCM.md) |
| Fix something VR-specific | [BOOK_TEXT.md](BOOK_TEXT.md#delivery-the-getdescription-hook) | [BOOK_FORMS.md](BOOK_FORMS.md#the-engine-behaviour-this-rests-on), [DEVELOPMENT.md](DEVELOPMENT.md#engine-touchpoints) |
| Work on theft, return or the SkyrimNet decorator | [THEFT.md](THEFT.md) | [PAPYRUS_AND_API.md](PAPYRUS_AND_API.md) |
| Add a language or fix a translation | [LOCALIZATION.md](LOCALIZATION.md) | |
| Add a setting end to end (INI, MCM) | [CONFIG_AND_MCM.md](CONFIG_AND_MCM.md#adding-a-setting-end-to-end) | [PAPYRUS_AND_API.md](PAPYRUS_AND_API.md) |
| Add a DB column or inspect a live database | [DATABASE.md](DATABASE.md) | |
| Let another mod read diary text | [PAPYRUS_AND_API.md](PAPYRUS_AND_API.md#inter-plugin-api-skse-messaging) | |
| Reuse the runtime-form mechanism in another mod | [BOOK_FORMS.md](BOOK_FORMS.md#reuse-in-other-mods) | `include/DynamicForms.h` |
| Add or change a record in the ESP | [PLUGIN.md](PLUGIN.md) | `spriggit/SkyrimNetPhysicalDiaries/` |
| Pick up a known bug or cleanup task | [KNOWN_ISSUES.md](KNOWN_ISSUES.md) | |

## By System

### Runtime
| Document | Description |
|----------|-------------|
| [ARCHITECTURE.md](ARCHITECTURE.md) | Repo layout, component map, startup and load sequence, one entry end to end, threading, dependencies |
| [BOOK_FORMS.md](BOOK_FORMS.md) | Book forms the engine saves itself (no plugin file): the engine facts they rest on, creation, the co-save record, load and matching against DiaryDB, retirement, templates, finding the NPC, the DPF migration, alternatives tested |
| [VOLUMES_AND_SYNC.md](VOLUMES_AND_SYNC.md) | Entries → volumes, boundaries, update and seal, load-time recovery and catch-up, refresh on open, save reverts, save-folder detection |
| [BOOK_TEXT.md](BOOK_TEXT.md) | Rendering, dates, cleaning LLM output, the `GetDescription` hook (the book menu and other readers), UTF-8 → Win-1251 |
| [THEFT.md](THEFT.md) | Theft, return and handover; the decorator; clearing; save reverts |
| [EDITING.md](EDITING.md) | The player writing in their own journal in the book menu: writing mode, diaries and journals, the SWF's edit mode, input, saving to SkyrimNet, back to reading, tearing out an entry, the close hook |
| [NPC_DIARIES.md](NPC_DIARIES.md) | Optional: NPCs writing diary entries on their own once a day through SkyrimNet's generation: when, who, the whitelist dialogue |
| [LOCALIZATION.md](LOCALIZATION.md) | Language choice, locale files, GMST names, MCM translations |

### Data, settings and interfaces
| Document | Description |
|----------|-------------|
| [DATABASE.md](DATABASE.md) | DiaryDB schema and lifetime, adding columns, co-save records, Reset, inspecting a DB |
| [CONFIG_AND_MCM.md](CONFIG_AND_MCM.md) | INI keys, MCM pages, when text is rebuilt, adding a setting |
| [PAPYRUS_AND_API.md](PAPYRUS_AND_API.md) | Scripts, natives, the public theft API, the SKSE-message API, ModEvents |
| [PLUGIN.md](PLUGIN.md) | The ESP: its Spriggit YAML source, records and FormIDs, building it, editing it in the CK or xEdit |

### Development
| Document | Description |
|----------|-------------|
| [DEVELOPMENT.md](DEVELOPMENT.md) | Build, deploy, Papyrus, logging, verifying in game, engine touchpoints, conventions |
| [KNOWN_ISSUES.md](KNOWN_ISSUES.md) | Backlog of known bugs, performance issues and tech debt |

## Ground rules for changing SNPD

- **SNPD presents SkyrimNet's data; the one thing it writes is the player's own journal, when the player writes in it.** Diary content always comes from SkyrimNet, and edits go back to SkyrimNet (never into DiaryDB). NPC diaries are never written or edited by SNPD: their entries are the NPCs' memories. (With [NPC diaries](NPC_DIARIES.md) on, SNPD asks SkyrimNet to write one; SkyrimNet writes it.) DiaryDB holds only SNPD's own bookkeeping and a render cache. See [EDITING.md](EDITING.md).
- **Identify books by FormID and NPCs by SkyrimNet UUID.** Never by name (localized, and shared by same-named NPCs), and never by a stored FormID without a UUID check.
- **Every persistence change must survive** save → reload, reload without saving, loading an older save with SkyrimNet KEEP **and** CLEAR, and a second character. See [VOLUMES_AND_SYNC.md](VOLUMES_AND_SYNC.md#save-reverts-the-keep--clear-fork).
- **Retire book forms, never delete them.** A volume that goes away for certain (Reset, a Clear) is retired (`BookManager::RetireBook`): its form stays in the save, flagged, because a world copy of a removed form crashes the game on load. Never pick FormIDs yourself or reuse a retired one. See [BOOK_FORMS.md](BOOK_FORMS.md#retirement).
- **One DLL for SE, AE and VR.** Test the hook, form creation and the co-save record on VR when you touch them.
- **A Papyrus change isn't done until the `.pex` is built and shipped.**
- **There is no automated test suite.** Changes are checked in game through the logs.
- **These docs describe the code as of 2026-09-28** (v2.0.0, with the new book forms not yet run in game on VR). Line numbers drift; function names are the stable anchor. When the code and a doc disagree, the code wins. Fix the doc in the same change.
