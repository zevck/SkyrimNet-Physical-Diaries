# Theft, Return and Handover

A diary can be pickpocketed or stolen. The next time that NPC writes an entry, SkyrimNet's prompt knows the diary is missing, and the NPC writes about it. Returning the diary before then removes the record. Taking a diary openly, through the dialogue trade menu (e.g. from a follower), is not theft and sends a separate event instead.

Code: `src/DiaryTheftHandler.cpp`, `src/PapyrusAPI.cpp` (`IsDiaryStolen`, `GetDiaryTheftStatus`, `SetTheftCleared`, `UpdateDiaryForFormID`), `DiaryTheftHandler::ReconcileAfterLoad` and `RegisterStolenDecorator` (called from `kPostLoadGame` in `src/main.cpp`), DiaryDB `stolen_volumes`.

---

## Detection

`DiaryTheftHandler::Register()` adds one `TESContainerChangedEvent` sink and one `MenuOpenCloseEvent` sink.

The menu sink tracks only whether the **Dialogue Menu**, **Console** and **ContainerMenu** are open. A container opened while dialogue is open and the console is not counts as a legitimate trade. (Reading the dialogue speaker would need `MenuTopicManager::speaker`, whose address is missing from the VR Address Library, so the handler avoids it.)

`ContainerChangeHandler::ProcessEvent`, for book transfers into or out of the player:

1. `BookManager::GetBookForFormID(baseObj)`: not one of our volumes → ignore. Book names are never looked at (they're localized, and same-named NPCs share them).
2. The NPC on the other side of the transfer must be alive (looting a corpse is not theft), and must **own** the volume: its live UUID (`GetUUIDFromFormID`) must equal the volume's UUID. If SkyrimNet doesn't know the NPC, or it's someone else's diary, nothing is recorded.

**Player receives a diary:**
- **Theft** = the instance in the player's inventory has `ExtraDataType::kOwnership`, the engine's stolen marker. Record it: `DiaryDB::AddStolenVolume(uuid, volume, gameTime)` and update `last_known_game_time`. Every volume counts, not only the latest.
- No ownership data, but a legitimate trade is under way → send ModEvent **`PhysicalDiary_Shared`** (`strArg` = book name, `numArg` = the actor's FormID as a float, sender = the actor), so a SkyrimNet trigger can have the NPC react to the player taking their diary.
- Neither → ignore (for example, console `additem`).

**Player gives a diary to its owner:** `RemoveStolenVolume(uuid, volume)`. Giving someone else's diary to an NPC does not clear anything (the owner check above).

## How SkyrimNet learns about it

- Decorator **`snpd_diary_stolen`** → `SkyrimNetDiaries_Decorators.IsDiaryStolen(actor)` → native `SkyrimNetDiaries_API.IsDiaryStolen` → `"true"` if the actor's UUID has **any** row in `stolen_volumes`. The user adds this to SkyrimNet's `diary_entry.prompt` (see the README's Installation section):
  ```
  {% if snpd_diary_stolen(npc.UUID) == "true" %}
      **IMPORTANT**: You notice that your diary has been stolen! Write about your reaction.
  {% endif %}
  ```
- The decorator is registered twice: by the event-listener quest's Papyrus `OnInit` (new game only), and from C++ on **every** `kPostLoadGame`, because SkyrimNet clears all decorator registrations on load.
- **Until 2026-09-26 this never worked.** `SkyrimNetDiaries_API.psc/.pex` had never shipped, so the VM could not bind the natives and `IsDiaryStolen` returned None. The C++ recorded thefts correctly; the prompt never saw them. Reported by a user and fixed by shipping the script.

## Clearing

- **The NPC wrote about it:** `UpdateDiaryForFormID`, which the event listener's native calls for every new entry, runs `ClearAllStolenVolumes(uuid)` **before** processing the entry (during a load, both wait for the post-load sync, so the clear lands in the right save's DB). The decorator was already evaluated while SkyrimNet generated that entry, so the NPC wrote about the theft once and the record is then cleared. This is done in C++ because Papyrus `Game.GetForm` returns None for NPCs in unloaded cells, which silently skipped the old Papyrus-side clear.
- **Returned in time:** the `HandleDiaryReturned` path above.
- **Public API:** `SkyrimNetDiaries_API.SetTheftCleared(actor)`.

## Save reverts

`last_known_game_time` (per actor, in `actor_templates`) is stamped on theft, on return and at every save. At `kPostLoadGame`, if the current game time is **earlier** than an actor's stamp, the player has loaded an earlier save and the theft may never have happened in this timeline, so that actor's stolen volumes are cleared.

## Related behaviour

- A stolen volume is never updated again. When the NPC writes their next entry, `UpdateDiaryForActorInternal` sees they no longer hold their latest volume and starts a new one. See [VOLUMES_AND_SYNC.md](VOLUMES_AND_SYNC.md).
- `persisted_in_save` stops a stolen book being put back into the NPC's inventory on load. See [BOOK_FORMS.md](BOOK_FORMS.md#keeping-forms-valid-across-loads).
- `GetDiaryTheftStatus` returns JSON: `{"stolen": true, "chronicled": false}` or `{"stolen": false}`. `chronicled` is always `false` (left over from an older design).
- The ESP still has `SNPD_DiaryStolenFaction` from an earlier faction-based design. Nothing uses it. (An even earlier spell-based design looked up `SNPD_DiaryStorageSpell`, which is not in the ESP; its natives were removed on 2026-09-26.)
