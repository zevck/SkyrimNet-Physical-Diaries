# NPC Diaries

Optionally, NPCs write diary entries on their own, once a game day, so diaries exist without the player asking anyone to write. SNPD only decides **who** writes and **when**; the entry is SkyrimNet's own diary generation, and it becomes a book through the usual pipeline ([VOLUMES_AND_SYNC.md](VOLUMES_AND_SYNC.md)).

Status (2026-10-01): run on AE (daily runs wrote entries that became books; declined picks cost nothing). Not yet run in game: the Daily Writers page, the Sleep/Wait and fast-travel early runs. Off by default: every entry is an LLM call the player pays for.

Code: `src/NpcDiaries.cpp`; the daily writers on the MCM's Daily Writers page, kept in the save; settings in [CONFIG_AND_MCM.md](CONFIG_AND_MCM.md).

---

## How SkyrimNet writes an entry

SNPD calls SkyrimNet's Papyrus native `SkyrimNetApi.GenerateDiaryEntry(actor)` through the VM (`DispatchStaticCall`). It is asynchronous: the generation runs on SkyrimNet's thread pool, and the new entry fires `SkyrimNet_DiaryCreated` like any other. What SkyrimNet does with it (`DiaryManager`):

- It gathers the actor's **uncovered events**: events involving them that none of their last 10 entries used. With `respect_day_boundary` on (`Diary.yaml`), only events since the start of the current game day, `floor(GameDaysPassed)` (midnight); off, the most recent ones, up to `max_recent_events`.
- Below `min_events_for_generation` it stops **before any LLM call**, so asking for an actor with nothing to write about costs nothing.
- The entry is dated when it is generated. Only the start matters: SkyrimNet collects the events when the task starts, so an LLM call that ends after midnight is fine.

