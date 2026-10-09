# Player Diary Editing

The player can write in their own **journal** in the game's book menu: the pages look as they do when reading, and the entries' text is editable in place. Saving writes the changed entries back to SkyrimNet; an entry can be torn out (deleted), and new entries can be written at the end.

**The editor is Ink & Quill's** (*Ink & Quill - Writing Framework*, a separate mod and repo): its `book.swf`, the keys while writing, the close prompt, quills, ink and the blood prompt. SNPD is one of its clients: it says which books are its own, renders a journal as **marked text** (what the player may not change locked), and keeps what the player saves. Ink & Quill's `docs/API.md` is the contract, its `docs/EDITOR.md` the editor's behaviour. Until 2026-10-02 the editor was SNPD's own; it moved so Physical Letters can share it.

Status (2026-10-02, branch `player-writing`): SNPD as Ink & Quill's client runs on AE (writing and saving felt as before, 2026-10-02); the rest of the list in [Not done yet](#not-done-yet) is unchecked. The editor itself, saving, tearing out, blank journals and blood ran on AE with SNPD's own editor before the move, and Ink & Quill's marked text was checked on an SNPD journal (its layout test). Journals side by side (entry tags, the entry limit, Reset keeping them) are built but not yet run in game. SE, VR, the Convenient Reading variant and Cyrillic text are not tested. See [Not done yet](#not-done-yet).

Code: `src/BookEditor.cpp` (the client: sessions, marked text, saving, tearing out, the new-entry key; `EditorWrites.cpp`, the write queue; `EditorJournals.cpp`, a new journal from a blank, restoring lost journals and room; shared state in `include/EditorInternal.h`), `src/WritingMode.cpp` (finding Ink & Quill), `include/InkAndQuillAPI.h` (Ink & Quill's C header, copied), `FormatDiaryEntries(…, marked)` in `BookText.cpp`, `Database::AddDiaryEntry` / `UpdateDiaryEntry` / `DeleteDiaryEntry` (SkyrimNet public API v11), `BookManager::ReconcileAfterWrite` and `BookManager::CreateEmptyVolume`.

---

## Rules

- **Only the player's journal.** A volume is editable only when it is a journal (`VolumeKind::Written`, see [Diaries and journals](#diaries-and-journals)); journals are only ever the player's. An NPC's diary entries are also that NPC's memories in SkyrimNet; editing one would rewrite what they remember. The player's own diary (SkyrimNet's AI-written entries for the player) is read-only too.
- **Only changed entries are written.** An entry the player didn't touch stays exactly as SkyrimNet has it, including the lines the reading view hides (see [Text](#text)).
- **Headings and the title page are not editable.** Each entry's date heading is the anchor that ties the text under it to one SkyrimNet entry id. They're locked in the marked text.
- **Nothing the player writes may be lost silently.** Writing doesn't start unless it can be saved, a refused save keeps the text for the next edit, and a text that can't be read back is reported, not dropped.

---

## Writing mode

Writing is on when **Ink & Quill is installed, new enough, and its writing is on** (its `book.swf` is the one the game loads; Ink & Quill decides that and logs it in `InkAndQuill.log`). There is no SNPD setting.

- `WritingMode::Connect` at `kPostLoad`: `GetModuleHandleA("InkAndQuill.dll")`, `IQ_GetAPI(IQ_API_VERSION)`. Missing, or older than SNPD's copy of the header: off. The log says which (`[WritingMode] Ink & Quill found` / `Off: <why>`).
- `WritingMode::IsOn` asks Ink & Quill (`IsWritingOn`). Ink & Quill detects its `book.swf` at its own `kPostLoad`, so the answer is right by SNPD's `kDataLoaded`, whichever plugin gets the message first.
- Only with writing on does `BookEditor::Register` (at `kDataLoaded`) add SNPD as an **owner** (`AddOwner`), Ink & Quill blanks for the blank journals, its keys (`RegisterKeys`) and the input sink for the tear-out and new-entry keys. With it off, journals are read-only and both keys do nothing.
- SNPD ships no `book.swf`. A development `book.swf` left from before the move (interface 3) is too old for Ink & Quill (4), and would turn writing off if it won the file conflict.

