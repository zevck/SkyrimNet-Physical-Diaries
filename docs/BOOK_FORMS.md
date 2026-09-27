# Book Forms

Every diary volume is its own `TESObjectBOOK` base form, created at runtime by Dynamic Persistent Forms (DPF) as a clone of a template book from the ESP. This doc covers how those forms are created, configured, attached to NPCs and kept valid across loads, and records the alternatives that were tested and rejected.

Code: `src/BookCreation.cpp` (`CreateDiaryBook`, `PumpDiaryCreateQueue`, `DPFCreateCallback`, the FormID claim table, `ClearBogusSourceFiles`), `src/ActorLookup.cpp` (`FindActorForBook`), `src/BookManager.cpp` (`EnsureBookInInventory`, `LoadFromDB`).

---

## Why one base form per volume

The engine offers exactly one identifier that is globally unique and survives inventory transfers, drops, pickups and save/load: the **base form's FormID**. SNPD needs to tell every volume apart (which NPC, which volume) everywhere the engine hands it an item — the book menu, `TESContainerChangedEvent`, inventory scans. So each volume needs its own base form. Per-instance data on a shared form was tested and does not work; see [Alternatives evaluated](#alternatives-evaluated).

---

## Creation pipeline

1. **`CreateDiaryBook`** chooses the template (`SelectJournalTemplate`), finds it by EditorID, and pushes a `PendingDiaryCreation` (the whole request: actor, volume, entries, template, boundary data) onto a global queue. Creation is asynchronous.
2. **`PumpDiaryCreateQueue`** dispatches the next request only when none is in flight: `DispatchStaticCall("DynamicPersistentForms", "Create", template, DPFCreateCallback)`.
3. **`DPFCreateCallback::operator()`** runs on a Papyrus VM thread. It claims the FormID (below), then releases the in-flight slot, queuing a collision retry at the **front** of the queue in the same locked step so the next pump can't skip it, and queues the next pump.
4. **`CompleteCreation`** runs on the game thread: configure the form, `RegisterBook`, write the text (`SetVolumeText`), then find the NPC and `AddObjectToContainer`. It looks the form up again by FormID rather than keeping a pointer across threads; the 5-second `kCantTake` timer does the same.

### Pending creations and cancellation

A volume only appears in `books_` once step 4 runs, which can be seconds after it was queued (a catch-up burst queues hundreds). Anything that decides "which volumes does this actor have" in that window would create them again, which is how an NPC ended up with two "volume 1" books. So:

- **Pending count per UUID.** `CreateDiaryBook` counts a volume as pending until it is registered or given up on. `HasPendingCreations(uuid)` makes `UpdateDiaryForActorInternal` wait (it re-runs itself 500 ms later) and makes the catch-up scan skip the actor.
- **Generation.** Every request carries the generation it was queued in. `CancelPendingCreations()`, called at `kPreLoadGame` and by MCM Reset, drops the queue, clears the pending counts, resets the in-flight slot and bumps the generation. A callback or completion from an older generation is discarded instead of registering an old volume into the new state, and it leaves the in-flight slot alone because the new session may already have a `Create()` in flight.
- A failed dispatch releases the slot and pumps the next request, so the queue never stalls.

### The two DPF bugs this pipeline works around

These are the root causes of what used to show up as "DPF losing records" and cross-linked diaries (opening one NPC's diary showed another NPC's text).

| Bug | Symptom | Defence |
|---|---|---|
| DPF's FormID allocator is **not thread-safe** | Burst creation (the catch-up scan queues one task per actor, each making several volumes) handed concurrent callbacks the **same** FormID | The serial create queue. Only one `Create()` is ever in flight. |
| DPF **recycles duplicate deleted records**. `Dispose()` puts a record in DPF's recycle pool, and over repeated reset/reload cycles the persisted pool gathers duplicates of the same FormID, which get handed out more than once. | A new volume received a FormID an existing diary already owned | The **FormID claim table** (`g_claimedFormIds`, FormID → owning UUID). The callback claims each FormID synchronously. If it is already claimed by a different UUID, the form is abandoned and the request re-queued, up to `kMaxCreateRetries` (16). DPF's next allocation consumes the duplicate slot. `LoadFromDB` seeds the table with every loaded volume; `ClearActorCache` clears it each load. |

Because of the second bug, **MCM Reset deliberately does not call `DPF.Dispose()`** on the books it removes (see `ResetAllDiariesInternal` in `VolumeSync.cpp`). Don't add it back.

### Configuring a new form (game thread)

