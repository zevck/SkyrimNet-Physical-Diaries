# Theft, Return and Handover

A diary can be pickpocketed or stolen. The next time that NPC writes an entry, SkyrimNet's prompt knows the diary is missing, and the NPC writes about it. Returning the diary before then removes the record. Taking a diary openly, through the dialogue trade menu (e.g. from a follower), is not theft and sends a separate event instead.

Code: `src/DiaryTheftHandler.cpp`, `src/PapyrusAPI.cpp` (`UpdateDiaryForFormID`), `DiaryTheftHandler::ReconcileAfterLoad` (called from `kPostLoadGame`) and `RegisterStolenDecorator` (called once at `kDataLoaded`, both in `src/main.cpp`), DiaryDB `stolen_volumes`.

---

## Detection

`DiaryTheftHandler::Register()` adds one `TESContainerChangedEvent` sink and one `MenuOpenCloseEvent` sink.

The menu sink tracks only whether the **Dialogue Menu**, **Console** and **ContainerMenu** are open. A container opened while dialogue is open and the console is not counts as a legitimate trade. (Reading the dialogue speaker would need `MenuTopicManager::speaker`, whose address is missing from the VR Address Library, so the handler avoids it.)

`ContainerChangeHandler::ProcessEvent`, for book transfers into or out of the player:

1. `BookManager::GetBookForFormID(baseObj)`: not one of our volumes → ignore. Book names are never looked at (they're localized, and same-named NPCs share them).
2. The NPC on the other side of the transfer must be alive (looting a corpse is not theft), and must **own** the volume: its live UUID (`GetUUIDFromFormID`) must equal the volume's UUID. If SkyrimNet doesn't know the NPC, or it's someone else's diary, nothing is recorded.

**Player receives a diary:**
- **Theft** = the instance in the player's inventory has `ExtraDataType::kOwnership`, the engine's stolen marker. Record it: `DiaryDB::AddStolenVolume(uuid, volume, gameTime)`. Every volume counts, not only the latest.
- No ownership data, but a legitimate trade is under way → send ModEvent **`PhysicalDiary_Shared`** (`strArg` = book name, `numArg` = the actor's FormID as a float, sender = the actor), so a SkyrimNet trigger can have the NPC react to the player taking their diary.
- Neither → ignore (for example, console `additem`).

**Player gives a diary to its owner:** `RemoveStolenVolume(uuid, volume)`. Giving someone else's diary to an NPC does not clear anything (the owner check above).

## How SkyrimNet learns about it

- Decorator **`snpd_diary_stolen`**, a **native** decorator (`PublicRegisterDecorator`, SkyrimNet public API v5+, through `Database::RegisterDecorator`): actor → UUID → `"true"` if the actor has **any** row in `stolen_volumes`, else `"false"` (`""` for no actor). SkyrimNet calls it on its worker threads, so it never touches DiaryDB (closed and reopened on every load): it reads an in-memory set of stolen UUIDs behind a mutex (`DiaryTheftHandler::IsDiaryStolen`). `SyncStolenCache()` reloads the set from DiaryDB after every change (theft, return, the "wrote about it" clear, load reconciliation, Reset), and `ClearStolenCache()` empties it when a session ends. SNPD ships it as a SkyrimNet Beta 25 plugin, an external layer at `SKSE/Plugins/SkyrimNet/external/zevick.physical-diaries/` (`manifest.json` plus `prompts/submodules/system_head/0500_diary_stolen.prompt`):
  ```
  {% if render_mode == "full" and rendering_diary %}
  {% if snpd_diary_stolen(npc.UUID) == "true" %}
  **IMPORTANT**: You notice that your diary, which you always keep on you, has been stolen! Write about your reaction in a new volume.
  {% endif %}
  {% endif %}
  ```
  **Why `system_head`, and what it depends on:** the diary prompt renders only `system_head` (in `"full"` mode) and the event history; `user_final_instructions` is never part of it. `character_bio` is reached too, but through `render_character_profile`, whose 55 s render cache is shared with memory generation (`rendering_diary` isn't in SkyrimNet's cache key), so text there appears only sometimes. The outer `render_subcomponent("system_head", "full")` call is shared only with dialogue, which always sets a response target and so never matches the diary's cache key. `rendering_diary` keeps the line out of dialogue. This relies on SkyrimNet's current prompt layout (the diary is the only `"full"`-mode `system_head` render with no response target, and diaries render in `"full"` mode): a SkyrimNet change there would silently drop the line. The proper fixes are upstream (SkyrimNet #283 / #979): `rendering_diary` in the render cache key and a diary-only submodule; if the submodule arrives, move the file there. Decorator results and the `system_head` block are cached for 55 s, so two diaries for the same NPC within 55 s around a theft can get the old answer. Verified in game on 2026-09-27. Bump `version` in `manifest.json` whenever the prompt changes.
- It is registered once, at `kDataLoaded`; native registrations survive loads. Until 2026-09-27 it was a Papyrus decorator (`SkyrimNetDiaries_Decorators.IsDiaryStolen`), registered by the event listener's `OnInit` and again through the Papyrus VM on every load, because SkyrimNet drops Papyrus decorator registrations on load. Papyrus decorators are slow (a VM round trip, run in bulk for every nearby actor by SkyrimNet's prefetch, and skipped while the game is paused), and SkyrimNet plans to retire them. The script is gone; old saves are unaffected.
- **Until 2026-09-26 this never worked.** `SkyrimNetDiaries_API.psc/.pex` had never shipped, so the VM could not bind the natives and `IsDiaryStolen` returned None. The C++ recorded thefts correctly; the prompt never saw them. (That script, and its `GetDiaryTheftStatus`/`SetTheftCleared` natives, were removed on 2026-09-27; see [PAPYRUS_AND_API.md](PAPYRUS_AND_API.md).)

## Clearing

- **The NPC wrote about it:** `UpdateDiaryForFormID`, which the event listener's native calls for every new entry, runs `ClearAllStolenVolumes(uuid)` **before** processing the entry (during a load, both wait for the post-load sync, so the clear lands in the right save's DB). The decorator was already evaluated while SkyrimNet generated that entry, so the NPC wrote about the theft once and the record is then cleared. This is done in C++ because Papyrus `Game.GetForm` returns None for NPCs in unloaded cells, which silently skipped the old Papyrus-side clear.
- **Returned in time:** the `HandleDiaryReturned` path above.

## Save reverts

At `kPostLoadGame`, `ReconcileAfterLoad` deletes every theft record whose `stolen_at` is later than the loaded save's game time (`DiaryDB::RemoveStolenVolumesAfter`): that theft happened in a timeline the player has left. Thefts from before the save stand, so a save made while carrying a stolen diary still has it counted. Until 2026-09-27 this compared a per-actor `last_known_game_time` stamp instead and cleared **all** of an actor's thefts on any load of an earlier save.

## Related behaviour

- A stolen volume is never updated again. When the NPC writes their next entry, `UpdateDiaryForActorInternal` sees they no longer hold their latest volume and starts a new one. See [VOLUMES_AND_SYNC.md](VOLUMES_AND_SYNC.md).
- A stolen book stays where the save put it: inventories are the engine's `.ess` state, and SNPD never re-adds a book to the NPC on load. See [BOOK_FORMS.md](BOOK_FORMS.md#load).
- The ESP still has `SNPD_DiaryStolenFaction` from an earlier faction-based design. Nothing uses it; it is harmless and left in place. (An even earlier spell-based design looked up `SNPD_DiaryStorageSpell`, which is not in the ESP; its natives were removed on 2026-09-26.)
