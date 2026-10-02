# Player Diary Editing

The player can write in their own **journal** in the game's book menu: the pages look as they do when reading, and the entries' text is editable in place. Saving writes the changed entries back to SkyrimNet; an entry can be torn out (deleted), and new entries can be written at the end.

Status (2026-09-30, branch `player-writing`): editing, saving, going back to reading, tearing out entries, blank journals, quill and ink, and writing in blood work on AE. Journals side by side (entry tags, the entry limit, Reset keeping them) are built but not yet run in game. SE, VR, the Convenient Reading variant and Cyrillic text are not tested, and there is no FOMOD yet. See [Not done yet](#not-done-yet).

Code: `swf/book/scripts/__Packages/BookMenu.as` (the edit mode in the book menu's SWF), `src/WritingMode.cpp` (is it installed), `src/BookEditor.cpp` (the plugin side: the edit session; `EditorWrites.cpp`, the write queue; `EditorJournals.cpp`, which journal and blank journals; shared state in `include/EditorInternal.h`), `Database::AddDiaryEntry` / `UpdateDiaryEntry` / `DeleteDiaryEntry` (SkyrimNet public API v11), `BookManager::ReconcileAfterWrite` and `BookManager::CreateEmptyVolume`.

---

## Rules

- **Only the player's journal.** A volume is editable only when it is a journal (`VolumeKind::Written`, see [Diaries and journals](#diaries-and-journals)); journals are only ever the player's. An NPC's diary entries are also that NPC's memories in SkyrimNet; editing one would rewrite what they remember. The player's own diary (SkyrimNet's AI-written entries for the player) is read-only too.
- **Only changed entries are written.** An entry the player didn't touch stays exactly as SkyrimNet has it, including the lines the reading view hides (see [Text](#text)).
- **Headings and the title page are not editable.** Each entry's date heading is the anchor that ties the text under it to one SkyrimNet entry id.
- **Nothing the player writes may be lost silently.** Writing doesn't start unless it can be saved, a refused save keeps the text for the next edit, and a text that can't be read back is reported, not dropped.

---

## Writing mode

Writing is optional: it is on when **SNPD's `book.swf` is the one the game loads** (the FOMOD's writing option installs it; there is no setting). `WritingMode::Detect` runs at `kDataLoaded`, before anything else of the editor's:

