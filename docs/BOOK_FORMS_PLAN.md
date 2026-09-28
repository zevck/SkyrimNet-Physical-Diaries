# Plan: engine-persisted book forms (replacing DPF)

Status: **design, spike passed on AE** (2026-09-28). Decision: replace Dynamic Persistent Forms with dynamic forms the engine itself persists. No plugin file, no FormID allocator of our own, no global registry. Why not DPF RE is in [BOOK_FORMS.md](BOOK_FORMS.md#alternatives-evaluated). When this ships, this file folds into BOOK_FORMS.md.

---

## Spike results (AE 1.6.1170, 2026-09-28) — these override the design below where they differ

The spike (throwaway code, never committed, deleted once it passed) created a factory `TESObjectBOOK`, recorded it in a co-save record, and logged everything to `SKSE/snpd_spike.log` (which survives restarts). Verified on AE; the engine facts were decompiled on SE; **VR not tested yet**.

| Test | Result |
|---|---|
| Factory form: `0xFF` ID from the engine, flags `0x08` (no `kTemporary`), `AddChange(kFlags)` accepted | ✅ |
| Save → **restart** → load: form recreated at the same FormID as a book; inventory copy intact | ✅ |
| World copy survives, but its 3D is built before the load callback configures the shell, so it is invisible | ✅ with a fix: after configuring, `Disable()` + `Enable(false)` every world copy in loaded cells (rebuilds 3D) |
| Dropped book vanished (no 3D) | Fixed: `ConfigureDiaryForm` now copies the template's world model, bounds, pickup/putdown sounds and keywords (already in `BookCreation.cpp`) |
| A loaded save that owns our FormID for another form renumbers ours (`ClearForm`) | ✅ (a Wizards' Guard took `FF001601`) |
| Item card (`CNAM`) with null `sourceFiles` | ✅ no crash |
| Book menu text with **no `OpenBookMenu` injection** | ✅ via the `GetDescription` hook, see below |
| `kCantTake` delay | Not needed (it guarded a DPF async-registration race; creation is synchronous now) |
| `SetDelete(true)` / `RemoveChange` to retire a form | ❌ Neither keeps it out of the save: the save loop writes every change-map entry, even one with zero flags, and `RemoveChange` only clears flags |
| Retire via a `TESObjectBOOK::CheckSaveGame` hook (vtable slot `0x0D`) returning false | ✅ The save loop removes the entry and doesn't write it; after restart the form is gone (and its FormID was immediately reused by a Draugr) |

**Design changes these force:**

1. **Book text through `GetDescription` only; drop the `OpenBookMenu` hook.** All three engine callers of `OpenBookMenu` build the text with `GetDescription(book + 0xA8, out, parent = nullptr, 'DESC')`. Of the 16 direct callers of `GetDescription` (SE ID 14399), only those three pass a book's component (`+0xA8`); the others pass a different class (`+0x30`) or the item card (`+0x128`, with a parent, field `CNAM`). So in the hook: **our book's component with no parent** = the book menu is opening → refresh from SkyrimNet, Win-1251 for Cyrillic; **our book with a parent** = another reader (SkyrimNet, Immersive Reading) → cached UTF-8, font tags stripped. Identify our books by a `TESDescription*` → form map kept with the FormID index (never by pointer arithmetic). Dropping the `OpenBookMenu` hook removes the VR ninth-argument risk entirely. Re-check the caller list on AE and VR binaries.
2. **Retirement (Reset, volumes dropped after a Clear) = `CheckSaveGame` hook, not `SetDelete`.** The hook returns false for a **session-only** retired set: filled when SNPD retires a volume, applied at every save that session, cleared at `EndSession`. Session-only because freed IDs are reused at once; a persistent list could drop an unrelated book later. The next load removes every copy everywhere, including unloaded containers (inventory entries whose base form is missing are deleted). This closes KNOWN_ISSUES #1 and needs no cell-attach cleanup.
3. **After the load callback configures shells, rebuild the 3D of world copies in loaded cells.** Copies in cells that load later are built after configuration and need nothing.
4. **The engine does not drop unreferenced forms** that have a change record: a form nobody holds stays in the save until `CheckSaveGame` says no. Only retirement removes forms; nothing else accumulates (one ~20-byte record per live volume).

**Remaining:** VR run (steps: create, restart+load, world copy, item card, book text); re-check the `GetDescription`/`OpenBookMenu` caller facts on the AE and VR binaries; delete the spike.

---

## The engine facts this rests on

Reverse-engineered from Skyrim SE 1.5.97 (static decompilation; Address Library IDs given so it can be re-checked, and re-checked on AE and VR). Not yet run-tested: that is what the spike is for.

1. **Any runtime form with a change record is written to the save.** `BGSSaveLoadGame::SaveGame` (ID 34676) writes one changed-form record per entry in its change map. `TESForm::AddChange` (14451) puts a form there; its gate (34653) rejects only FormID 0, forms with the `kTemporary` flag (`0x4000`), and deleted non-reference dynamic forms. There is no form-type filter. The closed "Created Objects" list (enchantments, potions, poisons; IDs 35260/35261) is a separate mechanism for *their content* and is irrelevant to us.
2. **On load, the engine recreates the form before it resolves inventories.** In `BGSSaveLoadGame::LoadGame` (34677), loop 1 (34681) constructs a fresh instance of the saved form type for every record with a `0xFF` FormID (via the type's factory, then `SetFormID(saved, true)`). Loop 2 then runs each form's `LoadGame`; that is where `InventoryChanges::LoadGame` (15903) resolves an inventory entry's base form with `LookupByID` and **deletes the entry if it doesn't resolve**. Because loop 1 finished first, entries pointing at our books resolve. World copies (created references) are filed by cell in loop 1 and constructed when the cell attaches, later still.
3. **The record holds only flags.** `TESObjectBOOK::SaveGame` (17440) writes form flags and the book's read/teaches byte. Name, model, type, text: nothing. The plugin re-applies those every session.
4. **The `0xFF` allocator is the engine's.** `GetNextID` (13635) skips every ID in the all-forms map and every ID in the loaded save's change map; the `TESForm` constructor takes an ID from it. `SetFormID` (14508) has no collision check, so a plugin must never pick IDs itself. The counter is written to the save but **not restored** (34744/34745); after a load it relies on the skip loop.
5. **FormIDs are stable within a save's timeline, not across timelines.** `ClearForm` (34665) renumbers a dynamic form when the loaded save doesn't mention it (34644) or another record of a different type claims its ID. Renumbered forms stay alive in memory under the new ID.
6. **Timing of SKSE hooks.** `kPreLoadGame` fires before the load, `kPostLoadGame` after it returns. SKSE's serialization load callbacks run *inside* the load, in the Papyrus global-data phase: after loop 2 (inventories already resolved) and after `InitLoadGame`/`FinishLoadGame`, before `kPostLoadGame`.

Consequences: a form that exists before the load with the same FormID is reused; one recreated only in a load callback is too late for inventories — but with a change record the engine has already recreated it, and the callback only fills it in.

---

## The model

- **A diary volume is a dynamic `TESObjectBOOK`** created with the type factory. The engine assigns its `0xFF` FormID. It is marked persistent (`AddChange`) at creation and kept alive for the whole process (never deleted, never freed; renumbered forms are harmless orphans).
- **The save owns the form.** Its inventory entries, world copies and the form's existence all live in the `.ess`. SNPD adds one co-save record per live volume saying what the form *is*: `FormID → (actor UUID, volume number, template EditorID, display name)`.
- **Forms are per save timeline.** Two playthroughs never share a form; saves in one playthrough share the forms that existed when each was written. Loading a save that predates a volume means that volume's form doesn't exist there (the engine renumbered the in-memory one), and the volume is recreated from SkyrimNet's entries if they still exist in that timeline, exactly the case `LoadFromDB` handles today.
- **Every session re-applies the look.** The load callback configures each shell from the co-save record (name, template's model/type/item card) so books look right the moment the game is playable; `LoadFromDB` later reconciles with DiaryDB as now.

What stays exactly as it is: DiaryDB and the volume logic, both text hooks, `GetBookForFormID` and its index, theft, `TimelineGate`, the rendering.

---

## Components

### 1. `DynamicForms` (new, no SNPD types inside it)

A small library-style module, written so it can be lifted into other mods later (letters, quest rewards): it knows nothing about diaries.

```cpp
namespace DynamicForms {
    // Creates a persistent runtime form of T (the engine assigns the FormID), clears
    // kTemporary, calls AddChange(kFlags). Returns nullptr if the factory fails.
    template <class T> T* Create();

    // The co-save record: what this form is, in the owner's terms.
    struct Record { RE::FormID formId; std::string owner; std::string key;
                    std::string templateEditorId; std::string displayName; };

    void Save(SKSE::SerializationInterface*, std::span<const Record>);     // one record type, versioned
    std::vector<Record> Load(SKSE::SerializationInterface*, std::uint32_t version);

    // For each loaded record: LookupByID, check the form type, drop mismatches (the
    // engine renumbered a squatter; log it), return the survivors with their forms.
    template <class T> std::vector<std::pair<Record, T*>> Resolve(const std::vector<Record>&);
}
```

Plus the two text hooks (`OpenBookMenu`, `GetDescription`) with a **text-provider** registration (`FormID → optional text`, refresh-on-open callback) instead of direct `BookManager` calls, so a second book-using mod can register its own provider. This is the only part that is book-specific; a rewards mod ignores it.

Sharing: with no ESP there is no plugin-slot argument for a separate runtime plugin. Ship it as source inside SNPD, with no SNPD includes, in its own folder; extract to a git submodule when the second consumer exists. A runtime plugin would only add version coupling.

### 2. `BookForms` (SNPD's use of it, replaces `BookCreation.cpp`)

- `CreateDiaryBook(...)` becomes synchronous, on the game thread, in the caller's task:
  ```
  template = SelectJournalTemplate(...)
  book = DynamicForms::Create<RE::TESObjectBOOK>()
  ConfigureDiaryForm(book, template, FormatBookName(name, volume))
  RegisterBook(DiaryBookData{... bookFormId = book->GetFormID() ...})
  SetVolumeText(registered, entries)
  FindActorForBook(...)->AddObjectToContainer(book)
  ```
  `CreateAllVolumesForActor` loops it as today. No queue, no claim table, no generations, no pending counters, no collision retries, no Papyrus callback.
- `ConfigureDiaryForm` unchanged, moved here. It runs at creation, in the load callback, and in `LoadFromDB`.

### 3. Co-save (`Serialization.cpp`)

- `SaveCallback`: after `FlushToDB` as now, write one `DynamicForms::Record` per volume in `books_` (owner `"snpd"`, key `"<uuid>|v<n>"`, template, name). Record type `'SNBF'`, version 1, under the existing `'SNDB'` unique ID. Older saves' `SNDB`/`SNDF`/`SNDC` records stay unread.
- `LoadCallback` (new, registered again): `Load` → `Resolve<TESObjectBOOK>` → `ConfigureDiaryForm` each survivor → hand the list to `BookManager::SetLoadedShells(...)`, an in-memory map `FormID → (uuid, volume)` for this load.
- `RevertCallback`: clears that map too.

### 4. `LoadFromDB` (simpler)

For each DiaryDB row: the volume is valid if its `book_form_id` is in the loaded-shells map with the same (uuid, volume). Otherwise the form doesn't exist in this timeline: delete the row and queue recreation (today's path). The `LookupByID` + type check stays as a second line of defence.

Two mechanisms become unnecessary and should be removed once the spike confirms the engine behaviour:
- **`persisted_in_save` and `QueueInventoryCheck`.** They exist because DPF forms survived a reload-without-save in memory and had to be put back into inventories by hand. Now a reload either has the form in the save's records (inventory intact, engine-restored) or doesn't (renumbered; recreate). The column stays in the schema, unused.
- **The Keep-after-in-session-revert fix in `ReconcileWithTimeline`** (a volume newer than the save is handed back to the NPC): that volume's form no longer exists after the load, so it goes through recreation instead.

### 5. Reset and orphans (KNOWN_ISSUES #1)

Today a Reset can only remove books from loaded references, and the forms are never disposed. New behaviour: Reset calls `SetDelete(true)` on each volume's form after removing it from loaded references. Fact 1's gate then refuses the form a change record, so the next save omits it, and on the next load the engine drops **every** copy, including ones in unloaded cells and containers, as it does for any missing base form. The form stays alive for the rest of the session (its remaining copies are removed as their cells load, as now). Verify in the spike that `SetDelete` on a dynamic base form is safe in-session.

The same mechanism handles a volume dropped by `ReconcileWithTimeline` after a Clear.

### 6. Hooks

`BookTextHook` unchanged in behaviour; it now asks the registered text provider (SNPD's), which answers from `GetBookForFormID`. A form of ours with no `books_` entry (a shell whose DiaryDB row was deleted, or a deleted-but-not-yet-saved form) shows the localized "all entries removed" page.

---

## Lifecycle

| Event | Action |
|---|---|
| `kDataLoaded` | Nothing form-related any more (the DPF check and its warning go). |
| Diary event / catch-up / recovery | `CreateDiaryBook`, synchronous. |
| `kPreLoadGame` / `kNewGame` (`EndSession`) | As today minus `CancelPendingCreations` and the claim table. |
| SKSE load callback | Resolve records → configure shells → loaded-shells map. |
| `kPostLoadGame` sync | `LoadFromDB` validates rows against the loaded shells; recreation for the rest; reconcile; recovery; catch-up. |
| Save | `FlushToDB`, then the `SNBF` records. The engine writes the forms' change records and every inventory entry itself. |
| Reset | DiaryDB rows, loaded-reference removal, `SetDelete` on the forms. |

---

## Migration from DPF

1. First launch without DPF: `Dynamic Persistent Forms.esp` is gone, DPF restores nothing, the old forms (ESP-space IDs) don't exist. Skyrim's "this save relies on content that is no longer present" prompt appears once per save, as for any removed plugin.
2. `LoadFromDB` finds no loaded shell for any row → deletes the rows and queues recreation. Every NPC gets new books with the same content. Catch-up and recovery already handle this.
3. **Books the player held are lost once** (stolen copies, world copies): the engine drops entries whose base form is missing. Theft records clear through the normal "NPC writes" path. Release notes.
4. `stolen_volumes` and `actor_templates` survive unchanged.
5. DiaryDB schema unchanged: `book_form_id` now holds `0xFF` IDs.
6. README: DPF no longer required; users may keep it for other mods.

---

## What gets deleted

- `BookCreation.cpp` as it is: the create queue, `PendingDiaryCreation`, `DPFCreateCallback`, `PumpDiaryCreateQueue`, the claim table, generations, `HasPendingCreations` and its waits in `VolumeSync`, `CancelPendingCreations`, collision retries, `ClearBogusSourceFiles`, `SanitizeLoadedBookForms` (the `0x1` `sourceFiles` were DPF's; factory forms have a null array, which `GetFile` handles).
- `CantTakeTimer` and the 5 s `kCantTake` delay, **if** the spike's take-and-save-immediately test passes (the crash it guarded against was DPF's async registration).
- The DPF-installed check and `DpfMissingText` (nine locale files).
- `persisted_in_save` handling, `QueueInventoryCheck`, `EnsureBookInInventory`, the Keep hand-back in reconcile (after the spike).
- Docs: BOOK_FORMS.md's DPF sections become history; KNOWN_ISSUES #1, #4 (VR `sourceFiles`), #5, #7 close.

---

## Failure modes

| Failure | Behaviour |
|---|---|
| Factory returns null | Log, skip; the volume is created next time it is needed. |
| A co-save record's form is missing or the wrong type | The engine renumbered a squatter or the record is stale: log at info, drop the record; `LoadFromDB` recreates the volume. |
| A form ends up with `kTemporary` set (unknown engine path) | `Create` clears it before `AddChange`; the spike checks the factory's default flags. |
| Co-save unreadable | No shells resolved → every row recreated. Same as the DPF migration. |
| Renumbered in-memory orphans | Harmless; they hold no inventory entries and are gone at exit. |

---

## The spike (do this first, on a throwaway branch)

Pass/fail on SE first (the RE was SE), then AE and VR.

1. Create a `TESObjectBOOK` with the factory, log its FormID (expect `0xFF...`), log `formFlags` (is `kTemporary` set?), `ConfigureDiaryForm` it, `AddChange(1)`, add it to an NPC by `AddObjectToContainer`.
2. Open it from the inventory; drop it and open it from the world; hover its item card (the `CNAM` path, null `sourceFiles`).
3. Save. **Restart the game.** Load. Expect: the NPC still holds it, it opens, `LookupByID` finds a Book with the same FormID, and the co-save record round-trips. Check the world copy after re-entering its cell.
4. Take the book and save within a second of creation; restart; load. (Decides the `kCantTake` delay.)
5. Load an **older** save from the same session: expect the form renumbered (log the new ID) and the row recreated. Start a **new game**: same.
6. `SetDelete(true)` on a book that is in an NPC's inventory and lying in another cell; save; restart; load. Expect both copies gone and no crash in between.
7. Steps 1–3 and 6 on AE and VR.

If step 3 fails, fact 2 doesn't hold on that runtime and the ESP design in git history (`BOOK_FORMS_PLAN.md` before 2026-09-28) is the fallback.

---

## Work breakdown

1. Spike (above).
2. `DynamicForms` module + `SNBF` co-save records + load callback configuring shells.
3. Synchronous `CreateDiaryBook`; remove the queue and everything listed above; `LoadFromDB` against loaded shells. Run the flows: new NPC, overflow volume, theft then new volume, catch-up on an existing save, load older save, new game.
4. Reset via `SetDelete`; drop `persisted_in_save`/`QueueInventoryCheck`/Keep hand-back; re-test the KEEP/CLEAR matrix in [VOLUMES_AND_SYNC.md](VOLUMES_AND_SYNC.md).
5. Text-provider hooks; `kCantTake` removal if step 4 of the spike passed.
6. Docs (BOOK_FORMS rewrite, ARCHITECTURE, DATABASE co-save section, VOLUMES_AND_SYNC, KNOWN_ISSUES, README requirements), locale strings, release notes.

---

## Open questions

- Does the type factory set `kTemporary`, or any flag that would keep the form out of the change map? (Spike step 1.)
- Is a `0xFF` base form's item card safe with a null `sourceFiles` on VR? DPF's crash was a garbage pointer (`0x1`), not null; `GetFile` returns null for a null array. (Spike step 2 on VR.)
- `SetDelete` on a base form that still has inventory copies this session: does anything in the engine object before the next save? (Spike step 6.)
- Memory: renumbered orphans across many loads in one session. Same as today with DPF; measure only if it shows.