### Diaries and journals

A volume has a **kind** (`VolumeKind`: `DiaryBookData::kind`, DiaryDB `kind`), fixed when it is made, so turning writing on or off never rebuilds a book:

| Kind | Title | Holds | Made by |
|---|---|---|---|
| `Generated`: a diary | `[Format] DiaryTitle`, "{Name}'s Diary" | SkyrimNet's entries without the `snpd_player_written` tag | The pipeline ([VOLUMES_AND_SYNC.md](VOLUMES_AND_SYNC.md)): every NPC's volumes, and the player's unless `[Diary] PlayerDiaryBooks` is off |
| `Written`: a journal | `[Format] JournalTitle`, "{Name}'s Journal" | The player's entries with the tag, each in the journal its `snpd_journal_<n>` tag names | Reading a blank journal ([below](#reading-a-blank-journal)) |

- **Each kind numbers its own volumes from 1**: the player can have Diary v1 to v3 and Journals 1 and 2. `BookManager` keeps each kind as its own chain (`ChainKey`: the UUID for the diary, so every lookup by UUID alone gets the diary; `"<UUID>|j"` for the journals). DiaryDB's key is `(actor_uuid, kind, volume_number)`, and a journal's co-save key is `"<UUID>|j<n>"`.
- **Journals sit side by side** (since 2026-09-30; before, they were a chain split by time like a diary, and a new one closed the last): the player writes in whichever one they open. An entry is in the journal its **tag** `snpd_journal_<n>` names (`JournalOf` / `SetJournal` in `Database.h`; `n` is the journal's volume number), whatever its date. An entry the player wrote without one (before 2026-09-30) is journal 1's. `BookManager::GetLiveEntries` fetches the player's written entries and keeps the journal's; a journal's entry id bounds mean nothing (`GetLiveEntries` reads a journal by its tag). SNPD tags a new entry for its journal, and any entry it saves (`OnSave`). The tag travels with the entry in SkyrimNet, so neither Keep/Clear nor a lost DiaryDB can separate the two.
- **A journal holds `[Diary] EntriesPerVolume` entries**, as a diary volume does: limited ink and unlimited pages didn't fit together, and a limit keeps blank journals worth finding. A full journal takes no new entry (`EditJournalFull`); reading, editing and tearing out (which makes room) work as ever. Lowering the setting moves nothing: a journal over it just counts as full ([New entries](#new-entries)).
- **Numbering:** a new journal takes the number after the highest of the character's journals and of every journal tag in SkyrimNet (`StartJournal`). After a Keep, SkyrimNet still has the entries of a journal made after the loaded save; a new journal must not take that number and show them (and the journal itself comes back, below).
- **A journal's book with no DiaryDB row** (a new or lost DiaryDB, or a row another branch of saves dropped) is registered again from its co-save record (`LoadFromDB`): its number, look and title are in the record, and its entries follow their tags. A diary's book in that case waits to be reused ([BOOK_FORMS.md](BOOK_FORMS.md#load)).
- **MCM Reset keeps the journals** as they are: same books, same looks, wherever they are ([DATABASE.md](DATABASE.md#mcm-reset-resetalldiariesinternal)).
- **The filter** is SkyrimNet's, by tag, in `Database::GetEntriesById` (the diary leaves out `snpd_player_written`, journals ask for it, before the limit); `GetDiaryEntries` does the same in SNPD for its date queries (fetching them all and applying the limit afterwards). Only the player has written entries, so for anyone else it changes nothing. Counts and seals only ever see one kind. A journal shows its entries in date order (`EntryOlder`).
- **The player's diary is a diary like any NPC's, read-only,** whether writing is on or off: it sits beside the journal, and the two titles tell them apart. With **`[Diary] PlayerDiaryBooks`** off (MCM "Your Diary Books"; by default off with writing on, on without) it stops growing: `UpdateDiaryForActorInternal`, the recovery probe and the catch-up scan skip the player, and `GetLiveEntries` ends the player's latest diary volume at its `lastId` instead of leaving it open, so SkyrimNet's new AI-written entries for the player are in no book (they stay in SkyrimNet as memories; SkyrimNet's own settings can stop writing them). Turned on in the MCM, the diary catches up at once through the usual update (`UpdateDiaryForActorInternal(0x14)`, as a new entry would): the latest volume fills first if the player carries it, then new volumes; with no diary yet, every volume from 1. Turned on any other way (the INI, or the default flipping because Ink & Quill came or went), the load's recovery probe or catch-up scan does the same. Turned off, nothing moves: the latest volume just stops at its `lastId` from its next render.
- **Writing off:** journals stay, read-only (SNPD isn't an owner) and still rendered. A save with journals tells the player once per game run, after the post-load sync has loaded the volumes (`WarnIfWritingOff` in `main.cpp`: a message box with `[Messages] WritingOff`, which names Ink & Quill, and the game's OK, like the missing-templates warning, plus a banner in the log). It can't be done at startup, where the missing-templates warning is: only the save says whether there are journals, and a player who never wrote shouldn't be told.
- **A journal whose book isn't in the loaded save** (made later, then SkyrimNet's Keep) is dropped like any volume ([BOOK_FORMS.md](BOOK_FORMS.md#load)), then **made again in the player's inventory** (`RestoreLostJournals`, after `ReconcileWithTimeline`): every journal number SkyrimNet has entries for (untagged ones count as journal 1) that this save has no book for gets a new book with that number, in its old look (`LoadFromDB` notes it from the DiaryDB row before deleting the row, `TakeLostJournalLooks`; else a new one), and its entries; the player gets `JournalRestored`. Keep means the writing happened, so the journal exists, as NPC diaries are rebuilt from SkyrimNet. After a Clear there's nothing to restore: the entries are gone. Each save branch keeps its own copy of the book (one in a chest in one save, in the inventory in another), both showing the same entries; the save's own form wins in DiaryDB at each load ([BOOK_FORMS.md](BOOK_FORMS.md#load)). A save from before the blank was written in still has the blank, so the player keeps both ([KNOWN_ISSUES.md](KNOWN_ISSUES.md) #10).
- DiaryDBs from before 2026-09-29 are migrated once, every volume a diary ([DATABASE.md](DATABASE.md#schema-changes)). Entries already written through the editor then leave the diary; untagged, they show in journal 1.
- **Journals on SkyrimNet's Keep/Clear** (`ReconcileWithTimeline`): the journals in the save stay, whatever their entries (a Clear can empty one); each is re-rendered if its entry count changed.

### Blank journals

"Blank Journal" books in the ESP, one per journal look (`SNPD_BlankJournal1`–`3`, and `SNPD_BlankJournalN` for the Nightingales; [PLUGIN.md](PLUGIN.md#records)), 20 gold. `BlankJournals::OnDataLoaded` (at `kDataLoaded`):

- **Writing on:** names them from the locale file (`[Format] BlankJournal`; the ESP's name is English), and adds `SNPD_LItemBlankJournal` (one of the three at random) to Skyrim.esm's `LItemMiscVendorMiscItems75` **in memory**: the general-goods list that already sells the Roll of Paper, rolled by 18 merchant chests (Belethor, the Riverwood Trader, Bits and Pieces, the Khajiit caravans, …) and by two loot containers (`Cupboard01`, `PersonalChestSmall`). No vanilla record is overridden, so no other mod's edit of that list can drop them, and no patch is needed. A chest restocks every 48 game hours, so an existing save's merchants carry them from their next restock.
- **Crafting:** at a tanning rack, 1 Leather + 2 Roll of Paper, one recipe per look. A fourth, in the Nightingale look, unlocks once the player has taken the Nightingale Oath (TG08A "Trinity Restored" stage 57, or the quest done); merchants never sell it.
- **Writing off:** the recipes lose their workbench (so they're hidden), and merchants never get the list.

Blank journals are Ink & Quill **blanks**: `BookEditor::Register` registers each (`RegisterBlank`, `BlankJournals::Forms`), after `BlankJournals::OnDataLoaded` has found them.

### Reading a blank journal

Reading a blank journal **from the player's own inventory** starts writing the first entry of a journal **that doesn't exist yet**; the first save makes it, in place. Until then nothing is made: putting the quill down, discarding or closing leaves the blank as it was and costs nothing (until 2026-10-02 the journal was made as soon as the blank was read, so a blank read by accident left an empty journal).

1. **Ink & Quill decides when** (its `docs/EDITOR.md`, "Blanks"): only from the player's own inventory (in the world, a container, a shop or the gift menu, or not carried, it's just an empty book), with a quill (else a notice, and nothing more). Its edit key on a blank in the inventory does the same. Once the menu has the text it calls SNPD's `OnBlankOpen`.
2. **`OnBlankOpen`** checks SkyrimNet can save (`EditNeedsSkyrimNet`), then begins a session (`g_blankLook` = the blank's look, `BlankJournals::LookOf`) over one new entry, untagged for any journal, dated now, red-headed if the session would be in blood ([Writing in blood](#writing-in-blood)): the title page with the player's name, and that entry, caret in it. The new-entry key does nothing (there is no journal yet).
3. **The first save** (`OnSave`, an entry with text): `StartJournal` makes a new, empty journal beside the others ([Numbering](#diaries-and-journals)), in the blank's look (passed to `CreateEmptyVolume`, so the journal keeps that look for good); the session becomes that journal's (`g_bookFormId`, `g_journal`), and the entry is tagged for it and added as any new entry ([Saving](#saving), [New entries](#new-entries)). The answer is `ReplySaveAsBook` with the journal and its reading text, and the player gets `EditStartedVolume`. Ink & Quill then charges the ink or blood, removes one blank, and points the open menu at the journal (the engine's book-menu globals: the code that was SNPD's `SetBookMenuBook`); the edit key goes back to reading the journal, the close prompt's Save closes it. Either way that ends the session.
4. **If the journal can't be made**, the save is refused with `BlankJournalFailed`: writing goes on, the blank stays, nothing is charged.

### Ink, quill and blood

Ink & Quill's: a quill to start, one use of ink **per save** (or, with a quill and no ink, the blood prompt and 10% of maximum health per save), charged only once SNPD accepts the save; inkwells and their names. See Ink & Quill's `docs/EDITOR.md`. Until 2026-10-02 SNPD had its own: ink per session, blood per entry, and partly used inkwells as the ESP's `SNPD_Inkwell1`–`9` (with a Description Framework file for how full they were). Those records are gone; a development save that had one loses it.

### Writing in blood

What SNPD keeps of blood:

- **What's typed in blood is dark red** (`#2B0202`; vanilla ink is `#000000`, and the lit book page brightens colours, so `#6B0F0F` and `#420606` read as bright red in game), and stays red: reopened later with ink, the red stays; text typed in ink inside a red passage is ink.
- **The red is kept apart from the text**: SkyrimNet stores the plain text (markup there would reach memories and prompts). The red is a list of `[start, end)` byte ranges of the entry's content (`"s:e,s:e"`), in DiaryDB's `blood` table with a hash of the content they belong to ([DATABASE.md](DATABASE.md#schema)). If the content no longer matches (edited outside SNPD), the red is dropped rather than painted on the wrong words, and the log says so once per entry with both hashes (a SkyrimNet that changed the text as it stored it would look the same to the player).
- **In the marked text and the runs it is marked inline** instead, between two private-use characters, U+E000 and U+E001 (`kBloodOpen`, `kBloodClose`; `MarkBlood` / `SplitBlood` in `BookText`), so no offsets have to be kept in step between C++'s UTF-8 and the SWF's UTF-16. A save splits each run (`SplitBlood`): the plain text goes to SkyrimNet, the ranges to DiaryDB once SkyrimNet has the entry (`CompleteWrite`, when the id is known; a tear-out deletes the row). Reading (`SanitizeBookText`) marks the content, sanitizes and escapes it, then turns the markers into `<font color='#2B0202'>…</font>`. `MarkBlood` first removes any marker already in the text (an NPC's entry is LLM output), so only DiaryDB's ranges become markup.
- **An entry begun in blood has a red heading too** (`DiaryEntry::bloodHeading`). It's the entry's, not its text's: it stays red if the blood text is later retyped in ink, and if the text was changed outside SNPD. A new entry added while writing takes Ink & Quill's `InBlood`; one a session begins with (the new-entry key while reading, an empty journal, a blank journal) is red if Ink & Quill says the session would be in blood (`WouldBeInBlood`: its blood setting on, a quill but no ink); declining the prompt ends the session.
- **SkyrimNet knows**: an entry with any blood (text or heading) is tagged `snpd_written_in_blood` (next to `snpd_player_written`), and loses the tag when no red is left.

---

## Marked text

What SNPD hands Ink & Quill: the journal as the book shows it, from `FormatDiaryEntries(entries, name, Written, true)`, with everything the player can't change between U+E002 and U+E003 (`kLockOpen`, `kLockClose`). The rules are Ink & Quill's (its `docs/API_DESIGN.md`, "Marked text", agreed with SNPD's side 2026-10-02):

- **Locked:** the blank first page, the title page and dates, every `[pagebreak]`, each heading with its font resets and `\n\n`, and the breaks after each entry.
- **Each entry's text is one run**, even an empty one (a lock close straight followed by a lock open), and headings off changes nothing: **run k is entry k** of the session (`g_edit`). The run is the entry's paragraphs and the `\n\n` between them; the lock opens right after the last paragraph's `</font>`, so typing at an entry's end stays in its last paragraph.
- **Blood stays as its markers** (the editor paints it); a locked heading keeps its colour tag.
- **No empty-journal sentinel** (an HTML comment, untested in an input field): an empty journal is the title page and a locked page break, with no runs.
- **Never through `ForBookMenu`**: its Win-1251 step handles only one- and two-byte UTF-8, and the markers are three bytes. The text read again after a save is a separate, reading render.
- **Stored text can't break the runs**: `MarkBlood` removes U+E002 and U+E003 from an entry's content as it does the blood markers.
- **Hint:** the session's `runFont` and `runSize` are the content font and size, so typed text and paragraph breaks look as the renderer makes them.

Ink & Quill compares each run against what it loaded to know whether anything changed; SNPD compares each against its own `savedBody` to know what to write. `savedBody` is the run as the editor reads it back (`RunText`): the entry's text (`EditableEntryText`) as paragraphs (`Paragraphs`: empty ones are blank lines and stay, only trailing ones go, as the renderer does: [BOOK_TEXT.md](BOOK_TEXT.md#rendering-formatdiaryentries)), with `\n` line breaks.

---

## Starting

Two ways, each `Begin`, and a third for a blank journal ([Reading a blank journal](#reading-a-blank-journal)). Ink & Quill holds one session at a time, so `BeginSession` keeps SNPD's previous one aside (`TakeState`): if Ink & Quill refuses the new one (writing, or a prompt open) the previous one's state goes back (`PutState`).

1. **The edit key** (Ink & Quill's, `InkAndQuill.ini` `[Keys] Edit`, default F3) on an open book: Ink & Quill asks each owner; SNPD's (`Owner`) takes its journals (an NPC's diary or the player's diary: not editable, logged, passed on).
2. **The new-entry key while reading** a journal (`NewEntryInBook`): a session with a new entry, caret in it.

`Begin` checks **SkyrimNet can save** (`Database::CanWriteDiaries`: the API initialized, public API v11 or later, with its add, update and delete functions), or the player gets `EditNeedsSkyrimNet`; and for a new entry, **room** (once the entries are read: as many as `EntriesPerVolume` is full). Then:

- **The entries:** `BookManager::GetShownEntries` (the same entries `RefreshVolumeOnOpen` renders), or SNPD's own copy if writes to this journal are still in SkyrimNet (see [Saving](#saving)). Each goes into `g_edit` with its `savedBody` and whether its last save was refused (`unsaved`).
- **A new entry** if asked, or if the journal has none (there'd be nowhere to type); the session's `caretRun` is then its run.
- **The session** (`IQ_Session`): the marked text, the font hint, the callbacks (`OnSave`, `OnDiscard`, `OnEnd`), and `user` = the session's number (`g_session`), so a callback from an older one is ignored.
- **Paused or not:** Ink & Quill writes in book menus that don't pause the game too (Skyrim Souls RE). Its callbacks are UI tasks: on the main thread while paused, on the engine's "UI" job during play, which never runs alongside SNPD's other game-thread work ([ARCHITECTURE.md](ARCHITECTURE.md#threading)), so saving and tearing out (which make and change book forms and the inventory) need no pause. Until 2026-10-03 SNPD refused to write unless the menu paused the game, from an unchecked guess that UI tasks weren't safe for game state.
- Ink & Quill checks the quill and the ink (or asks about blood), and may end it there; **`OnEnd`** clears `g_edit`, `g_bookFormId` and `g_session` whichever way it ends.

---

## Saving

Ink & Quill calls `OnSave` with every run, on its edit key while writing (save, then read again) or on its close prompt's Save (save, then close), when any run changed. Each run is compared with its entry's `savedBody`; an entry is written if its text changed, or if its last save was refused (`unsaved`). For each:

- The entry in `g_edit` takes the new text, the `snpd_player_written` tag (`kPlayerWrittenTag`) and the tag of the journal being edited (`SetJournal`, `g_journal`), keeping its other tags, and the volume is re-rendered from the edited entries at once (`SetVolumeText`): the book, its text snapshot and DiaryDB's cache show the edit before SkyrimNet has it. `RefreshVolumeOnOpen` skips the volume until its writes are done (`HasPendingWrites`), or it would put SkyrimNet's old text back.
- An update job goes to the **write queue**: one worker thread that runs the writes in order (`PublicUpdateDiaryEntry` blocks while SkyrimNet re-embeds the entry's memory), so two quick saves of the same entry can't land out of order.
- Each finished job queues a game-thread task. When a volume's last pending write is done, `BookManager::ReconcileAfterWrite` re-renders it from SkyrimNet.
- **Pending writes, per volume** (`g_pending`): until they are done SkyrimNet still has the old text, so writing in that journal again (the edit key right after saving) starts from SNPD's saved entries, not from SkyrimNet.
- **A refused or failed write** (for example while SkyrimNet's keep/clear timeline check is pending) shows `EditSaveFailed`. The entry's text stays in `g_pending`, marked unsaved, so the next session in that journal starts from it and saving writes it again. (The book shows SkyrimNet's text meanwhile.) The write queue runs after the save was accepted, so the ink or blood is spent either way.
- **A load or new game** (`BookEditor::Reset`, from `EndSession`) bumps a generation and drops the edit state: a write that finishes afterwards is ignored instead of touching a volume in the new session.

**The answer** (`ReplySave`): accepted, with the journal rendered for reading from the edited entries (`FormatDiaryEntries`, then `BookTextHook::ForBookMenu`, the same text the book menu would get); Ink & Quill charges the ink or blood and shows it. Refused (nothing charged, writing goes on): a run count that doesn't match `g_edit` (`EditSaveFailed`), or **only an emptied entry**: an emptied entry is left as it was, and if nothing else changed the hint (`EditEmptiedHint`) is the refusal's message; with other changes it's a notification. Removing an entry is tearing it out (below), which asks first.

---

## Tearing out an entry

The **tear-out key** (`[Diary] DeleteKey`, default F10, SNPD's own, set in the MCM; registered with Ink & Quill so it reaches SNPD while writing, `RegisterKeys`) in one of the player's journals while Ink & Quill writes in it (`AskTearOut`):

- **The entry** is the run the caret is in (`CaretRun`); none (-1), or a blank journal's session (nothing to tear out yet): the key does nothing.
- **The question** is Ink & Quill's `Prompt`, which holds every key while it's open (nothing reaches the journal, Enter included): "Tear out the entry from {Date}? It will be gone from your journal and from your memory." (`EditDeletePrompt`, with `EntryDate`, so the date shows even with headings off), **Tear out** or **Keep it** (`EditDelete`, `EditKeep`; Keep it is the cancel button, Escape's). SNPD's own keys wait while it's open (`g_asking`): Ink & Quill lets every key through to the game during a prompt.
- **The answer** comes in `TearOutAnswered`, after the box has closed and the editor has its keys back. If the session ends first (a load, the book closing), Ink & Quill never calls it; `OnEnd` clears `g_asking`.

Until 2026-10-02 the key was SNPD's; for a day it was Ink & Quill's (its `removePrompt` / `onRemove` callbacks), then Ink & Quill gave tear-out back and added `CaretRun` and `Prompt` for it.

**Tear out** (`TearOut`) acts at once:

- The runs as the player has them (`CurrentRuns`, unsaved text included) are rendered again without the entry, and Ink & Quill reloads (`Reload`, with the reading text without it, `ReadingText(run)`, which the edit key goes back to since no run changed; and `from` skipping it, so the other entries' unsaved changes still prompt on close); the caret ends the entry before, or starts the one after. `g_edit` drops it.
- The volume is re-rendered from the remaining entries, and a delete job goes to the write queue (`PublicDeleteDiaryEntry`: the entry and its memory), counted in the volume's pending writes like a save. A new entry never saved just leaves the editor.
- On failure the player gets `EditDeleteFailed`; the volume's pending entries are dropped when its writes finish (they lack an entry SkyrimNet still has), and the reconcile puts the entry back in the book.
- Tearing out every entry leaves the title page and a blank page (a journal never shows the "all entries removed" notice diaries get; see [BOOK_TEXT.md](BOOK_TEXT.md#rendering-formatdiaryentries)). The book stays, ready for new entries.

Tearing out an entry makes room in a full journal, and never moves another entry: each is in the journal its tag names. It costs no ink.

The new-entry key is registered the same way (`RegisterKeys`), and waits while the question is open.

---

## New entries

The **new-entry key** (`[Diary] NewEntryKey`, SNPD's, set in the MCM; default F8; disabled in the MCM with writing off). It only acts in an open book: SNPD's input sink is prepended, so the book menu never gets it. While the player writes, Ink & Quill keeps every key from the game except those registered with it, so SNPD registers this one (`RegisterKeys`: at `Register`, and when the MCM changes it). The MCM takes only keys Ink & Quill's `CheckKey` accepts ([CONFIG_AND_MCM.md](CONFIG_AND_MCM.md#the-ini)).

- **In any journal with room**: journals sit side by side ([Diaries and journals](#diaries-and-journals)). A full one (`EntriesPerVolume` entries) gets `EditJournalFull`: while reading, before writing starts (`Begin`, by the entries it starts from: SkyrimNet's, or the pending writes'); while writing, by the session's own count (`AppendNewEntry`).
- **While reading**: a session with a new entry ([Starting](#starting)). **While writing** (`AppendNewEntry`, when Ink & Quill `IsWriting` and the session is SNPD's): `CurrentRuns`, the new entry added at the end, rendered and reloaded (`from` = each run itself, then -1), caret in it. **One unsaved new entry at a time**: if one is already there and not saved, the key reloads with the caret back in it.
- **The new entry** is a new page at the end: its heading (today's date, if headings are on) and an empty run. In SNPD it is a `DiaryEntry` with no id and a **local key** (`localKey`, from `g_nextLocalKey`), dated now and tagged `snpd_player_written` and for its journal.
- **Saving it** queues an **add** (`PublicAddDiaryEntry`, with that date and the tags, emotion `neutral` and importance 0.5: SkyrimNet's own entries are scored by the LLM that writes them, the player's aren't scored, and an edit keeps both. A stand-in: with no emotion SkyrimNet's `diary_entry_created` event reads "(feeling )", so NPCs' event history says "(feeling neutral)" for every entry the player writes, whatever it says. The fix belongs in SkyrimNet: leave the clause out when the emotion is empty) instead of an update. The worker records the id SkyrimNet returns against the local key (`ids_`); the completion task writes it into the session's and the pending entries. Until then, later saves and tear-outs of the entry are queued by local key, and the worker finds the id because the add ran first. An add that fails leaves the entry unsaved, and the next save adds it again (`g_addQueued`).
- **Left empty**, a new entry is never written and doesn't count as a change (no save prompt); torn out before it was ever saved, it just leaves the editor.
- SkyrimNet announces the entry (`SkyrimNet_DiaryCreated`) as it does its own. The pipeline handles only the player's diary (not at all with `PlayerDiaryBooks` off), and the kind filter keeps journal entries out of it ([Diaries and journals](#diaries-and-journals)); the editor's `ReconcileAfterWrite` renders the journal.
- **Order:** the session keeps its entries in the order it began with, a new one last, until it ends; reading sorts by date again.

Journals were split by time at first, like diaries, with a new one starting after the last one's final entry. That made every new journal close the last for good, and an entry still being saved when a blank journal was read could end up between two journals, in neither. Tags removed both (2026-09-30); journals had no entry limit for a day in between.

### Outside the book

Until 2026-10-04 the new-entry key also worked during play: it opened the journal the player last wrote in (or the newest one they carried with room) and began a new entry once the menu was open (Ink & Quill's `BeginSessionOnOpen`). Removed (user, 2026-10-04): the key acts only in an open journal.

---

## Text

The marked text shows each entry as the reading view does: `SanitizeBookText` (the sanitizing of [BOOK_TEXT.md](BOOK_TEXT.md#cleaning-llm-output-sanitizebooktext) and the markup escaping; the editor reads the runs back as plain text), the heading (the entry's date, none when headings are off) and `TitlePageDates`. When the player edits an entry, what they saw becomes the stored text: lines the reading view hid (an LLM's own date heading) are dropped from that entry only.

**Player-written entries skip the date stripping** (`IsPlayerWritten`), so a player's entry that starts with a date keeps it. The typographic cleanup still applies (the handwriting fonts lack those characters), and so does the stripping of `*` and `_` markdown. A reload mid-session (a new entry, a tear-out) renders the runs the same way, so that cleanup can show on screen then.

---

## Not done yet

- **Restoring a lost journal:** verified on AE 2026-10-03 (a new journal, an earlier save, Keep: the journal is back). Still unchecked: the later save keeping its own copy, and a journal stashed in a chest in the later save.
- **Run in game as Ink & Quill's client**: a blank journal read, then put down (nothing made) or saved (the journal made, the blank gone, one ink); edit, save and read again; a new entry while reading and while writing; tearing out with unsaved changes elsewhere (the close prompt still asks); blood, with a new entry's red heading; an empty journal and its first entry; headings off; discard; a load with the book open. Ink & Quill's log prints `[Editor] Marked text: N runs; layout ok (…)` at each start: N must be the journal's entry count (plus a new one).
- **Packaging:** the README and FOMOD wording for Ink & Quill as the requirement for writing.
- **SE, VR and the Convenient Reading variant** are untested. VR also needs a keyboard story, and the book-menu extra-list address Ink & Quill uses for blanks on VR (`0x30111F8`) was found in the binary, not in VR's address library, and hasn't run.
- **Cyrillic.** Reading needs Win-1251 because Scaleform's pagination mixes byte and character offsets; the marked text is UTF-8. Untested with Cyrillic text ([KNOWN_ISSUES.md](KNOWN_ISSUES.md)).
- **Translations**: SNPD's writing strings, in the locale files and the MCM, are in all nine languages, as first drafts written without native speakers (like `JournalTitle`); they stay until native feedback.