- It reads `Data\Interface\book.swf`, the file the game sees (under MO2, the winning loose file; a loose file beats every BSA). No file means the game's own or a BSA's: off.
- SNPD's SWF ships **uncompressed** (`FWS`; the build runs `ffdec-cli -decompress` and fails if the output isn't uncompressed with the marker, see [DEVELOPMENT.md](DEVELOPMENT.md#swf)), so the marker `BookMenu.as` carries, `WRITING_INTERFACE = "BOOKMENU_WRITING_INTERFACE=<n>"`, is plain text in the file. A compressed file (`CWS`/`ZWS`) or one without the marker is another mod's: off.
- The marker is **mod-neutral**: Physical Letters will ship the same SWF, and whichever copy wins the file conflict serves both mods. So `<n>` only ever goes up: bump it when a call is added, and a plugin needs `<n>` of at least its `kMinInterface` (`WritingMode.cpp`), raised only when the plugin starts using a newer call. An older SWF is logged as an error and writing is off. The marker's name can't change once a SWF has shipped.

Only with writing on does `BookEditor::Register` install the input sink, the menu sink and the two book menu hooks; with it off, the edit and new-entry keys do nothing. The result is in the log (`[WritingMode] On` / `Off: <why>`).

### Diaries and journals

A volume has a **kind** (`VolumeKind`: `DiaryBookData::kind`, DiaryDB `kind`), fixed when it is made, so turning writing on or off never rebuilds a book:

| Kind | Title | Holds | Made by |
|---|---|---|---|
| `Generated`: a diary | `[Format] DiaryTitle`, "{Name}'s Diary" | SkyrimNet's entries without the `snpd_player_written` tag | The pipeline ([VOLUMES_AND_SYNC.md](VOLUMES_AND_SYNC.md)): every NPC's volumes, and the player's unless `[Diary] PlayerDiaryBooks` is off |
| `Written`: a journal | `[Format] JournalTitle`, "{Name}'s Journal" | The player's entries with the tag, each in the journal its `snpd_journal_<n>` tag names | Reading a blank journal ([below](#reading-a-blank-journal)) |

- **Each kind numbers its own volumes from 1**: the player can have Diary v1 to v3 and Journals 1 and 2. `BookManager` keeps each kind as its own chain (`ChainKey`: the UUID for the diary, so every lookup by UUID alone gets the diary; `"<UUID>|j"` for the journals). DiaryDB's key is `(actor_uuid, kind, volume_number)`, and a journal's co-save key is `"<UUID>|j<n>"`.
- **Journals sit side by side** (since 2026-09-30; before, they were a chain split by time like a diary, and a new one closed the last): the player writes in whichever one they open. An entry is in the journal its **tag** `snpd_journal_<n>` names (`JournalOf` / `SetJournal` in `Database.h`; `n` is the journal's volume number), whatever its date. An entry the player wrote without one (before 2026-09-30) is journal 1's. `BookManager::GetLiveEntries` fetches the player's written entries and keeps the journal's; a journal's entry id bounds mean nothing (`GetLiveEntries` reads a journal by its tag). The editor tags a new entry for its journal, and any entry it saves (`Save`). The tag travels with the entry in SkyrimNet, so neither Keep/Clear nor a lost DiaryDB can separate the two.
- **A journal holds `[Diary] EntriesPerVolume` entries**, as a diary volume does: limited ink and unlimited pages didn't fit together, and a limit keeps blank journals worth finding. A full journal takes no new entry (`EditJournalFull`); reading, editing and tearing out (which makes room) work as ever. Lowering the setting moves nothing: a journal over it just counts as full ([New entries](#new-entries)).
- **Numbering:** a new journal takes the number after the highest of the character's journals and of every journal tag in SkyrimNet (`StartJournal`). After a Keep, SkyrimNet still has the entries of a journal made after the loaded save; a new journal must not take that number and show them. Those entries stay in SkyrimNet as memories, in no book.
- **A journal's book with no DiaryDB row** (a new or lost DiaryDB, or a row another branch of saves dropped) is registered again from its co-save record (`LoadFromDB`): its number, look and title are in the record, and its entries follow their tags. A diary's book in that case waits to be reused ([BOOK_FORMS.md](BOOK_FORMS.md#load)).
- **MCM Reset keeps the journals** as they are: same books, same looks, wherever they are ([DATABASE.md](DATABASE.md#mcm-reset-resetalldiariesinternal)).
- **The filter** is SkyrimNet's, by tag, in `Database::GetEntriesById` (the diary leaves out `snpd_player_written`, journals ask for it, before the limit); `GetDiaryEntries` does the same in SNPD for its date queries (fetching them all and applying the limit afterwards). Only the player has written entries, so for anyone else it changes nothing. Counts and seals only ever see one kind. A journal shows its entries in date order (`EntryOlder`).
- **The player's diary is a diary like any NPC's, read-only,** whether writing is on or off: it sits beside the journal, and the two titles tell them apart. With **`[Diary] PlayerDiaryBooks`** off (MCM "Your Diary Books"; by default off with writing installed, on without) it stops growing: `UpdateDiaryForActorInternal`, the recovery probe and the catch-up scan skip the player, and `GetLiveEntries` ends the player's latest diary volume at its `lastId` instead of leaving it open, so SkyrimNet's new AI-written entries for the player are in no book (they stay in SkyrimNet as memories; SkyrimNet's own settings can stop writing them). Turned on in the MCM, the diary catches up at once through the usual update (`UpdateDiaryForActorInternal(0x14)`, as a new entry would): the latest volume fills first if the player carries it, then new volumes; with no diary yet, every volume from 1. Turned on any other way (the INI, or the default flipping because `book.swf` came or went), the load's recovery probe or catch-up scan does the same. Turned off, nothing moves: the latest volume just stops at its `lastId` from its next render.
- **Writing off:** journals stay, read-only (the editor isn't registered) and still rendered. A save with journals tells the player once per game run, after the post-load sync has loaded the volumes (`WarnIfWritingOff` in `main.cpp`: a message box with `[Messages] WritingOff` and the game's OK, like the missing-templates warning, plus a banner in the log). It can't be done at startup, where the missing-templates warning is: only the save says whether there are journals, and a player who never installed writing shouldn't be told.
- A journal whose book isn't in the loaded save (it was made later) is dropped like any volume ([BOOK_FORMS.md](BOOK_FORMS.md#load)) and not remade; see Numbering above for its entries.
- DiaryDBs from before 2026-09-29 are migrated once, every volume a diary ([DATABASE.md](DATABASE.md#schema-changes)). Entries already written through the editor then leave the diary; untagged, they show in journal 1.
- **Journals on SkyrimNet's Keep/Clear** (`ReconcileWithTimeline`): the journals in the save stay, whatever their entries (a Clear can empty one); each is re-rendered if its entry count changed.

### Blank journals

"Blank Journal" books in the ESP, one per journal look (`SNPD_BlankJournal1`–`3`, and `SNPD_BlankJournalN` for the Nightingales; [PLUGIN.md](PLUGIN.md#records)), 20 gold. `BlankJournals::OnDataLoaded` (at `kDataLoaded`):

- **Writing on:** names them from the locale file (`[Format] BlankJournal`; the ESP's name is English), and adds `SNPD_LItemBlankJournal` (one of the three at random) to Skyrim.esm's `LItemMiscVendorMiscItems75` **in memory**: the general-goods list that already sells the Roll of Paper, rolled by 18 merchant chests (Belethor, the Riverwood Trader, Bits and Pieces, the Khajiit caravans, …) and by two loot containers (`Cupboard01`, `PersonalChestSmall`). No vanilla record is overridden, so no other mod's edit of that list can drop them, and no patch is needed. A chest restocks every 48 game hours, so an existing save's merchants carry them from their next restock.
- **Crafting:** at a tanning rack, 1 Leather + 2 Roll of Paper, one recipe per look. A fourth, in the Nightingale look, unlocks once the player has taken the Nightingale Oath (TG08A "Trinity Restored" stage 57, or the quest done); merchants never sell it.
- **Writing off:** the recipes lose their workbench (so they're hidden), and merchants never get the list.

### Reading a blank journal

Opening a blank journal **from the player's own inventory** turns it into a new journal where it is: the book stays open and becomes the journal (the two share the model). There is nothing else to do with a blank journal and it closes no other journal, so there is no question. Read in the world (`BookMenu::GetTargetReference`), in a container, a shop or the gift menu, with SkyrimNet unable to save (`EditNeedsSkyrimNet`), or with a book menu that doesn't pause the game (`EditNeedsPause`: the conversion is a UI task, on the game thread only while the menu pauses it), it's just an empty book. It needs no quill: it only becomes a journal, and writing starts as in any journal (the edit or new-entry key), with its checks.

1. The book menu opening (`MenuOpenCloseEvent`) checks all that (`NoteBlankJournal`, a UI task) and marks the blank (`g_blankOnOpen`); the `AdvanceMovie` hook waits for its text to be in (`EditReady`) and runs `ConvertBlankJournal`. Closing and reopening instead played the close animation over the opening one (found in game, 2026-09-30).
2. `StartJournal` makes a new, empty journal beside the others ([Numbering](#diaries-and-journals)), in the blank's look (`BlankJournals::LookOf`: its template's EditorID, passed to `CreateEmptyVolume`, so the journal keeps that look for good). It becomes the new-entry key's first pick (`g_lastJournal`).
3. Only then is the blank used up (`RemoveItem`, one); if the volume couldn't be made, the player keeps the blank and gets `BlankJournalFailed`.
4. The book menu is pointed at the journal (`SetBookMenuBook`): the engine keeps the book it shows in globals, the base form (`BookMenu::GetTargetForm`, IDs 519295 / 405835, VR `0x3011200`) and the inventory item's extra data list (519294 / 405834; VR `0x30111F8`, not in VR's address library: 8 bytes before the form's, as on SE, both written by `OpenBookMenu`), which is cleared because it was the blank's. The book shows the journal's pages (the SWF's `ReplaceBookText`, with the volume's rendered text) and the player gets `EditStartedVolume`. It stays in reading: starting to write straight away was inconsistent with every other journal (changed 2026-09-30). The edit key on a journal with no entries starts a new entry (`BeginWriting`), since there's nothing else to write in.

### Quill and ink

Writing needs a **quill**, and each writing session **one use of ink**; reading is free. Both are vanilla clutter the ESP lists (`SNPD_Quills`: `Quill01`, the Quill of Gemination `FVDQuill`; `SNPD_Inkwells`, full inkwells: `Inkwell01`; [PLUGIN.md](PLUGIN.md#records)), so a patch or the Physical Letters mod can add its own. `WritingTools`:

- **Writing starts** (the edit key or the new-entry key: `BeginWriting`) only with a quill: without one, a message box (`EditNeedsQuill`, OK only: `ShowNotice`) states the requirements and writing doesn't start. With a quill but no ink (`WritingTools::HasInk`), the blood prompt comes then, before edit mode ([Writing in blood](#writing-in-blood)). Both are checked in `SendDiaryContent`.
- **Ink** is used on the session's **first key that changes the text** (a character, Enter, Backspace or Delete; `CanWrite` in `HandleKey`), once. A key that changes nothing costs nothing: the caret in no entry, Backspace at an entry's start or Delete at its end (the SWF's `EditCanErase`). After it, the rest of the session is free, however many entries it touches, so a session never runs dry halfway through an entry. Opening the editor and closing it, moving the caret, adding an empty entry or tearing one out use none, and discarding the changes gives nothing back. Having ink was checked when writing started, so it's there (the menu pauses the game).
- **A partly used inkwell is its own item**: the ESP's `SNPD_Inkwell1` to `SNPD_Inkwell9`, clones of the vanilla inkwell, one per uses left out of 10 (`kUses`). A use swaps the player's **emptiest** inkwell for the next one down (`UseInk`: `RemoveItem`, then `AddObjectToContainer`), a full one for `SNPD_Inkwell9`; the last use removes it, with the corner notification `EditInkRanDry`. The emptiest goes first so there is only ever one partly used inkwell. The state is the item, so it goes wherever the inkwell goes (dropped, stored, sold) and a reload restores it with the rest of the inventory. **Nothing needs refreshing**: with the book opened from the inventory, the plain swap shows correctly in the inventory afterwards (tested on AE, 2026-09-30). Tried and dropped along the way, each of which made things worse: `SendInventoryUpdateMessage` while the book was open (broke the inventory's navigation), `ItemList::Update` after it closed, and `SendInventoryUpdateMessage` per changed item after it closed (the inkwell line still showed the old one). The navigation break first seen with the clones was most likely the old blank-journal conversion, which closed the blank and reopened the journal from its base form over the inventory; the in-place conversion doesn't. The clones get the game's own name for an inkwell at load (the ESP's is English), so they look like any inkwell.
- **How full it is** shows only with **Description Framework** (optional): `SkyrimNet Physical Diaries_DESC.ini`, which the build deploys to the mod's root, where DF reads every `*_DESC.ini`, gives each clone a description ("About half full."), and the vanilla `Inkwell01` "Full.". DF keeps the first line it reads for a form unless a later one has a higher priority, so another mod's description of `Inkwell01` can take its place. DF puts it on the item card, disguising a misc item's card as a book's. The file is English only (DF's config has no languages). Without DF a partly used inkwell is a second "Inkwell" line.

**An earlier design** kept the fill on the item itself (`ExtraHealth`, which `ExtraDataList::SaveGame` writes for any inventory item exactly like a stack count) and showed it in a custom name (`ExtraTextDisplayData`). It worked, but renamed the item, and splitting one inkwell off a stack needed the engine's `ExtraDataList` constructor, which CommonLib declares without defining: 0x18 bytes and ID 11437 on SE, 0x20 bytes and ID 11583 on AE, 0x18 bytes at `0x117C80` on VR (found through `InventoryChanges::EnchantObject`, 2026-09-30). Kept here in case per-item state is needed again.


### Writing in blood

With a quill but **no ink**, starting to write asks (`EditBloodPrompt`, from `SendDiaryContent`): **Write in blood**, which starts writing in blood (`g_bloodChosen`, then `BeginWriting` again, with the new entry if the new-entry key asked), or **Put the quill down** (the cancel button: the book stays in reading). In blood (`g_blood`, and the SWF's `EditSetBlood`):

- **Each entry costs 10% of the player's maximum health** (`kBloodCost` of `GetPermanentActorValue(kHealth)`), charged on the first change to that entry in the session (`BledFor`: the entry under the caret, by `SessionKey`, which for a new entry stays its local key when its add returns an id mid-session), once. If it would leave them under 1 health (`kMinHealthAfterBleeding`), a message box says they're too weak (`EditTooWeak`) and the key does nothing. The next session asks again (ink first, if there is any).
- **What's typed is dark red** (`#2B0202`, `BookMenu.BLOOD_COLOR`; vanilla ink is `#000000`, and the lit book page brightens colours, so `#6B0F0F` and `#420606` read as bright red in game), and stays red: reopened later with ink, the red stays; text typed in ink inside a red passage is ink.
- **The red is kept apart from the text**: SkyrimNet stores the plain text (markup there would reach memories and prompts). The red is a list of `[start, end)` byte ranges of the entry's content (`"s:e,s:e"`), in DiaryDB's `blood` table with a hash of the content they belong to ([DATABASE.md](DATABASE.md#schema)). If the content no longer matches (edited outside SNPD), the red is dropped rather than painted on the wrong words, and the log says so once per entry with both hashes (a SkyrimNet that changed the text as it stored it would look the same to the player).
- **In the editor and while rendering it is marked inline** instead, between two private-use characters, U+E000 and U+E001 (`kBloodOpen`, `kBloodClose`; `MarkBlood` / `SplitBlood` in `BookText`), so no offsets have to be kept in step between C++'s UTF-8 and the SWF's UTF-16. `EditableEntryText` gives the editor marked text; the SWF strips the markers into per-entry ranges (`aSegs[k].blood`, `ParseBlood`), keeps them in step with typing and deleting (`BloodInsert`, `BloodDelete`), repaints them after every edit (`FormatBreaks` resets the whole body to ink first), and hands marked text back (`EditGetBodies`, `MarkBlood`). A save splits it (`SplitBlood`): the plain text goes to SkyrimNet, the ranges to DiaryDB once SkyrimNet has the entry (`CompleteWrite`, when the id is known; a tear-out deletes the row). Rendering (`SanitizeBookText`) marks the content, sanitizes and escapes it, then turns the markers into `<font color='#2B0202'>…</font>`. `MarkBlood` first removes any marker already in the text (an NPC's entry is LLM output), so only DiaryDB's ranges become markup. `BookManager::GetLiveEntries` reads journal entries' ranges from DiaryDB.
- **An entry begun in blood has a red heading too** (`DiaryEntry::bloodHeading`: a new entry added while writing in blood). It's the entry's, not its text's: it stays red if the blood text is later retyped in ink, and if the text was changed outside SNPD. The SWF gets the heading between the blood markers as well (`HeadingFor`), and the renderer colours the date line.
- **SkyrimNet knows**: an entry with any blood (text or heading) is tagged `snpd_written_in_blood` (next to `snpd_player_written`), and loses the tag when no red is left.
- This added calls between the plugin and the SWF, so the writing interface became 2; `EditCanErase` (only a key that changes the text pays) made it 3 ([Writing mode](#writing-mode)).

---

## Opening

1. While the player reads a book, the **edit key** (`[Diary] EditKey`, default F3, set in the MCM) queues `BeginWriting(false)` as a UI task; the new-entry key queues `StartNewEntry`, which checks the journal has room and calls `BeginWriting(true)` ([New entries](#new-entries)). `BeginWriting` runs `EnterEditMode`, then adds an entry if asked, or if the journal has none (there'd be nowhere to type). The book menu doesn't get the key. Nothing on screen mentions the key; the README and the MCM tooltip tell players.
2. `EnterEditMode` → `SendDiaryContent` checks, in order, and does nothing (logged) if one fails:
   - the open book (`BookMenu::GetTargetForm`) is one of SNPD's volumes (`BookManager::GetBookForFormID`) and a journal (any other book isn't editable: an NPC's diary above all, and the player's own diary too);
   - **SkyrimNet can save** (`Database::CanWriteDiaries`: both v11 functions resolved), or the player gets `EditNeedsSkyrimNet`;
   - **the book menu pauses the game** (`UI::GameIsPaused`), or the player gets `EditNeedsPause`. Key handling runs straight from the input sink and relies on the paused menu's input arriving on the main thread (Skyrim Souls RE, for one, can unpause the book menu);
   - **a quill** (`WritingTools::HasQuill`), or the requirements box (`EditNeedsQuill`, [Quill and ink](#quill-and-ink));
   - **ink** (`WritingTools::HasInk`), unless the player just chose blood, or the blood prompt ([Writing in blood](#writing-in-blood)), whose Write in blood runs `BeginWriting` again.
3. The entries: `BookManager::GetShownEntries` (the same entries `RefreshVolumeOnOpen` renders), or the editor's own copy if writes to this volume are still in SkyrimNet (see [Saving](#saving)). They go to the SWF as `SetEditContent(font, title size, small size, date size, content size, title, dates, entries)`, packed as `heading \x1F body` and joined by `\x1E`. The plugin keeps each one in `g_edit`: the whole `DiaryEntry` plus the text it was given (`savedBody`).
4. The SWF's `EnterEditMode` builds the editor. **Editing starts on the page being read:** the SWF takes the reading view's page (`iLeftPageNumber`) and, for a book, which engine slots its spread is in (`iLeftPageNumber - iPageSetIndex`), and turns the editor there. Both views break pages the same way, so it's the same page. The caret goes to the first place to type on it. A page with nowhere to type (the title spread) turns to the first entry's page instead; with no entry at all the view stays and there is no caret: the field is switched to non-selectable display text (`EditSetCaretOrNone`), because an unfocused input field still draws one. The caret comes back with a new entry (`EditSetCaret`).
5. Text input is allowed (`ControlMap::AllowTextInput`) while editing.

A `SetEditContent` or `EnterEditMode` that fails means the loaded `book.swf` isn't SNPD's after all (see [Writing mode](#writing-mode)); edit mode stays off.

---

## The SWF

### One field, one page shown

All the text is in one input `TextField` (a copy of the reference page clip) that is 20000 px tall and never scrolls. A mask shows one page of it: `ShowEditPage(p)` moves the field up by the page's top and redraws the mask to the page's height. (A fixed-height input field scrolls a line at a time to follow the caret, which looked like endless scrolling.)

### Segments

`EditBuildContent` lays the book out as `FormatDiaryEntries` does, as segments `{locked, body, editable}` (lengths, in order):

| Segment | Text | Editable |
|---|---|---|
| 0 | Nothing: the blank first page | No |
| 1 | `\r` + blank lines, the title (title size, centred), the date range (small size, centred) | No |
| 2 … | Locked: `\r\r`, then the heading (date size) and `\r\r` if headings are on. Body: the entry's text (content size) | Body only |

Every segment after the first starts with a locked `\r` and a new page, as entries do when reading. Line breaks are formatted as reading has them (`FormatBreaks`): the blank line between paragraphs in the page's own font and size, a line break inside a paragraph at the content size. Without content there is one editable segment (unused by SNPD; kept for the planned letters mod).

Every edit goes through the SWF's own functions (`AppendEditChar`, `EditBackspace`, `EditDelete`, `EditMoveCursor`), so each one adjusts exactly one body's length. The caret is only ever inside a body: `EditSnap` moves it out of locked text in the direction of travel, typing outside a body goes to the next one, Backspace stops at a body's start and Delete at its end. `EditGetBodies` returns the bodies in order, joined by `\x1E`, with `\n` line breaks; it returns `undefined` once the editor is gone.

### Pages

`EditLayout` runs after every edit and caret move. It uses the reading view's rule (`CalculatePagination`: a line whose bottom passes the page height starts a new page), plus a forced break at each segment's first line. It records each page's top and first line, and shows the caret's page.

**Books show four page slots.** The engine draws the open book by calling `ShowPageAtOffset(0 … 3)`: the visible spread and the two sides of the leaf being turned. Vanilla's `TurnPage` window means that after a forward turn the visible spread is in slots 2–3, after a backward turn in slots 0–1. Edit mode keeps the same bookkeeping (`iEditShownFrom`); treating slot k as "k pages after the spread" showed pages that don't exist after a turn. Notes use slot 0 only.

**Turning** (the engine's `TurnPage`) goes only to a page that exists; returning false skips the animation. A turn moves the caret to the first place to type on the target page; a page with nowhere to type (the blank and title pages) is shown without moving the caret, and typing brings the caret's page back.

---

## Input

The input sink is **prepended** to `BSInputDeviceManager`, so it runs before the menu's own input handling. While writing, the game is paused and input arrives on the main thread (checked when writing starts).

- **Keys** are turned into text with the active keyboard layout (`ToUnicode`; a dead key that doesn't combine gives both characters), with held-key repeat, and sent to the SWF. Arrows, Home and End move the caret; Backspace, Delete and Enter edit.
- **Every keyboard event's user event is blanked**, so no key acts as a game or menu control while typing (E types an "e" instead of taking the book). **Other mods' hotkeys can still fire while typing**: many read raw key codes or poll the keyboard themselves. SkyrimNet polls with `GetAsyncKeyState`, and its dashboard and book capture keys work in the book menu (the capture then gets the unsaved editor's text). Zeroing the key events for the sinks after ours was tried and made no measurable difference, so it was dropped (2026-09-30); SkyrimNet could skip its capture while `ControlMap` text input is on, which SNPD turns on while editing. Until then players bind such keys to keys that don't type ([KNOWN_ISSUES.md](KNOWN_ISSUES.md)).
- **The book menu turns pages on the arrow keys, A and D by key code, not by user event** (found in game: the arrows reached our sink already without a user event), so blanking can't stop it. Instead every key event (press, held repeat, release) calls `EditSuppressTurn`, and the SWF refuses turns for 200 ms after it. A click never comes with a key event.
- **The mouse** keeps the book's own page turns: left click previous, right click next (the `Book` context of `controlmap.txt`). Clicking doesn't place the caret.
- **The edit key** saves and goes back to reading (see [Saving](#saving)); **the delete key** asks to tear out the entry under the caret (see [Tearing out an entry](#tearing-out-an-entry)); **the new-entry key** adds an entry at the end (see [New entries](#new-entries)). None is typed.
- While a prompt is open over the book, the sink leaves every key to it.

---

## Saving

Two ways: the **edit key** while writing (`SaveAndRead`: save, then back to reading), or **Save** on the close prompt (save, then close).

`Save` reads the bodies (`ReadBodies`) and compares each with its `savedBody` (line breaks normalized to `\n` as the SWF returns them). An entry is written if its text changed, or if its last save was refused (`unsaved`). For each:

- The entry in `g_edit` takes the new text, the `snpd_player_written` tag (`kPlayerWrittenTag`) and the tag of the journal being edited (`SetJournal`, `g_journal`), keeping its other tags, and the volume is re-rendered from the edited entries at once (`SetVolumeText`): the book, its text snapshot and DiaryDB's cache show the edit before SkyrimNet has it. `RefreshVolumeOnOpen` skips the volume until its writes are done (`HasPendingWrites`), or it would put SkyrimNet's old text back.
- An update job goes to the **write queue**: one worker thread that runs the editor's writes in order (`PublicUpdateDiaryEntry` blocks while SkyrimNet re-embeds the entry's memory), so two quick saves of the same entry can't land out of order.
- Each finished job queues a game-thread task. When a volume's last pending write is done, `BookManager::ReconcileAfterWrite` re-renders it from SkyrimNet.
- **Pending writes, per volume** (`g_pending`): until they are done SkyrimNet still has the old text, so editing that volume again (the edit key right after saving) starts from the editor's saved entries, not from SkyrimNet.
- **A refused or failed save** (for example while SkyrimNet's keep/clear timeline check is pending) shows `EditSaveFailed`. The entry's text stays in `g_pending`, marked unsaved, so the next edit of that volume starts from it and saving writes it again. (The book shows SkyrimNet's text meanwhile.)
- **A load or new game** (`BookEditor::Reset`, from `EndSession`) bumps a generation and drops the edit state: a write that finishes afterwards is ignored instead of touching a volume in the new session.

**The editor's text can't be read** (the SWF returned nothing, or not one text per entry): the edit key and the prompt's Save show `EditSaveFailed` and stay in edit mode, so nothing is thrown away while the player can still see it. A close with unreadable text goes through (logged): there is nothing to save.

**An emptied entry is left as it was**, with a hint (`EditEmptiedHint`): removing an entry is tearing it out (below), which asks first.

**Back to reading** (`ReturnToReading` in the SWF): the plugin renders the volume from the edited entries (`FormatDiaryEntries`, then `BookTextHook::ForBookMenu`, the book menu's Win-1251 step), ends edit mode, and hands the text to the SWF. The SWF drops the editor and the old reading pages and lays the text out as the engine's `SetBookText` does, on the spread being edited: `iLeftPageNumber` is that spread's left page and `iPageSetIndex` puts it in the engine slots it is shown in (`iEditShownFrom`). A spread in slots 2–3 on the first spread leaves slots 0–1 before page 0, so `UpdatePages` skips page numbers below 0.

---

## Tearing out an entry

The **delete key** (`[Diary] DeleteKey`, default F10, set in the MCM) while writing asks about the entry the caret is in (`EditCurrentEntry`): "Tear out the entry from {Date}? It will be gone from your journal and from your memory." (`EditDeletePrompt`, with `EntryDate`, so the date shows even with headings off), **Tear out** or **Keep it** (the cancel button). The caret outside any entry does nothing.

**Tear out** (`TearOut`) acts at once:

- The SWF removes the entry's segment, heading and text (`EditRemoveEntry`); the caret goes to the end of the entry before. `g_edit` drops it, so the other entries keep their indexes in step with the SWF. Their unsaved changes stay in the editor.
- The volume is re-rendered from the remaining entries, and a delete job goes to the write queue (`PublicDeleteDiaryEntry`: the entry and its memory), counted in the volume's pending writes like a save.
- On failure the player gets `EditDeleteFailed`; the volume's pending entries are dropped when its writes finish (they lack an entry SkyrimNet still has), and the reconcile puts the entry back in the book.
- Tearing out every entry leaves the title page and a blank page (a journal never shows the "all entries removed" notice diaries get; see [BOOK_TEXT.md](BOOK_TEXT.md#rendering-formatdiaryentries)). The book stays, ready for new entries.

Tearing out an entry makes room in a full journal, and never moves another entry: each is in the journal its tag names.

---

## New entries

The **new-entry key** (`[Diary] NewEntryKey`, set in the MCM; unbound by default, because during play any key we picked would clash with some other mod's) with the player's journal open:

- **In any journal with room**: journals sit side by side ([Diaries and journals](#diaries-and-journals)). A full one (`EntriesPerVolume` entries) gets `EditJournalFull`: while reading, before writing starts (`JournalFull`: the rendered count, `lastKnownEntryCount`, or the pending writes' entries); while writing, by the editor's own count (`AppendNewEntry`).
- **While reading** (`StartNewEntry`): writing starts (the same checks as the edit key) with a new entry. **While writing** (`AppendNewEntry`): the new entry is added; if one is already there and not saved, the key goes back to it (`EditFocusEntry`), one at a time.
- **The new entry** (`EditAppendEntry` in the SWF) is a new page at the end: its heading (today's date, if headings are on) and an empty body with the caret in it. In the plugin it is a `DiaryEntry` with no id and a **local key** (`localKey`, from `g_nextLocalKey`), dated now and tagged for its journal.
- **Saving it** queues an **add** (`PublicAddDiaryEntry`, with that date and the `snpd_player_written` and journal tags) instead of an update. The worker records the id SkyrimNet returns against the local key (`ids_`); the completion task writes it into the editor's and the pending entries. Until then, later saves and tear-outs of the entry are queued by local key, and the worker finds the id because the add ran first. An add that fails leaves the entry unsaved, and the next save adds it again (`g_addQueued`).
- **Left empty**, a new entry is never written and doesn't count as a change (no save prompt); torn out before it was ever saved, it just leaves the editor.
- SkyrimNet announces the entry (`SkyrimNet_DiaryCreated`) as it does its own. The pipeline handles only the player's diary (not at all with `PlayerDiaryBooks` off), and the kind filter keeps journal entries out of it ([Diaries and journals](#diaries-and-journals)); the editor's `ReconcileAfterWrite` renders the journal.

Journals were split by time at first, like diaries, with a new one starting after the last one's final entry. That made every new journal close the last for good, and an entry still being saved when a blank journal was read could end up between two journals, in neither. Tags removed both (2026-09-30); journals had no entry limit for a day in between.

### Outside the book

The new-entry key also works **during play** (no menu pausing the game; the input sink queues `NewEntryFromPlay` as a game-thread task): it opens a journal (`BookMenu::OpenMenuFromBaseForm`) and starts a new entry once its text is in (`OpenForNewEntry`, then the `AdvanceMovie` hook). The journal is the one the player last wrote in (`g_lastJournal`: set by a save, a tear-out or a blank journal becoming a journal; forgotten on load) if they carry it and it has room, else the newest one they carry with room. With no journal the player gets `EditNoJournal`; with none on them (lost, sold, put away; `CountInInventory`), `EditJournalNotCarried`; with every one they carry full, `EditJournalFull`. The key never makes a journal: SNPD never puts an empty journal in the player's inventory on its own, a journal comes only from reading a blank journal.

---

## Closing

Every way of closing the book reaches the menu through `BookMenu::ProcessMessage`, which `BookEditor` hooks (vtable index 4, the same code on SE, AE and VR):

| Message | With unsaved changes | Without |
|---|---|---|
| `kUserEvent` "Cancel" (gamepad B) | Held back (`kHandled` consumes it), the save prompt opens | Passed on; edit mode ends |
| `kHide` (Escape, the menu's buttons, other mods) | Held back with `kIgnore`, the save prompt opens | Passed on; edit mode ends |
| `kForceHide` (loads, the game shutting menus) | Passed on: the changes are lost | Passed on |

- **"Cancel" has to be caught itself:** the menu answers it by playing its close animation and only then sending `kHide`, so holding back the hide left the prompt over a book that looked closed.
- **`kIgnore` keeps the menu open.** `kHandled` tells the UI the hide was done, and the UI removes the menu itself (found in game). While the prompt is open the menu resends the hide every frame; each one is refused.
- **Edit mode ends as soon as a close is let through,** because the menu sends a second `kHide` after its close animation, by which time `PrepForClose` has dropped the SWF's editor.
- Escape is handled by the input sink (text input would swallow it) and just asks the menu to close (`RequestClose`), so it takes the same path.

**The prompt** is a vanilla message box over the book: Save, Discard, Keep writing (its cancel button, so Escape on the prompt keeps writing). While it is open the input sink leaves keys alone and text input is off, so the prompt's own keys work. Save and Discard end edit mode and close (`CloseBook`); the hook then lets that close through.

---

## Text

The editor gets the text the reading view shows, as plain text: `EditableEntryText` (the sanitizing of [BOOK_TEXT.md](BOOK_TEXT.md#cleaning-llm-output-sanitizebooktext), without the markup escaping), `EntryHeading` (empty when headings are off) and `TitlePageDates`. When the player edits an entry, what they saw becomes the stored text: lines the reading view hid (an LLM's own date heading) are dropped from that entry only.

**Player-written entries skip the date stripping** (`IsPlayerWritten`), so a player's entry that starts with a date keeps it. The typographic cleanup still applies (the handwriting fonts lack those characters).

---

## Not done yet

- **Shipping the SWF.** `Interface/book.swf` replaces the book menu for every book and note. It needs a FOMOD (vanilla, Convenient Reading, none). The interface version check and the warning when writing is off but the save has journals are done ([Writing mode](#writing-mode), [Diaries and journals](#diaries-and-journals)). The planned Physical Letters mod will ship the same SWF, so its interface must stay mod-neutral (no `SNPD_` events).
- **SE, VR and the Convenient Reading variant** are untested, including plain reading through SNPD's `book.swf` on VR. VR also needs a keyboard story, and `SetBookMenuBook`'s VR extra-list address (`0x30111F8`) was found in the binary, not in VR's address library, and hasn't run.
- **Cyrillic.** Reading needs Win-1251 because Scaleform's pagination mixes byte and character offsets; the editor gets UTF-8. Untested with Cyrillic text.
- **Translations**: every writing string, in the locale files and the MCM, is in all nine languages, as first drafts written without native speakers (like `JournalTitle`); they stay until native feedback.
