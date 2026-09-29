# Player Diary Editing

The player can edit their own diary in the game's book menu: the pages look as they do when reading, and the entries' text is editable in place. Saving writes the changed entries back to SkyrimNet.

Status (2026-09-28, branch `player-writing`): editing existing entries works on AE. New entries, deleting an entry, SE, VR and the Convenient Reading variant are not done or not tested. See [Not done yet](#not-done-yet).

Code: `swf/book/scripts/__Packages/BookMenu.as` (the edit mode in the book menu's SWF), `src/BookEditor.cpp` (the plugin side), `Database::UpdateDiaryEntry` (SkyrimNet public API v11).

---

## Rules

- **Only the player's own diary.** A volume is editable only when its owner's UUID is the player's (`Database::GetUUIDFromFormID(0x14)`). An NPC's diary entries are also that NPC's memories in SkyrimNet; editing one would rewrite what they remember.
- **Only changed entries are written.** An entry the player didn't touch stays exactly as SkyrimNet has it, including the lines the reading view hides (see [Text](#text)).
- **Headings and the title page are not editable.** Each entry's date heading is the anchor that ties the text under it to one SkyrimNet entry id.

---

## Opening

1. While the player reads a book, the **edit key** (`[Diary] EditKey`, default F3, set in the MCM) queues `EnterEditMode` as a UI task. The book menu doesn't get the key. Nothing on screen mentions the key; the README and the MCM tooltip tell players.
2. `EnterEditMode`:
   - `SendDiaryContent`: the open book (`BookMenu::GetTargetForm`) → `BookManager::GetBookForFormID` → the player check → `BookManager::GetShownEntries` (the same entries `RefreshVolumeOnOpen` renders). It sends the SWF `SetEditContent(font, title size, small size, date size, content size, title, dates, entries)`, with the entries packed as `heading \x1F body`, joined by `\x1E`, and keeps each entry's id, text and tags (`g_loaded`).
   - Any other book, and another actor's diary above all, isn't editable: nothing happens (logged).
   - `SetEditMode(true)`. The SWF builds the editor at once, since the book's text (and whether it's a note) is already known; if edit mode were ever asked for before the engine's `SetBookText`, it waits for it (`bEditPending`).
   - **Editing starts on the page being read.** The SWF takes the reading view's page (`iLeftPageNumber`) and, for a book, which engine slots its spread is in (`iLeftPageNumber - iPageSetIndex`), and turns the editor there. Both views break pages the same way, so it's the same page. The caret goes to the first place to type on it; on the title spread the view stays and the caret waits on the first entry.
3. Text input is allowed (`ControlMap::AllowTextInput`) while editing.

A `SetEditContent` or `SetEditMode` that fails means the loaded `book.swf` isn't SNPD's (another mod's won the file conflict); edit mode stays off.

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
| 2 … | Locked: `\r`, then the heading (date size) and `\r\r` if headings are on. Body: the entry's text (content size) | Body only |

Every segment after the first starts with a locked `\r` and a new page, as entries do when reading. Without content (a note, dev harness) there is one editable segment.

Every edit goes through the SWF's own functions (`AppendEditChar`, `EditBackspace`, `EditDelete`, `EditMoveCursor`), so each one adjusts exactly one body's length. The caret is only ever inside a body: `EditSnap` moves it out of locked text in the direction of travel, typing outside a body goes to the next one, Backspace stops at a body's start and Delete at its end. `EditGetBodies` returns the bodies in order, joined by `\x1E`, with `\n` line breaks; it returns `undefined` once the editor is gone.

### Pages

`EditLayout` runs after every edit and caret move. It uses the reading view's rule (`CalculatePagination`: a line whose bottom passes the page height starts a new page), plus a forced break at each segment's first line. It records each page's top and first line, and shows the caret's page.

**Books show four page slots.** The engine draws the open book by calling `ShowPageAtOffset(0 … 3)`: the visible spread and the two sides of the leaf being turned. Vanilla's `TurnPage` window means that after a forward turn the visible spread is in slots 2–3, after a backward turn in slots 0–1. Edit mode keeps the same bookkeeping (`iEditShownFrom`); treating slot k as "k pages after the spread" showed pages that don't exist after a turn. Notes use slot 0 only.

**Turning** (the engine's `TurnPage`) goes only to a page that exists; returning false skips the animation. A turn moves the caret to the first place to type on the target page; a page with nowhere to type (the blank and title pages) is shown without moving the caret, and typing brings the caret's page back.

---

## Input

The input sink is **prepended** to `BSInputDeviceManager`, so it runs before the menu's own input handling.

- **Keys** are turned into text with the active keyboard layout (`ToUnicode`), with held-key repeat, and sent to the SWF. Arrows, Home and End move the caret; Backspace, Delete and Enter edit.
- **Every keyboard event's user event is blanked**, so no key acts as a game or menu control while typing (E types an "e" instead of taking the book).
- **The book menu turns pages on the arrow keys, A and D by key code, not by user event** (found in game: the arrows reached our sink already without a user event), so blanking can't stop it. Instead every key event (press, held repeat, release) calls `EditSuppressTurn`, and the SWF refuses turns for 200 ms after it. A click never comes with a key event.
- **The mouse** keeps the book's own page turns: left click previous, right click next (the `Book` context of `controlmap.txt`). Clicking doesn't place the caret.
- **The edit key** saves and goes back to reading (see [Saving](#saving)).

---

## Saving

Two ways: the **edit key** while writing (`SaveAndRead`: save, then back to reading), or **Save** on the close prompt (save, then close).

`Save` reads the bodies (`ReadBodies`) and compares each with the text it was given (`g_loaded`, line breaks normalized to `\n` as the SWF returns them). For each changed entry:

- The editor's copy of the entries (`g_entries`) takes the new text and the tag, and the volume is re-rendered from it at once (`SetVolumeText`): the book, its text snapshot and DiaryDB's cache show the edit before SkyrimNet has it. (`RefreshVolumeOnOpen` alone would not: it re-renders only when the entry count changes.)
- The entry's tags plus `snpd_player_written` (`kPlayerWrittenTag`) are sent with the new text to `PublicUpdateDiaryEntry`, on a detached thread: SkyrimNet re-embeds the entry's memory, which blocks.
- When the writes are done, a game-thread task re-renders the volume from SkyrimNet (`GetShownEntries`, `SetVolumeText`).
- **Until then SkyrimNet still has the old text,** so editing the same volume again (the edit key right after saving) starts from the saved entries (`g_pending`), not from SkyrimNet.
- If SkyrimNet refuses (for example while its keep/clear timeline check is pending) or the call fails, the player gets the `EditSaveFailed` notification.

**An emptied entry is left as it was** (logged): removing an entry is tearing it out (below), which asks first.

**Back to reading** (`ReturnToReading` in the SWF): the plugin renders the volume from the edited entries (`FormatDiaryEntries`, then `BookTextHook::ForBookMenu`, the book menu's Win-1251 step), ends edit mode, and hands the text to the SWF. The SWF drops the editor and the old reading pages and lays the text out as the engine's `SetBookText` does, on the spread being edited: `iLeftPageNumber` is that spread's left page and `iPageSetIndex` puts it in the engine slots it is shown in (`iEditShownFrom`). A spread in slots 2–3 on the first spread leaves slots 0–1 before page 0, so `UpdatePages` skips page numbers below 0.

---

## Tearing out an entry

The **delete key** (`[Diary] DeleteKey`, default F10, set in the MCM) while writing asks about the entry the caret is in (`EditCurrentEntry`): "Tear out the entry from {Date}? It will be gone from your diary and from your memory." (`EditDeletePrompt`, with `EntryDate`, so the date shows even with headings off), **Tear out** or **Keep it** (the cancel button). The caret outside any entry does nothing.

**Tear out** (`TearOut`) acts at once:

- The SWF removes the entry's segment, heading and text (`EditRemoveEntry`); the caret goes to the end of the entry before. The editor's lists (`g_loaded`, `g_entries`) drop it, so the other entries keep their indexes in step with the SWF. Their unsaved changes stay in the editor.
- The volume is re-rendered from the remaining entries, and the edit-again guard (`g_pending`) holds them until SkyrimNet is done.
- `PublicDeleteDiaryEntry` deletes the entry and its memory, on a detached thread. Then a game-thread task runs `BookManager::ReconcileAfterDeletion`: a re-render from SkyrimNet and the `endTime` move (see [VOLUMES_AND_SYNC.md](VOLUMES_AND_SYNC.md)). On failure (refused while SkyrimNet's timeline check is pending, or an error) the player gets `EditDeleteFailed`, and the reconcile puts the entry back in the book.
- Tearing out every entry leaves the title page; back to reading shows the "all entries removed" page.

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

- **F4:** the SWF's `DebugState` (layout, pages, segments, turns, clips) in the log.
- **F5:** what a save would write, without writing.
- The state after entering edit mode and after the first three keys; every book-menu message while editing (except updates and Scaleform events); mouse buttons' user events; where a body differs from what was loaded, with the bytes around it.

---

## Not done yet

- **Going back to reading without closing the book.** The only way out of edit mode is closing it.
- **New entries.** Deliberately left for later (how to start one immersively).
- **SE, VR and the Convenient Reading variant** are untested. VR also needs a keyboard story.
- **Cyrillic.** Reading needs Win-1251 because Scaleform's pagination mixes byte and character offsets; the editor gets UTF-8. Untested with Cyrillic text.
- **Translations** of the five `[Messages] Edit…` strings: only English has them; other languages show the English defaults.
- **The shared `book.swf`.** The planned Physical Letters mod will ship the same SWF, and whichever mod wins the file conflict serves both. The SWF needs a version the plugin checks before enabling editing, and its interface must stay mod-neutral (no `SNPD_` events).