From the template: `data.type` (must be a book tome, `0x00`. A note scroll, `0xFF`, ignores `[pagebreak]`), `inventoryModel` and `itemCardDescription`. Then `weight = 0.5`, `value = 0`, flags cleared and `kCantTake` set, and the name from `Localization::FormatBookName`.

- **Don't touch `data.teaches`.** Clearing or nulling it crashed DPF's serializer on save. The template's clean value is left alone.
- **Invalid `sourceFiles` (VR crash fix, untested on VR as of 2026-09-26).** On VR, DPF clones have been seen with `sourceFiles.array == 0x1`. That hard-crashes `TESForm::GetFile` whenever anything calls `GetDescription` (item card refresh, Description Framework, save serialization). On SE/AE the value is `nullptr`, which `GetFile` null-checks. `ClearBogusSourceFiles` resets any value inside the first 64 KB (never a real pointer) to `nullptr`, the SE/AE state. It leaves valid pointers alone, so SE/AE behaviour does not change. It runs on each new clone and, through `SanitizeLoadedBookForms`, on **every** book form at the start of `kPostLoadGame`. That covers the forms DPF restores from its co-save without our callback, **including ones DiaryDB no longer tracks** (Reset orphans, rebuilt volumes, loads where the DB failed to open).
  - An earlier version instead aliased the template's `sourceFiles` onto each clone, and only for tracked rows. Review rejected it: it missed untracked forms, changed SE/AE (descriptions resolved to the template's "This is a placeholder diary.") and made two forms share one engine allocation.
  - Not yet tested on VR. Open question: where the `0x1` comes from (DPF's copy path is a suspect).
- **`kCantTake` for 5 seconds.** A detached thread sleeps 5 s and then queues the flag clear. Taking the book and saving immediately after creation crashed before DPF had finished registering the form. One sleeping thread per book is wasteful during bulk creation (see [KNOWN_ISSUES.md](KNOWN_ISSUES.md)).

---

## Templates

Four EditorIDs, declared once in `BookManager.h` (`kJournalTemplates`: `SkyrimNetDiaryTemplate`, `…2`, `…3`; `kNightingaleTemplate`: `SkyrimNetDiaryTemplateN`) and used by both `BookManager::Initialize` and the `kDataLoaded` check.

`SelectJournalTemplate`: Karliah, Gallus and Mercer Frey get the Nightingale template. They are matched by the actor's base NPC (`Skyrim.esm` `0x1B07F`, `0x1BB5D`, `0x1B07C`), never by name, so it works in every language and doesn't catch same-named NPCs. Everyone else gets `variants[hash(uuid) % count]`, so the choice is stable across reloads. It is cached in `actorTemplates_` and stored in DiaryDB `actor_templates`, so every volume of an NPC looks the same.

Templates are found with `LookupByEditorID`, which works with powerofthree's Tweaks or Native EditorID Fix. `kDataLoaded` checks all four the same way and shows a message box if any are missing. Don't compare `GetFormEditorID()` on the form itself: without Native EditorID Fix it returns "" for books, so a scan finds nothing even though the lookup succeeds. Before 2026-09-26 `CreateDiaryBook` did exactly that, and every creation failed on a setup with only powerofthree's Tweaks.

---

## Finding the NPC (`FindActorForBook`)

The SkyrimNet UUID is the identity. FormIDs are never trusted on their own, because non-persistent references reuse them across sessions and ESL load-order changes shift them.

1. Player diary (`player_special` bio template, UUID, or FormID `0x14`) → the player.
2. No UUID → reject. An unverifiable FormID could resolve to the wrong NPC.
3. Cache hit by UUID.
4. **Tier 1:** UUID → live FormID via SkyrimNet (`GetFormIDForUUID`).
5. **Tier 2:** the stored FormID, accepted only if SkyrimNet maps it back to the same UUID (reverse-lookup check).

Only hits are cached (`g_actorCacheByUuid`, keyed by UUID), so a miss retries next time. The cache is cleared each load.

History: older builds matched NPCs by name plus the last three hex digits of the reference FormID over a `ProcessLists` sweep. That is gone. The `bioTemplate` parameter now serves only the player check.

---

## Keeping forms valid across loads

DPF restores its forms from its own co-save before `kPostLoadGame`. `LoadFromDB` then checks each DiaryDB row:

- The book FormID no longer resolves to a book → delete the row and queue the actor for recreation. This happens when the volume was created after the loaded save (never saved, or saved only in a later save).
- The form exists but `persisted_in_save = 0` → a reload without saving; the volume loads normally and `QueueInventoryCheck` puts the book back.
- Otherwise → claim the FormID, load into `books_`, and **re-apply the volume's look** (`ConfigureDiaryForm`: name, template model, book type, item card). DPF hands the same FormIDs out again in later sessions, and a save made in an earlier session restores its own data for that FormID, including another NPC's name (seen: Katarina's volume showing as "Svala's Diary" while opening Katarina's text). DiaryDB is authoritative, so the look is reset on every load and a corrected name is logged. (`sourceFiles` was already fixed by the load sweep.)

`QueueInventoryCheck` then re-adds books to NPCs, but **only for volumes not yet in a save** (`persisted_in_save = 0`). That covers reload-without-save, where the DPF form survived in memory but the inventory entry did not. Volumes that were in a save are left alone, because their inventory state in the `.ess` is authoritative and re-adding would give a pickpocketed diary back.

---

## Alternatives evaluated

Investigated July–August 2026, partly with an in-game probe (since removed). Recorded here so they are not re-investigated from scratch.

### Per-instance identity on one shared base form — rejected

The idea: one diary base form, with per-instance data telling the volumes apart. Results from the probe:

| `ExtraDataList` field | Save → reload | Transfer to another actor | Drop to world | Drop then pick up |
|---|---|---|---|---|
| `ExtraUniqueID.uniqueID` | kept | **reset** to that container's counter (61441 → 1) | reset to 0 | **removed entirely** |
| `ExtraUniqueID.baseID` | rewritten to the holding container | rewritten to the new container | rewritten to the world REFR | — |
| `ExtraTextDisplayData` (display name) | kept | kept | kept | kept |

- The engine does not give ordinary items an `ExtraUniqueID` at all, so there is nothing to read. A self-written one has to be maintained.
- `uniqueID` is a small **per-container counter** (`InventoryChanges::GetNextUniqueID`), not a global ID. `baseID` is the **container**, not the item's base form. `TESUniqueIDChangeEvent` reports resets, so resets during transfers could be caught and undone, but a missed event silently loses a diary's identity, and a drop/pickup deletes the data outright.
- The display name survives everything, but using it as a key means parsing names, which is what `DiaryTheftHandler` moved away from.
- Inventory items are not references (`TESObjectREFR` exists only while an item is in the world), so "mutate the reference" does not work either.

### Pre-generated pool of book records in an ESL — rejected

Real ESP records avoid every DPF problem (no allocator, no ordering hazard, working `GetFile`). An ESL holds about 2,048. SkyrimNet can write a diary for any NPC the player talks to, and lists add NPCs (Bruma, follower mods), so the count is unbounded and a pool would eventually run out. One book per NPC instead of per volume would shrink the pool, but volumes are needed to keep books readable.

### DPF RE ("Dynamic Persistent Forms RE::thinked") — open option

It keeps only slot identity (plugin + local FormID + type + owner + key) in a global `Data/SKSE/Plugins/DPF_Cache.bin`, not full form records in the co-save. It has a C++ API (`DPFAPI.h`, `GetDPFAPI` export) as well as Papyrus.

- `GetOrCreateByOwnerKey(owner, key, type, …)` gives a **deterministic** FormID for `(uuid, volume)`, which matches `PRIMARY KEY (actor_uuid, volume_number)`. `book_form_id` would become a cache rather than the source of truth, and most of the `LoadFromDB` validation would go away.
- Slots are registered lazily: loading the registry creates no forms (`RegisterDynamicSlot` is bookkeeping only). Unused slots cost a map entry.
- Keys can be unscoped (`<uuid>|v<n>`). A slot is an address, not ownership: whether an NPC holds the book is `.ess` state, and the text always comes from the active save's DiaryDB. Playthroughs stay separate.
- **Never release slots on Reset.** `ReleaseByOwnerKey` is global, so a Reset in one playthrough would free slots another playthrough's `.ess` still references, and those inventory entries could not be repaired. Reset should drop DiaryDB rows and inventory entries only.
- Forms come back empty. Populating them every session (template properties, name) is SNPD's job. It is what `LoadFromDB` already does.
- `Dispose` does not exist in DPF RE; `Create(Form)` keeps its signature. Not save-compatible with old DPF forms, which does not matter here: content is derived, so a Reset rebuilds everything.
- To check before adopting: `SaveGlobalRegistry()` rewrites the whole registry file on **every** create or fetch, and bulk creation makes hundreds; whether the owner/key path is thread-safe; whether forms exist early enough for `.ess` inventory entries to resolve on load (untested).
