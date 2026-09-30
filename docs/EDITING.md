# Player Diary Editing

The player can write in their own **journal** in the game's book menu: the pages look as they do when reading, and the entries' text is editable in place. Saving writes the changed entries back to SkyrimNet; an entry can be torn out (deleted), and new entries can be written at the end.

Status (2026-09-29, branch `player-writing`): editing, saving, going back to reading and tearing out entries work on AE; new entries and the next volume are built but not yet run in game. SE, VR, the Convenient Reading variant and Cyrillic text are not tested, and there is no FOMOD yet. See [Not done yet](#not-done-yet).

Code: `swf/book/scripts/__Packages/BookMenu.as` (the edit mode in the book menu's SWF), `src/WritingMode.cpp` (is it installed), `src/BookEditor.cpp` (the plugin side), `Database::AddDiaryEntry` / `UpdateDiaryEntry` / `DeleteDiaryEntry` (SkyrimNet public API v11), `BookManager::ReconcileAfterWrite` and `BookManager::CreateEmptyVolume`.

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
| `Written`: a journal | `[Format] JournalTitle`, "{Name}'s Journal" | The player's entries with the tag | The book editor only |

- **Each kind numbers its own volumes from 1**: the player can have Diary v1 to v3 and Journal v1. `BookManager` keeps each kind as its own chain (`ChainKey`: the UUID for the diary, so every lookup by UUID alone gets the diary; `"<UUID>|j"` for the journal). DiaryDB's key is `(actor_uuid, kind, volume_number)`, and a journal's co-save key is `"<UUID>|j<n>"`.
- **The filter** is in `Database::GetDiaryEntries`, so also `GetVolumeEntries`: for the player (FormID `0x14`) it keeps only the entries of the kind asked for, fetching them all and applying the limit afterwards (SkyrimNet's limit keeps the newest). Only the player has written entries, so for anyone else it changes nothing. Counts, seals and boundary data only ever see one kind.
- **The player's diary is a diary like any NPC's, read-only,** whether writing is on or off: it sits beside the journal, and the two titles tell them apart. With **`[Diary] PlayerDiaryBooks`** off (MCM "Your Diary Books"; by default off with writing installed, on without) it stops growing: `UpdateDiaryForActorInternal`, the recovery probe and the catch-up scan skip the player, and `GetShownEntries` bounds the player's latest diary volume by its `endTime` instead of leaving it open, so SkyrimNet's new AI-written entries for the player are in no book (they stay in SkyrimNet as memories; SkyrimNet's own settings can stop writing them). Turned on in the MCM, the diary catches up at once through the usual update (`UpdateDiaryForActorInternal(0x14)`, as a new entry would): the latest volume fills first if the player carries it, then new volumes; with no diary yet, every volume from 1. Turned on any other way (the INI, or the default flipping because `book.swf` came or went), the load's recovery probe or catch-up scan does the same. Turned off, nothing moves: the latest volume just stops at its `endTime` from its next render. A journal's latest volume is always open-ended.
- **Writing off:** journals stay, read-only (the editor isn't registered) and still rendered.
- A journal whose book isn't in the loaded save (it was made later) is dropped like any volume ([BOOK_FORMS.md](BOOK_FORMS.md#load)) and not remade. If SkyrimNet kept its entries (Keep), they show in the latest journal, which is open-ended, or in the next one.
- DiaryDBs from before 2026-09-29 are migrated once, every volume a diary ([DATABASE.md](DATABASE.md#schema-changes)). Entries already written through the editor then leave the diary; the first journal (volume 1 starts at time 0 and is open-ended) shows them all.
- **A journal on SkyrimNet's Clear:** a trailing journal volume left empty is kept if it started before the loaded save (it was in the save, made empty or torn out) and re-rendered empty; only one that started after the save is retired, as a diary's would be (`ReconcileWithTimeline`).

---

## Opening

1. While the player reads a book, the **edit key** (`[Diary] EditKey`, default F3, set in the MCM) queues `EnterEditMode` as a UI task (the new-entry key does the same, then adds an entry: see [New entries](#new-entries)). The book menu doesn't get the key. Nothing on screen mentions the key; the README and the MCM tooltip tell players.
2. `EnterEditMode` → `SendDiaryContent` checks, in order, and does nothing (logged) if one fails:
   - the open book (`BookMenu::GetTargetForm`) is one of SNPD's volumes (`BookManager::GetBookForFormID`) and a journal (any other book isn't editable: an NPC's diary above all, and the player's own diary too);
   - **SkyrimNet can save** (`Database::CanWriteDiaries`: both v11 functions resolved), or the player gets `EditNeedsSkyrimNet`;
   - **the book menu pauses the game** (`UI::GameIsPaused`), or the player gets `EditNeedsPause`. Key handling runs straight from the input sink and relies on the paused menu's input arriving on the main thread (Skyrim Souls RE, for one, can unpause the book menu).
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
- **Every keyboard event's user event is blanked**, so no key acts as a game or menu control while typing (E types an "e" instead of taking the book).
- **The book menu turns pages on the arrow keys, A and D by key code, not by user event** (found in game: the arrows reached our sink already without a user event), so blanking can't stop it. Instead every key event (press, held repeat, release) calls `EditSuppressTurn`, and the SWF refuses turns for 200 ms after it. A click never comes with a key event.
- **The mouse** keeps the book's own page turns: left click previous, right click next (the `Book` context of `controlmap.txt`). Clicking doesn't place the caret.
- **The edit key** saves and goes back to reading (see [Saving](#saving)); **the delete key** asks to tear out the entry under the caret (see [Tearing out an entry](#tearing-out-an-entry)); **the new-entry key** adds an entry at the end (see [New entries](#new-entries)). None is typed.
- While a prompt is open over the book, the sink leaves every key to it.

---

## Saving

Two ways: the **edit key** while writing (`SaveAndRead`: save, then back to reading), or **Save** on the close prompt (save, then close).

`Save` reads the bodies (`ReadBodies`) and compares each with its `savedBody` (line breaks normalized to `\n` as the SWF returns them). An entry is written if its text changed, or if its last save was refused (`unsaved`). For each:

- The entry in `g_edit` takes the new text and the `snpd_player_written` tag (`kPlayerWrittenTag`, keeping its other tags), and the volume is re-rendered from the edited entries at once (`SetVolumeText`): the book, its text snapshot and DiaryDB's cache show the edit before SkyrimNet has it. (`RefreshVolumeOnOpen` alone would not: it re-renders only when the entry count changes.)
- An update job goes to the **write queue**: one worker thread that runs the editor's writes in order (`PublicUpdateDiaryEntry` blocks while SkyrimNet re-embeds the entry's memory), so two quick saves of the same entry can't land out of order.
- Each finished job queues a game-thread task. When a volume's last pending write is done, `BookManager::ReconcileAfterWrite` re-renders it from SkyrimNet (and moves `endTime` back after a deletion).
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

Deleting an entry never moves another entry to a different volume, and the reconcile's `endTime` move stops the next load from making a duplicate volume: see [VOLUMES_AND_SYNC.md](VOLUMES_AND_SYNC.md).

---

## New entries

The **new-entry key** (`[Diary] NewEntryKey`, set in the MCM; unbound by default, because during play any key we picked would clash with some other mod's) with the player's journal open:

- **Only in the latest volume** (`BookManager::GetBookForActor(uuid, VolumeKind::Written)`): a new entry is dated now, so it belongs at the end of the journal. In an older volume the player gets `EditNotLatest`.
- **While reading** (`StartNewEntry`): writing starts (the same checks as the edit key) with a new entry. **While writing** (`AppendNewEntry`): the new entry is added; if one is already there and not saved, the key goes back to it (`EditFocusEntry`), one at a time.
- **The new entry** (`EditAppendEntry` in the SWF) is a new page at the end: its heading (today's date, if headings are on) and an empty body with the caret in it. In the plugin it is a `DiaryEntry` with no id and a **local key** (`localKey`, from `g_nextLocalKey`), dated `max(now, the volume's startTime)`.
- **Saving it** queues an **add** (`PublicAddDiaryEntry`, with that date and the `snpd_player_written` tag) instead of an update. The worker records the id SkyrimNet returns against the local key (`ids_`); the completion task writes it into the editor's and the pending entries. Until then, later saves and tear-outs of the entry are queued by local key, and the worker finds the id because the add ran first. An add that fails leaves the entry unsaved, and the next save adds it again (`g_addQueued`).
- **Left empty**, a new entry is never written and doesn't count as a change (no save prompt); torn out before it was ever saved, it just leaves the editor.
- SkyrimNet announces the entry (`SkyrimNet_DiaryCreated`) as it does its own. The pipeline handles only the player's diary (not at all with `PlayerDiaryBooks` off), and the kind filter keeps journal entries out of it ([Diaries and journals](#diaries-and-journals)); the editor's `ReconcileAfterWrite` renders the journal.

### A full volume

When the latest volume has `EntriesPerVolume` entries, the new-entry key starts the next volume (`StartNextVolume`):

1. Changes in this book are saved (as the edit key does; unreadable text stays, see [Saving](#saving)).
2. This volume's `endTime` is moved to its last entry, counting the ones just written: the menu pauses game time, so they are dated now, and once a newer volume exists this one is bounded by `endTime`.
3. `BookManager::CreateEmptyVolume` makes the next volume, empty, in the player's inventory, starting on the next whole game second (so the entries just written can't fall into it). The player gets `EditNewVolume`.

**No entry is dated on a volume's start.** A new entry in a volume with a start time is dated at least half a game second after it (`kInsideStart`). SkyrimNet truncates the date it stores (`duration_cast` to its clock's tick, then six decimals in the database), so an entry dated exactly on the start came back a hair before it and belonged to no volume (found in game, 2026-09-29: the entry was in SkyrimNet but in no book).
4. This book closes; when it has (`MenuOpenCloseEvent`), the new book opens (`BookMenu::OpenMenuFromBaseForm`, as reading it from the inventory does).
5. The book menu's text arrives after it opens (the engine's `SetBookText`), so a hook on the menu's per-frame `AdvanceMovie` (vtable index 5) waits until the SWF says it has it (`EditReady`), then starts a new entry.

### Outside the book

The new-entry key also works **during play** (no menu pausing the game; the input sink queues `NewEntryFromPlay` as a game-thread task). It is the way back to writing when the journal is lost:

| The player's latest journal volume | What happens |
|---|---|
| Carried, with room | It opens (`BookMenu::OpenMenuFromBaseForm`) and a new entry starts once its text is in (`OpenForNewEntry`, then the `AdvanceMovie` hook) |
| Carried, full | The next volume is made (`CreateNextVolume`, as for [a full volume](#a-full-volume)) and opens with a new entry; `EditNewVolume` |
| Not carried (lost, sold, put away; `CountInInventory`) | The next volume is made and opens, the same rule as when an NPC's latest volume is taken ([VOLUMES_AND_SYNC.md](VOLUMES_AND_SYNC.md#when-an-entry-arrives-updatediaryforactorinternal)): the missing book keeps its entries as an earlier volume. `EditStartedVolume` |
| None at all | Volume 1 is made, empty, and opens. `EditStartedVolume` |

SNPD never puts an empty journal in the player's inventory on its own: a journal appears only when the player asks for one with this key.

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

## Diagnostics (to remove)

- **F4** (while writing): the SWF's `DebugState` (layout, pages, segments, turns, clips) in the log.
- **F5** (while writing): what a save would write, without writing.
- The state after entering edit mode and after the first three keys; every book-menu message while editing (except updates and Scaleform events); where a body differs from what was loaded, with the bytes around it.

---

## Not done yet

- **Shipping the SWF.** `Interface/book.swf` replaces the book menu for every book and note. It needs a FOMOD (vanilla, Convenient Reading, none) and a message to the player (not just the log) when writing is off but the save has written volumes. The interface version check is done ([Writing mode](#writing-mode)). The planned Physical Letters mod will ship the same SWF, so its interface must stay mod-neutral (no `SNPD_` events).
- **SE, VR and the Convenient Reading variant** are untested, including plain reading through SNPD's `book.swf` on VR. VR also needs a keyboard story.
- **Cyrillic.** Reading needs Win-1251 because Scaleform's pagination mixes byte and character offsets; the editor gets UTF-8. Untested with Cyrillic text.
- **Translations** of the fifteen `[Messages] Edit…` strings: only English has them; other languages show the English defaults. The MCM's Writing strings in the other eight languages still say "diary". The `JournalTitle` translations are first drafts.
- **Blank journals, quill and ink** (planned): a journal will come from reading a blank journal item, and writing will need a quill and an inkwell. Until then the new-entry key makes journals.
