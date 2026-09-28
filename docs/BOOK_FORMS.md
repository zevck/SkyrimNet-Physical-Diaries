# Book Forms

Every diary volume is its own `TESObjectBOOK` base form, created at runtime by the engine's own form factory and saved by the engine itself. There is no plugin file for them and no Dynamic Persistent Forms (DPF, used until 2.0.0). This doc covers the engine behaviour that makes this work, how forms are created, filled in, matched to volumes and retired, the migration from DPF, and the alternatives that were rejected.

Code: `src/DynamicForms.cpp` (the generic mechanism, no SNPD types), `src/BookCreation.cpp` (`CreateDiaryBook`, `ConfigureDiaryForm`, `ConfigureLoadedBooks`), `src/BookManager.cpp` (`LoadFromDB`), `src/Serialization.cpp` (the co-save record), `src/ActorLookup.cpp` (`FindActorForBook`).

---

## Why one base form per volume

The engine offers exactly one identifier that is globally unique and survives inventory transfers, drops, pickups and save/load: the **base form's FormID**. SNPD needs to tell every volume apart (which NPC, which volume) everywhere the engine hands it an item: the book menu, `TESContainerChangedEvent`, inventory scans. So each volume needs its own base form. Per-instance data on a shared form was tested and does not work; see [Alternatives evaluated](#alternatives-evaluated).

---

## The engine behaviour this rests on

Reverse-engineered from SE 1.5.97 (Address Library IDs below), re-checked on the AE 1.6.1170 and VR 1.4.15 binaries, and run-tested on AE in a spike (2026-09-28). Not yet run in game on VR.