**SkyrimNet's switches.** Nothing runs while SkyrimNet's global AI toggle (`game` config, `general.globalAIEnabled`; the toggle hotkey flips it) or its diaries (`Diary` config, `enabled`) are off: every request would fail with an error. SNPD reads them through SkyrimNet's Papyrus `SkyrimNetApi.GetConfigBool` (it serializes the live config, so the hotkey's change shows; the C++ API's config reader only reaches plugin configs), and only when they matter: when a run is due (`RunWhenAllowed`), and when the MCM opens. A Papyrus call answers later, so the run starts once the answers are in (a frame or two). Off: the player wants no diaries, so the day is skipped: marked as run (a time skip's days with it) and logged as `Day N: skipped, SkyrimNet's AI is off`. A spaced-out request still waiting is dropped if SkyrimNet's AI is found off when it goes out. Until 2026-10-01 the day waited and SNPD asked again every minute, so turning them back on that evening still ran it. The MCM shows SkyrimNet's diary switch and day boundary next to SNPD's settings and changes them through SkyrimNet's `PatchConfig` (applied and saved to SkyrimNet's config file; the label says they're SkyrimNet's), since the day boundary decides whether a skipped day is lost. The minimum events are SkyrimNet's to apply. The C++ public API has no generation call; SNPD doesn't add one.

---

## When

Once per game day (`g_lastRunDay`, the day number saved in the co-save, see [DATABASE.md](DATABASE.md#co-save-records)):

- **At the writing hour** (`[NpcDiaries] RunHour`, default 22:00): a clock thread queues a check every 10 seconds; the first one at or after that hour on a day not yet run runs it. Requests are spaced 3 seconds apart; one still waiting is dropped after a load (a counter `Revert` bumps), with the feature turned off, or before the post-load sync.
- **When a time skip passes the writing hour** (what matters is that the hour was passed, not where the skip started), with every request sent at once. **A skip gives one run, however many days it lasts** (a wait mod allows 30): SkyrimNet has no guard against generating for the same actor twice at once, so two runs seconds apart would write two entries about the same events and pay for both. While a skip is under way the clock's check waits (`g_skipActive`); when it ends, every day it passed the writing hour on counts as run (through today if it's past the hour now, else yesterday). If the wait's run is still reading SkyrimNet's settings then, it marks the days itself (`g_skipThrough`).
  - **Sleeping and waiting**: while the Sleep/Wait menu is open, a check every 100 ms (`kWaitPoll`; a wait passes the writing hour in well under a second) runs the day the moment the clock passes the writing hour, normally before midnight; only the first time in that wait (`g_skipRan`).
  - **Fast travel and carriages** skip time in one jump during their loading screen: logged on AE (2026-10-01), the screen opens with `PlayerCharacter::GetPlayerFlags().fastTraveling` set (not for doors) and the departure's game time; the jump lands during the load. SNPD notes the departure day when it opens; when it closes, if the trip passed a writing hour, the one run happens then (logged as `Days A-B`), arriving after the hour included. Arriving the same evening is the same run.
  - The same check runs when a wait ends, in case it jumped past midnight between two polls.
  - With SkyrimNet's day boundary on, a run after midnight sees only the new day, so SkyrimNet declines (free) and the day is lost; off, the uncovered events carry over and the entries are written.
  - Opening the map was a trigger until 2026-10-01 (players open it all the time, so it moved the run hour for nearly everyone), and so was opening Sleep/Wait or a fast travel in the 4 hours before the writing hour (a skip from earlier in the day passed 22:00 unnoticed).
- Nothing runs before the post-load sync, at the main menu, or twice a day.
- **A day can be lost**: loading a save from after midnight, a time skip some other way (a script's `SetValue` on the clock), or, with SkyrimNet's day boundary on, a fast travel that arrives after midnight.

---

## Who writes

From SkyrimNet's `PublicGetActorEngagement` (one call, no LLM; a short window of 1 game day, a medium of 7), the actors whose last event (`lastEventTime`) is within the last game day of the **game's** time. SkyrimNet's own short-window counts are measured from the newest event in its database, which after reloading an earlier save (no Keep/Clear prompt when only NPCs have later history) is ahead of the game, and would call everyone quiet. Its importance figures, used in the weight, have the same skew; after such a reload the picks are just more even.

- **SkyrimNet's activity data is keyed by actor name**: same-named NPCs share one row (their events summed) with the highest FormID of that name. So it only weights and finds the random picks; a pick that lands on the wrong same-named actor costs nothing (SkyrimNet finds no events for them). The daily writers don't come from it (below).
- **Filtered** like part of SkyrimNet's actor filter (SkyrimNet applies the whole filter itself from Beta 26; before that, the Papyrus generator didn't): not the player, not dead, not in `SkyrimNet_ActorBlacklistFaction`; in `SkyrimNet_ActorWhitelistFaction`, or of a race that allows dialogue.
- **Already written up** are skipped: when an actor's newest diary entry is dated at or after their last event (`lastEventTime`), SkyrimNet would find no uncovered events and decline, and the pick would be wasted. One `GetDiaryEntries(formId, 1)` per candidate, no LLM. (SkyrimNet's own test is by event id, over the last 10 entries; dates are the cheap stand-in.)
- **Anyone who already has an entry dated that game day is skipped**, daily writers included (`WroteOn`: one `PublicGetDiaryEntries` query for the day's range): players often write entries with SkyrimNet's diary hotkey, and an entry made before a reload counts too. This holds even with enough new events for another entry.
- **Daily writers**: this save's list ([below](#daily-writers)), uncapped: setting an NPC to write every day is the player's conscious choice, cost included. Kept by FormID (`g_writers`), not found through the name-keyed activity data, and filtered like everyone else (`MayWrite`: alive, not on SkyrimNet's blacklist, a race that can talk). Off with `[NpcDiaries] DailyWriters` = 0 (the list stays).
- **Random picks**: `[NpcDiaries] DailyRandom` (default 3) of the rest, weighted random without repeats. Weight: `1 + 4 × recent memory importance + 0.1 × recent events (up to 20)`, doubled for followers (`IsPlayerTeammate`) and the player's spouse (relationship Lover) with `[NpcDiaries] CloseBoost` on. Busy, important NPCs write most days; quiet ones sometimes. Adopted children aren't recognised yet.
- Each run logs a summary (actors from SkyrimNet, quiet, not loaded, filtered, wrote today, written up, candidates, daily writers and how many of them wrote today) and each pick (`[NpcDiaries] Day …`); Debug Logging adds every actor's numbers.

A pick SkyrimNet declines (too few events) costs nothing, so the daily cost is at most the daily writers plus `DailyRandom` calls.

---

## Daily writers

The NPCs who write every day belong to the save: an NPC who matters in one playthrough may not in another. They're kept in the `SNND` co-save record (FormIDs, resolved with `ResolveFormID` on load, so a load-order change is fine; an NPC whose plugin is gone drops out), in the order they were added, and a load or new game replaces the list.

The MCM's **Daily Writers** page (`RenderDailyWritersPage`, filled left to right):

| | |
|---|---|
| **Write Every Day** (toggle: `[NpcDiaries] DailyWriters`, on by default; off keeps the list, but only the random picks write) | **Add Targeted Actor** (shows the NPC in the crosshair when the menu opened, `Game.GetCurrentCrosshairRef`; disabled for no one, the player, or someone already listed) |
| *(a rule: two empty headers)* | |
| NPC 1 | NPC 2 |
| NPC 3 | … |

Selecting an NPC asks to remove them (`$SNPD_RemoveWriter{}` with their name). Up to 128 are listed. Adding and removing show `[Messages] DailyDiaryOn` / `DailyDiaryOff` with the NPC's name (`AddDailyWriter`, `RemoveDailyWriter`). The page list is set again in `OnConfigOpen` (`SetPages`), since `OnConfigInit` runs once per save and older saves had two pages.

History: until 2026-10-01 a silent dialogue ("Would you keep a diary?") added the NPC to a faction, with the same list in `SNND` (version 2, which still loads); for a day it was the INI's `[DailyWriters]`, which made the list the same for every playthrough.

---

## Not done yet

- Run in game: the Sleep/Wait run when the clock passes the writing hour, the fast-travel and carriage run, a day skipped with SkyrimNet's AI or diaries off, the Daily Writers page (add, remove with the prompt, the switch, the list after save/reload and on another character), the MCM's SkyrimNet options (`GetConfigBool` / `PatchConfig`), a reload not running twice.
- Adopted children for the close-NPC weight.
