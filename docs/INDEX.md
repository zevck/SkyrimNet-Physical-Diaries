# SkyrimNet Physical Diaries Documentation Index

Developer docs for SkyrimNet Physical Diaries (SNPD): an SKSE plugin that turns the diary entries SkyrimNet writes for NPCs into real, readable, stealable books. They are written for coding agents and the author. The user-facing doc is the repo-root [README.md](../README.md).

New here? Read [ARCHITECTURE.md](ARCHITECTURE.md) first, then [DEVELOPMENT.md](DEVELOPMENT.md).

## By Task (What Are You Trying To Do?)

| Task | Start Here | Also See |
|------|------------|----------|
| Understand how the pieces fit together | [ARCHITECTURE.md](ARCHITECTURE.md) | [BOOK_FORMS.md](BOOK_FORMS.md), [VOLUMES_AND_SYNC.md](VOLUMES_AND_SYNC.md) |
| Build, deploy, and check a change in game | [DEVELOPMENT.md](DEVELOPMENT.md) | |
| Compile or add a Papyrus script or native | [PAPYRUS_AND_API.md](PAPYRUS_AND_API.md#adding-or-changing-a-native-all-four-steps-every-time) | [DEVELOPMENT.md](DEVELOPMENT.md#papyrus) |
| Update CommonLib or support a new runtime | [DEVELOPMENT.md](DEVELOPMENT.md#engine-touchpoints) | [BOOK_TEXT.md](BOOK_TEXT.md#delivery-the-openbookmenu-hook) |
| Debug a missing, empty or wrong book | [BOOK_FORMS.md](BOOK_FORMS.md) | [DEVELOPMENT.md](DEVELOPMENT.md#logging), [VOLUMES_AND_SYNC.md](VOLUMES_AND_SYNC.md) |
| Debug two NPCs' diaries showing the same text | [BOOK_FORMS.md](BOOK_FORMS.md#the-two-dpf-bugs-this-pipeline-works-around) | |
| Change how entries are split into volumes | [VOLUMES_AND_SYNC.md](VOLUMES_AND_SYNC.md) | [DATABASE.md](DATABASE.md) |
| Work on save reverts (SkyrimNet KEEP / CLEAR) | [VOLUMES_AND_SYNC.md](VOLUMES_AND_SYNC.md#save-reverts-the-keep--clear-fork) | [THEFT.md](THEFT.md#save-reverts) |
| Change how the text looks or is cleaned up | [BOOK_TEXT.md](BOOK_TEXT.md) | [CONFIG_AND_MCM.md](CONFIG_AND_MCM.md) |
| Fix something VR-specific | [BOOK_TEXT.md](BOOK_TEXT.md#delivery-the-openbookmenu-hook) | [BOOK_FORMS.md](BOOK_FORMS.md#configuring-a-new-form-game-thread), [DEVELOPMENT.md](DEVELOPMENT.md#engine-touchpoints) |
| Work on theft, return or the SkyrimNet decorator | [THEFT.md](THEFT.md) | [PAPYRUS_AND_API.md](PAPYRUS_AND_API.md) |
| Add a language or fix a translation | [LOCALIZATION.md](LOCALIZATION.md) | |
| Add a setting end to end (INI, MCM) | [CONFIG_AND_MCM.md](CONFIG_AND_MCM.md#adding-a-setting-end-to-end) | [PAPYRUS_AND_API.md](PAPYRUS_AND_API.md) |
| Add a DB column or inspect a live database | [DATABASE.md](DATABASE.md) | |
| Let another mod read diary text | [PAPYRUS_AND_API.md](PAPYRUS_AND_API.md#inter-plugin-api-skse-messaging) | |
| Decide on DPF vs DPF RE vs something else | [BOOK_FORMS.md](BOOK_FORMS.md#alternatives-evaluated) | [KNOWN_ISSUES.md](KNOWN_ISSUES.md) |
| Pick up a known bug or cleanup task | [KNOWN_ISSUES.md](KNOWN_ISSUES.md) | |

## By System

### Runtime
| Document | Description |
|----------|-------------|
| [ARCHITECTURE.md](ARCHITECTURE.md) | Repo layout, component map, startup and load sequence, one entry end to end, threading, dependencies |
| [BOOK_FORMS.md](BOOK_FORMS.md) | DPF creation queue, FormID claim table, templates, finding the NPC, validity across loads, alternatives tested |
| [VOLUMES_AND_SYNC.md](VOLUMES_AND_SYNC.md) | Entries → volumes, boundaries, update and seal, load-time recovery and catch-up, refresh on open, save reverts, save-folder detection |
| [BOOK_TEXT.md](BOOK_TEXT.md) | Rendering, dates, cleaning LLM output, the `OpenBookMenu` hook (and VR's ninth argument), the `GetDescription` hook for other readers, UTF-8 → Win-1251 |
| [THEFT.md](THEFT.md) | Theft, return and handover; the decorator; clearing; save reverts |
| [LOCALIZATION.md](LOCALIZATION.md) | Language choice, locale files, GMST names, MCM translations |

### Data, settings and interfaces
| Document | Description |
|----------|-------------|
| [DATABASE.md](DATABASE.md) | DiaryDB schema and lifetime, adding columns, co-save records, Reset, inspecting a DB |
| [CONFIG_AND_MCM.md](CONFIG_AND_MCM.md) | INI keys, MCM pages, when text is rebuilt, adding a setting |
| [PAPYRUS_AND_API.md](PAPYRUS_AND_API.md) | Scripts, natives, the public theft API, the SKSE-message API, ModEvents |

### Development
| Document | Description |
|----------|-------------|
| [DEVELOPMENT.md](DEVELOPMENT.md) | Build, deploy, Papyrus, logging, verifying in game, engine touchpoints, conventions |
| [KNOWN_ISSUES.md](KNOWN_ISSUES.md) | Backlog of known bugs, performance issues and tech debt |

## Ground rules for changing SNPD

- **SNPD presents SkyrimNet's data; it never authors or changes it.** Diary content always comes from SkyrimNet. DiaryDB holds only SNPD's own bookkeeping and a render cache.
- **Identify books by FormID and NPCs by SkyrimNet UUID.** Never by name (localized, and shared by same-named NPCs), and never by a stored FormID without a UUID check.
- **Every persistence change must survive** save → reload, reload without saving, loading an older save with SkyrimNet KEEP **and** CLEAR, and a second character. See [VOLUMES_AND_SYNC.md](VOLUMES_AND_SYNC.md#save-reverts-the-keep--clear-fork).
- **Never `Dispose` DPF forms**, and never release DPF RE slots, on Reset. See [BOOK_FORMS.md](BOOK_FORMS.md).
- **One DLL for SE, AE and VR.** Test the hook and form creation on VR when you touch them.
- **A Papyrus change isn't done until the `.pex` is built and shipped.**
- **There is no automated test suite.** Changes are checked in game through the logs.
- **These docs describe the code as of 2026-09-27** (v1.2.0, with the VR hook changes still untested on VR). Line numbers drift; function names are the stable anchor. When the code and a doc disagree, the code wins. Fix the doc in the same change.