1. **Any form with a change record is written to the save.** `BGSSaveLoadGame::SaveGame` (SE ID 34676) writes one changed-form record per entry in its change map. `TESForm::AddChange` (14451) adds a form; its gate (34653) turns away only FormID 0, `kTemporary` forms and deleted non-reference dynamic forms. There is no form-type filter.
2. **On load the engine recreates the form before inventories resolve.** In `LoadGame` (34677), loop 1 (34681) creates a fresh form of the saved type through its factory for every record with an `0xFF` FormID and gives it the saved ID. Loop 2 runs each form's `LoadGame`; `InventoryChanges::LoadGame` (15903) looks up each entry's base form and **deletes the entry if it doesn't resolve**. Because loop 1 has finished, entries holding our books resolve. World copies are created when their cell attaches.
3. **The record holds only flags.** `TESObjectBOOK::SaveGame` (17440) writes form flags and the read/teaches byte. Name, model, type, text: nothing. SNPD re-applies those every session.
4. **The engine allocates the `0xFF` FormID.** `GetNextID` (13635) skips every ID in the all-forms map and in the loaded save's change map. `SetFormID` (14508) has no collision check, so a plugin must never choose IDs. The counter is not restored on load; the skip loop covers that.
5. **FormIDs are stable within a save's timeline, not across timelines.** `ClearForm` (34665) renumbers a dynamic form when the loaded save doesn't mention it (34644) or when another record of a different type claims its ID. A freed ID is reused at once (in the spike, a retired book's ID went to a Draugr).
6. **Timing.** SKSE's load callbacks run inside the load, after loop 2 (inventories resolved) and before `kPostLoadGame`. References in loaded cells have their 3D built before the load callback.
7. **The engine never drops a form with a change record on its own**, even if nothing holds it. The only way to keep one out of the save is its `CheckSaveGame` (vtable slot `0x0D`) returning false: the loop writes an entry even with zero change flags, so neither `RemoveChange` nor `SetDelete` does it.
8. **A world copy keeps its base form's raw FormID.** When a created reference is loaded (the per-record handler, AE `0x1406072F0`; the builder, AE `0x14060EE40`), the builder looks the base up and only checks that it is *some* bound object ("Bound object %08X no longer exists" otherwise). So **a form must never leave the save while a world copy may exist**: its freed FormID is reused at once (fact 5; leveled NPC bases are bound objects too), and the copy then loads with an unrelated base. Tested 2026-09-28: a Reset that dropped the forms of books lying in the world crashed every load of the save. Inventory entries are safe; the engine drops entries that aren't valid items (fact 2).

---

## Creation (`CreateDiaryBook`, game thread)

Synchronous: when it returns, the volume is in `books_` and the NPC holds the book.

1. Choose the template (`SelectJournalTemplate`) and find it by EditorID.
2. `DynamicForms::Create<TESObjectBOOK>()`: the type's factory creates the form (the engine assigns the FormID), `kTemporary` is cleared, and `AddChange(kFlags)` puts it in the change map.
3. `ConfigureDiaryForm`: from the template, `data.type` (must be a tome, `0x00`: a note scroll, `0xFF`, ignores `[pagebreak]`), world and inventory models, bounds, pickup/putdown sounds, keywords and item card. Then `weight = 0.5`, `value = 0`, no flags, and the name from `Localization::FormatBookName`. A factory form starts with none of these; without the world model and bounds a dropped book has no 3D and vanishes.
4. `DynamicForms::Track` the form's record (below), `RegisterBook`, `SetVolumeText`, then `FindActorForBook` and `AddObjectToContainer`.

A factory failure is logged and the volume is created the next time it is needed. There is no queue, no claim table and no `kCantTake` delay: those guarded DPF's asynchronous creation and its allocator (see [Alternatives evaluated](#dpf-used-until-200)).

---

## The co-save record

The save keeps only a form's flags (fact 3), so SNPD records what each form is. `DynamicForms` keeps the list of tracked forms and writes all of it at every save, as one `'SNBF'` record (version 2) under the `'SNDB'` unique ID. Per form: FormID, form type, flags (retired), key, template EditorID, display name. The key is `"<actor UUID>|v<volume>"` (`VolumeKey`).

The list is filled at creation and by the load callback, not from `books_`, so a save made before the post-load sync has loaded the volumes (during SkyrimNet's keep/clear prompt, or with diary books paused) still keeps every book's record. A book in the save without its record would load as an empty shell with nothing to fill it in.

---

## Load

1. **Load callback** (`Serialization.cpp`): `DynamicForms::Load` reads the record and tracks each form that still exists with its saved type. A form that is missing or has another type was renumbered by the engine (fact 5) and is dropped. Then `ConfigureLoadedBooks` fills each book in from its record, so books look right before DiaryDB is open.
2. **`kPostLoadGame`**: `DynamicForms::RebuildLoadedWorldCopies` disables and re-enables every world copy of a tracked form in the loaded cells. Their 3D was built while the form was still an empty shell (fact 6), so they were invisible. Copies in cells that load later need nothing.
3. **Post-load sync, `LoadFromDB`** matches DiaryDB's rows against the tracked forms by key. DiaryDB is shared by every save of the character, but a form belongs to one save's timeline, so **the save's record, not the row, says which form is the volume in this save**:
   - Row with a tracked form for its volume → load it (with two, the one the row names). If the row names a different FormID (a book created in another branch of saves), the row is updated. The look is re-applied from DiaryDB (the actor's name or the language may have changed) and the record kept in step.
   - Row with no tracked form → the volume has no book in this save (created after it, or in another timeline): delete the row and queue the actor for recreation from SkyrimNet's entries.
   - Tracked form with no row → **unclaimed**, left where it is. A missing row is not proof of a Reset: DiaryDB may have been lost or recreated empty, or the row may have been deleted by loading another save moments ago. The book shows the "all entries removed" page, is logged with its key and FormID, and **if its volume is created again, `CreateDiaryBook` reuses it** (and doesn't give it to the NPC again: the NPC still has it, or it was taken). So a lost DiaryDB, a quick switch between saves, or a Reset followed by loading an older save all end with the same books, rebuilt.
   - Retired forms match no row; they get their "all entries removed" page.

A reload without saving needs nothing special: forms created after the loaded save aren't in it, so their rows are recreated, and the in-memory forms (renumbered by the engine) hold no inventory entries.

---

## Retirement

A book whose volume is gone is **retired, never removed from the save** (fact 8). `BookManager::RetireBook` flags its record (`DynamicForms::Retire`; the flag is saved) and gives the book the "all entries removed" page under the diary's title (`FormatDiaryEntries` with no entries), served from the text snapshot. The form stays valid forever, so every copy of it, in any cell or container, stays a valid book. Each retired book costs one small change record and one co-save record.

A retired book must never be seen, so its copies are removed wherever they can be reached (a copy is deleted with `Disable` + `SetDelete` on its reference, or removed from an inventory):
- **`SweepRetiredBooks`**, after every retirement and at every load (end of `LoadFromDB`): the loaded cells (book, container and NPC references only; inventories are read without initializing them, so unopened containers keep their loot unrolled), each book's owner NPC (found by the UUID in its key; a persistent NPC is in memory wherever they are) and every merchant chest (vendor factions' `merchantContainer`: always in memory, and barter reads them without loading their cell).
- **The sweeper sink** (`TESCellAttachDetachEvent`, only while any book is retired): as each book, container or NPC reference attaches with its cell, a task removes retired books from it. The player can't reach a copy before its cell loads.

What's left are forms nothing can see: a few hundred bytes each in the save. A copy that escapes both (another mod handing one to the player) opens to the "all entries removed" page.

Only certain evidence retires a book:
- **MCM Reset** (`ResetAllDiariesInternal`): every tracked form, after memory is cleared.
- **`UnregisterVolumesFrom`**: volumes `ReconcileWithTimeline` drops after a Clear.

---

## Templates

Four EditorIDs, declared once in `BookManager.h` (`kJournalTemplates`: `SkyrimNetDiaryTemplate`, `…2`, `…3`; `kNightingaleTemplate`: `SkyrimNetDiaryTemplateN`) and used by both `SelectJournalTemplate` and the `kDataLoaded` check. The ESP holds only these templates; SNPD needs no other plugin.

`SelectJournalTemplate`: Karliah, Gallus and Mercer Frey get the Nightingale template. They are matched by the actor's base NPC (`Skyrim.esm` `0x1B07F`, `0x1BB5D`, `0x1B07C`), never by name, so it works in every language and doesn't catch same-named NPCs. Everyone else gets `variants[hash(uuid) % count]`, so the choice is stable across reloads. It is cached in `actorTemplates_` and stored in DiaryDB `actor_templates`, so every volume of an NPC looks the same.

Templates are found with `LookupByEditorID`, which works with powerofthree's Tweaks or Native EditorID Fix. `kDataLoaded` checks all four the same way and shows a message box if any are missing. Don't compare `GetFormEditorID()` on the form itself: without Native EditorID Fix it returns "" for books, so a scan finds nothing even though the lookup succeeds.

---

## Finding the NPC (`FindActorForBook`)

The SkyrimNet UUID is the identity. FormIDs are never trusted on their own, because non-persistent references reuse them across sessions and ESL load-order changes shift them.

1. Player diary (`player_special` bio template, UUID, or FormID `0x14`) → the player.
2. No UUID → reject. An unverifiable FormID could resolve to the wrong NPC.
3. Cache hit by UUID.
4. **Tier 1:** UUID → live FormID via SkyrimNet (`GetFormIDForUUID`).
5. **Tier 2:** the stored FormID, accepted only if SkyrimNet maps it back to the same UUID (reverse-lookup check).

Only hits are cached (`g_actorCacheByUuid`, keyed by UUID), so a miss retries next time. The cache is cleared each load.

---

## Migration from DPF (2.0.0)

- The first load of an older save has no `'SNBF'` record and none of its DiaryDB rows match a tracked form, so every volume is recreated with the same content and a new `0xFF` FormID. Catch-up and recovery already handle this.
- Without `Dynamic Persistent Forms.esp` the old forms don't exist, so the engine drops their inventory entries: **copies the player held (stolen or dropped) are lost once.** Theft records clear through the normal "the NPC writes" path. Skyrim may warn once per save that it relies on content no longer present.
- A user who keeps DPF for another mod gets the old diary forms restored by DPF in older saves, as blank books nothing claims.
- DiaryDB's schema is unchanged apart from the unused `persisted_in_save` column, which old DBs keep. `stolen_volumes` and `actor_templates` carry over.

---

## Failure modes

| Failure | Behaviour |
|---|---|
| Factory returns null | Logged; the volume is created the next time it is needed. |
| A record's form is missing or of another type | The engine renumbered ours (fact 5): logged at info, record dropped; `LoadFromDB` recreates the volume. |
| Co-save record unreadable or truncated | The forms read so far are kept; the rest behave like missing forms. |
| DiaryDB lost, recreated empty, or detected for the wrong save | The save's books are unclaimed, not retired; the catch-up recreates the volumes and reuses them. Nothing the player holds is taken. |
| DiaryDB can't be opened | Diary books pause for the session; the tracked forms stay tracked and are saved as they are. |
| Renumbered in-memory orphans | Harmless: they hold no inventory entries and are gone at exit. |
| A form dropped from the save while a world copy exists | Crash on load (fact 8). SNPD never drops its forms; don't add a path that does. |

---

## Reuse in other mods

`DynamicForms.h/.cpp` has no SNPD types: any form type, any owner key. A letters or quest-reward mod copies it, writes the record from its own co-save callbacks, retires forms it no longer needs (never removes them), and fills its forms in from their records on load. The book-text side (`BookTextHook`) is separate and book-specific.

---

## Alternatives evaluated

Recorded so they are not re-investigated from scratch.

### DPF (used until 2.0.0)

DPF cloned a template into an ESP-space form and restored it from its own co-save. Two DPF bugs shaped SNPD's old pipeline: its FormID allocator was **not thread-safe** (burst creation handed two NPCs the same FormID: cross-linked diaries), fixed with a serial create queue; and `Dispose()` put records in a recycle pool that gathered **duplicates** over reset/reload cycles, fixed with a FormID claim table and never calling `Dispose()`. Creation was asynchronous (a Papyrus VM callback), so volumes were "pending" for seconds and every "which volumes does this actor have" decision had to wait for them; a `kCantTake` flag for 5 s guarded a crash when a book was taken and saved before DPF had registered it. On VR, clones had `sourceFiles.array == 0x1`, which crashed `GetFile`. DPF forms also survived a reload without saving in memory, which needed `persisted_in_save` and an inventory re-add. All of that is gone.

### Dropping retired forms from the save — rejected (2026-09-28)

The first design retired a book by leaving it out of the save: a `TESObjectBOOK::CheckSaveGame` vtable filter returned false for a session-only retired set, so the next load dropped every copy (the spike confirmed it for inventory copies). A Reset with a diary lying on the ground then crashed every load of the save: the world copy kept the freed FormID (fact 8). The engine can't tell a stale base ID from a new form that took it, so no hook on the load side fixes this either. `SetDelete` and `RemoveChange` don't keep a form out of the save at all (fact 7).

### Per-instance identity on one shared base form — rejected

The idea: one diary base form, with per-instance data telling the volumes apart. Results from an in-game probe (July–August 2026):

| `ExtraDataList` field | Save → reload | Transfer to another actor | Drop to world | Drop then pick up |
|---|---|---|---|---|
| `ExtraUniqueID.uniqueID` | kept | **reset** to that container's counter (61441 → 1) | reset to 0 | **removed entirely** |
| `ExtraUniqueID.baseID` | rewritten to the holding container | rewritten to the new container | rewritten to the world REFR | — |
| `ExtraTextDisplayData` (display name) | kept | kept | kept | kept |

- The engine does not give ordinary items an `ExtraUniqueID` at all, so there is nothing to read. A self-written one has to be maintained.
- `uniqueID` is a small **per-container counter** (`InventoryChanges::GetNextUniqueID`), not a global ID. `baseID` is the **container**, not the item's base form. `TESUniqueIDChangeEvent` reports resets, but a missed event silently loses a diary's identity, and a drop/pickup deletes the data outright.
- The display name survives everything, but using it as a key means parsing names, which is what `DiaryTheftHandler` moved away from.
- Inventory items are not references (`TESObjectREFR` exists only while an item is in the world), so "mutate the reference" does not work either.

### Pre-generated pool of book records in an ESL — rejected

An ESL holds about 2,048 records. SkyrimNet can write a diary for any NPC the player talks to, so the count is unbounded and a pool would eventually run out.

### DPF RE ("Dynamic Persistent Forms RE::thinked") — rejected 2026-09-27 (source read)

It keeps slot identity (plugin + local FormID + type + owner + key) in a global `Data/SKSE/Plugins/DPF_Cache.bin`. `GetOrCreateByOwnerKey` gives a **stable, not deterministic** FormID: the next free one in request order, remembered only in that file, so another machine or a lost cache file gives a key a different ID while old saves still reference the previous one. Forms come back empty; there is no co-save and no load hook, so the consumer must create every form at `kDataLoaded` and re-apply its data each session. `SaveGlobalRegistry()` rewrites the whole file on every create, fetch and release. `ReleaseByOwnerKey` is global, so a Reset in one playthrough would free slots another playthrough's saves still reference. Version 0.1.0. What it would provide is an allocator and a registry file; the engine already provides both (facts 1 to 5).
