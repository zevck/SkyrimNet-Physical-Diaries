# NPC Diaries

Optionally, NPCs write diary entries on their own, once a game day, so diaries exist without the player asking anyone to write. SNPD only decides **who** writes and **when**; the entry is SkyrimNet's own diary generation, and it becomes a book through the usual pipeline ([VOLUMES_AND_SYNC.md](VOLUMES_AND_SYNC.md)).

Status (2026-10-01): run on AE (daily runs wrote entries that became books; declined picks cost nothing). Not yet run in game: the whitelist dialogue, the Sleep/Wait and fast-travel early runs. Off by default: every entry is an LLM call the player pays for.

Code: `src/NpcDiaries.cpp`; the whitelist dialogue in the ESP ([PLUGIN.md](PLUGIN.md#records)) with the fragments `SNPD_TIF_DailyDiaryStart` / `Stop`; settings in [CONFIG_AND_MCM.md](CONFIG_AND_MCM.md).

---

## How SkyrimNet writes an entry

SNPD calls SkyrimNet's Papyrus native `SkyrimNetApi.GenerateDiaryEntry(actor)` through the VM (`DispatchStaticCall`). It is asynchronous: the generation runs on SkyrimNet's thread pool, and the new entry fires `SkyrimNet_DiaryCreated` like any other. What SkyrimNet does with it (`DiaryManager`):

- It gathers the actor's **uncovered events**: events involving them that none of their last 10 entries used. With `respect_day_boundary` on (`Diary.yaml`), only events since the start of the current game day, `floor(GameDaysPassed)` (midnight); off, the most recent ones, up to `max_recent_events`.
- Below `min_events_for_generation` it stops **before any LLM call**, so asking for an actor with nothing to write about costs nothing.
- The entry is dated when it is generated. Only the start matters: SkyrimNet collects the events when the task starts, so an LLM call that ends after midnight is fine.

**SkyrimNet's switches.** Nothing runs while SkyrimNet's global AI toggle (`game` config, `general.globalAIEnabled`; the toggle hotkey flips it) or its diaries (`Diary` config, `enabled`) are off: every request would fail with an error. The day isn't marked as run, so it still runs that evening once they're back on, and a request still waiting is dropped. SNPD reads both live through SkyrimNet's Papyrus `SkyrimNetApi.GetConfigBool` (it serializes the live config, so the hotkey's change shows; the C++ API's config reader only reaches plugin configs). A Papyrus call answers later, so the two are cached: read after every load, on MCM changes and on every clock tick, at most 10 s old when a fast travel checks them. The day boundary and the minimum events are SkyrimNet's to apply. The C++ public API has no generation call; SNPD doesn't add one.

---

## When

Once per game day (`g_lastRunDay`, the day number saved in the co-save, see [DATABASE.md](DATABASE.md#co-save-records)):

- **At the writing hour** (`[NpcDiaries] RunHour`, default 22:00): a clock thread queues a check every 10 seconds; the first one at or after that hour on a day not yet run runs it. Requests are spaced 3 seconds apart; one still waiting is dropped after a load (a counter `Revert` bumps), with the feature turned off, or before the post-load sync.
- **Earlier, before time skips** (`kEarlyHours`, 4 hours before the writing hour), with every request sent at once (a skip across midnight would leave later ones an empty day):
  - opening the Sleep/Wait menu (before the player picks the hours);
  - fast travel and carriages: the loading screen opening with `PlayerCharacter::GetPlayerFlags().fastTraveling` set. Logged on AE (2026-10-01): the game time is still the departure time when that screen opens (the skip lands during the load), the flag is set for map fast travel and a carriage and not for doors; the queued `fastTravelMarker` isn't usable, it stays set afterwards. The run happens inside the menu event, not as a task, since a later frame can be after the skip.
  - Opening the map was a trigger until 2026-10-01; players open it all the time, so it moved the run hour for nearly everyone.
- Nothing runs before the post-load sync, at the main menu, or twice a day.
- **A day can be lost**, on purpose: a load from after midnight, or a time skip that is neither of the above. With SkyrimNet's day boundary off nothing is lost, since uncovered events carry over.

---

## Who writes

From SkyrimNet's `PublicGetActorEngagement` (one call, no LLM; a short window of 1 game day, a medium of 7), the actors whose last event (`lastEventTime`) is within the last game day of the **game's** time. SkyrimNet's own short-window counts are measured from the newest event in its database, which after reloading an earlier save (no Keep/Clear prompt when only NPCs have later history) is ahead of the game, and would call everyone quiet. Its importance figures, used in the weight, have the same skew; after such a reload the picks are just more even.

- **SkyrimNet's activity data is keyed by actor name**: same-named NPCs share one row (their events summed) with the highest FormID of that name. So it only weights and finds the random picks; a pick that lands on the wrong same-named actor costs nothing (SkyrimNet finds no events for them). The daily writers don't come from it (below).
- **Filtered** like part of SkyrimNet's actor filter (SkyrimNet applies the whole filter itself from Beta 26; before that, the Papyrus generator didn't): not the player, not dead, not in `SkyrimNet_ActorBlacklistFaction`; in `SkyrimNet_ActorWhitelistFaction`, or of a race that allows dialogue.
- **Already written up** are skipped: when an actor's newest diary entry is dated at or after their last event (`lastEventTime`), SkyrimNet would find no uncovered events and decline, and the pick would be wasted. One `GetDiaryEntries(formId, 1)` per candidate, no LLM. (SkyrimNet's own test is by event id, over the last 10 entries; dates are the cheap stand-in.)
- **Daily writers**: every NPC in `SNPD_DailyDiaryFaction` ([the whitelist](#the-whitelist)), uncapped: asking an NPC to write every day is the player's conscious choice, cost included. SNPD keeps them by FormID itself (`g_writers`, added and removed by the dialogue, saved in `SNND`), not from the name-keyed activity data. Faction members added another way (the console, another mod) join when an activity row resolves to them; ones removed from the faction another way are dropped.
- **Random picks**: `[NpcDiaries] DailyRandom` (default 3) of the rest, weighted random without repeats. Weight: `1 + 4 × recent memory importance + 0.1 × recent events (up to 20)`, doubled for followers (`IsPlayerTeammate`) and the player's spouse (relationship Lover) with `[NpcDiaries] CloseBoost` on. Busy, important NPCs write most days; quiet ones sometimes. Adopted children aren't recognised yet.
- Each run logs a summary (actors from SkyrimNet, quiet, not loaded, filtered, written up, candidates) and each pick (`[NpcDiaries] Day …`); Debug Logging adds every actor's numbers.

A pick SkyrimNet declines (too few events) costs nothing, so the daily cost is at most the whitelisted NPCs plus `DailyRandom` calls.

---

## The whitelist

The player asks an NPC in dialogue: **"Would you keep a diary? Write in it every day."** (`SNPD_DailyDiaryStartTopic`), and later **"You don't need to write in your diary every day anymore."** (`SNPD_DailyDiaryStopTopic`).

- The topics are top-level player dialogue in `SNPD_DialogueQuest` (start-game enabled, listed in `Seq/SkyrimNet Physical Diaries.seq`, the same setup as Physical Letters' mailing dialogue). The ask needs the feature on (the global `SNPD_NpcDiaries`, set from the INI at load and on MCM changes; a save keeps a global's value, so it's set again after every load) and the NPC outside the faction; the stop only that they're in it, so it always works.
- **The NPC says nothing**: one response with no text and no voice. Followers, spouses and children can have any voice type, and no vanilla line is recorded for all of them. (Not yet confirmed in game that an empty response ends cleanly; the fallback is a short unvoiced text line.)
- The fragments add or remove the faction and call `SkyrimNetDiaries_Native.DailyDiaryChanged`, which shows `[Messages] DailyDiaryOn` / `DailyDiaryOff` with the NPC's name. The topic text comes from the locale file (`[Format] DailyDiaryAsk` / `DailyDiaryStop`, set on the topics at load; the ESP's is English).
- Membership lives on the actor, in the save: no bookkeeping in DiaryDB or the co-save, and Keep/Clear can't confuse it. Other mods or the console can use the faction too.
- SkyrimNet's dialogue actions should pick the topics up like any vanilla topic.

---

## Not done yet

- Run in game: a day's run (log), the early run on sleep/wait/map, the dialogue (silent response, faction, notice), the MCM, a reload not running twice, the global after a load.
- Adopted children for the close-NPC weight.
