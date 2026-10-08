# Book Text

How a volume's entries become the text the player reads, and how that text reaches the book menu.

Code: `src/BookText.cpp` (`FormatDiaryEntries`, `SanitizeBookText`, `FormatGameDate`, `FormatGameDateShort`), `src/BookTextHook.cpp`, `src/Localization.cpp`.

---

## Rendering (`FormatDiaryEntries`)

The rendered text is Skyrim book markup: HTML-like `<font>` / `<p align>` tags and `[pagebreak]`. Layout:

| Page | Content |
|---|---|
| 0 | Blank |
| 1 | Title (`Localization::FormatTitle`: `DiaryTitle`, or `JournalTitle` for the player's journal) centred at `TitleSize`, then the earliest and latest entry dates at `SmallSize` (`TitlePageDates`: entries are in write order, so after a Keep the first page isn't the earliest, see [VOLUMES_AND_SYNC.md](VOLUMES_AND_SYNC.md#volumes)) |
| 2 … | One entry per page: an optional date header (`DateSize`), then the entry at `ContentSize` |

- **Font tags:** in the **marked** (editor) text every paragraph and every break between them gets its own `<font face size>` tag, since Skyrim resets the font after an empty line; the **reading** text has one tag per entry, its blank lines holding a character (next bullet).
- **Blank lines stay.** `Paragraphs` keeps empty pieces (only those at an entry's end are dropped). **Every break between paragraphs, the one under a date heading and the ones after the last entry are inside a `<font>` tag at `ContentSize`**, so a blank line is as tall as a text line. (In the editor, the line after an entry's text holds the breaks after it; in the page's bigger size that line overflowed to the next page and came back as soon as the player typed in it.) **An entry followed by a page break ends with one line break** (2026-10-07): the book ignores empty lines there, but the editor counted them, so an entry ending within about three lines of a page's bottom gave the editor one page more than the book from there on. One line break stays: with none, the `[pagebreak]` tag joined the entry's last line and showed as text (the book breaks only on a line holding the tag alone). **Reading text:** an entry's text and the breaks after it are one tag, and every blank line in the text holds a non-breaking space (`&nbsp;`; a line with only blood's tags counts as blank), as does the one under the date heading. The book menu finds each line by its first character, so an empty line never starts a new page: blank lines past a page's bottom stayed on it, and a page's leading blank lines vanished. With no empty line left, nothing resets the font, so one tag is enough. This is the game's own layout, with or without Ink & Quill (2026-10-05). **Marked text:** a tag per paragraph and per `\n\n` between them, the blank lines empty (Skyrim resets the font after an empty line). Ink & Quill takes a typed break's size from the text it's given. Until 2026-10-04 empty pieces were dropped, so three or more blank lines in a row, or blank lines at an entry's start, were lost on the next render. `BookEditor`'s `RunText` joins the same pieces, so an untouched entry still compares equal at a save.
- **Bookmarks** (reading text only): each entry starts with an empty Ink & Quill bookmark tag named by its date, `<a href="bookmark:Morndas, 18 Last Seed, 4E 201"></a>`, whether date headings are shown or not, so the player can pick an entry from Ink & Quill's bookmark list or step between entries with its bookmark keys (its docs/EDITOR.md#bookmarks). Only the book menu gets the tags: readers with the book as parent and the inter-plugin API get the text without them (`StripBookmarks`), so SkyrimNet's prompt and other mods see the text as before. Empty, a tag shows nothing; a book menu without Ink & Quill ignores it.
- The date header is optional (`[Diary] ShowDateHeaders`). It is preceded by an empty font tag so the title-page size does not carry across the page break.
- **Empty volume:** if there are no entries, page 2 is the localized "all entries removed" text, prefixed with the sentinel `<!-- SNPD_EMPTY -->` (`Localization::kEmptySentinel`). The inter-plugin API looks for the sentinel so it can report `NoEntries` instead of handing placeholder text to TTS mods. **A journal** (new, or its entries torn out in the editor) gets the sentinel alone: a blank page, not the notice.
- **Blood:** a journal entry's text written in blood (ranges from DiaryDB, `DiaryEntry::blood`) is marked with private-use characters before sanitizing and escaping, then rendered as `<font color='#2B0202'>…</font>` (`SanitizeBookText`; see [EDITING.md](EDITING.md#writing-in-blood)).
- Font face and sizes come from `Config` (see [CONFIG_AND_MCM.md](CONFIG_AND_MCM.md)).
- **Marked text** (`marked` = true, for Ink & Quill's editor only): the same layout with what the player can't change between U+E002 and U+E003, each entry's paragraphs one unlocked run, blood left as its markers, no empty-volume sentinel. Never sent to the book menu ([EDITING.md](EDITING.md#marked-text)).

The inter-plugin API splits the rendered text back apart on `"[pagebreak]\n\n"` and relies on this exact page layout (entries start at page index 2). Changing the layout changes that API. See [PAPYRUS_AND_API.md](PAPYRUS_AND_API.md#inter-plugin-api-skse-messaging).

## Dates

`FormatGameDate` (long: weekday, day, month, year) and `FormatGameDateShort` (title page) convert `entry_date` seconds to days and count forward from **17 Last Seed, 4E 201, Sundas** (the game start) with one helper, `ToGameDate`. Skyrim's months have the Gregorian lengths (31, 28, 31, 30, …) and there are no leap years. Names come from `Localization`. (Before 2026-09-27 every month was 30 days, so dates drifted after the first month.)

## Cleaning LLM output (`SanitizeBookText`)

Applied to each entry's content:

1. **Only when date headers are on** (SNPD adds its own, so the LLM's would be duplicates), and **not for an entry the player wrote** (tag `snpd_player_written`, see [EDITING.md](EDITING.md#text)): a date the player starts their entry with is theirs.
   - a leading `#` heading line, and a leading line that is entirely `**bold**`;
   - a leading `9:28 AM`-style time line;
   - a leading date: Tamrielic day or month names (English plus the active locale), era markers (`4E `, `4Э `, `第四紀`), or ordinal-first forms (`17th of Last Seed`). A whole first line is removed. A date that starts a sentence is cut up to the first sentence end, but only if that is within 200 characters.
   - Single-character (CJK) day names are skipped to avoid false matches.
2. **Always:** em and en dashes → `-`, curly quotes → straight, `…` → `...`, and Markdown `**`, `*`, `__` and leading `_` removed (the handwriting fonts have no bold or italic).

The book editor gets the text after this cleanup but before the escaping below (`EditableEntryText`), so it edits what the page shows.

**Last step: making the prose safe for the markup.** A literal `[pagebreak]` in an entry becomes `[page break]` (it would add a page and shift the entry numbering the inter-plugin API uses), and `&`, `<` and `>` are escaped (`&amp;` `&lt;` `&gt;`), because a `<` in prose ("<sigh>", "<3") would start a tag and swallow text. The NPC's name in the title is escaped the same way. The inter-plugin API turns the escapes back, so other mods get the prose as before.

## Delivery: the `GetDescription` and `OpenBookMenu` hooks

Everything that shows a book's text asks the form for it: `TESDescription::GetDescription(out, parent, 'DESC')`. For our volumes that would be the template's text, so `BookTextHook` (MinHook on `RELOCATION_ID(14399, 14552)`; VR reuses the SE id, see [DEVELOPMENT.md](DEVELOPMENT.md#engine-touchpoints)) answers `DESC` for them. Other fields (`CNAM`, the item card) and every other form go to the original. Mods can ask from any thread at any time, and during play a book read from the world asks from the engine's "Poll controls" job, not the main thread (found on AE, 2026-09-28: books read from the ground came up blank). So the hook reads only `BookManager`'s thread-safe snapshot: the description index (`FindBookByDescription`) and each book's text (`GetBookTextSnapshot`, updated wherever a volume's text changes), never `books_`. See [ARCHITECTURE.md](ARCHITECTURE.md#threading). Physical Letters hooks the same function the same way for its letters; each plugin answers only for its own forms and passes the rest on.

**The book menu: no parent.** All three engine callers of `BookMenu::OpenBookMenu` build the menu's text with `GetDescription(book's description component, out, nullptr, 'DESC')`, and they are the only callers that pass a book's component with no parent (checked in the SE, AE and VR binaries, 2026-09-28; AE's item card also passes no parent, but for weapon, armor, scroll and spell descriptions, never a book's own). So our book's component with no parent means the book is opening. The component is matched by identity (`FindBookByDescription`), and the answer is the snapshot text through `Utf8ToWin1251` if it has Cyrillic (below). A book with no volume (retired or unclaimed, see [BOOK_FORMS.md](BOOK_FORMS.md#retirement)) has the "all entries removed" page. The refresh comes after, in `OpenBookMenu` (below), which every open goes through; if that hook failed to install, this branch refreshes instead (`RefreshBeforeOpen`), so the engine's opens still do.

**Everyone else: the book as parent.** (Without Ink & Quill's bookmark tags: `StripBookmarks`.) SkyrimNet's book-read event, Immersive Reading on VR (it renders pages on the held book and never opens the book menu) and other book-text mods get the cached text in **UTF-8**, not the book menu's Win-1251: SkyrimNet and Immersive Reading both decode UTF-8, and SkyrimNet reads stray bytes as cp1252. No refresh, because callers can ask often: the text is as of the last render or open. The text is the rendered markup **with its `<font>` tags**, as a vanilla book's description has its own: the same text the book menu gets, only in UTF-8. SkyrimNet puts `book_text` into its prompt as-is for every book (it strips no markup); ours has more tags than most, one per paragraph and per break, so a diary costs SkyrimNet's prompt about a quarter more: kept on purpose (user, 2026-10-04), for every reader to get what a vanilla book gives. A cheaper prompt belongs in SkyrimNet, stripping markup from every book. Until 2026-10-04 SNPD stripped them for these readers (`StripFontTags`), to keep SkyrimNet's prompt short, which made a mod that opens the book menu itself with this text (Grid Inventory) show diaries in the wrong font.

This replaced Dynamic Book Framework, which refused to run on VR. **Setting a book's own description at runtime does not work**: `TESObjectBOOK`'s description is a `BGSLocalizedStringDL`, an ID into the plugin's string table, not a string buffer (and our books have no plugin).

**Mods that open the menu themselves: the `OpenBookMenu` hook.** Grid Inventory (an ImGui inventory) opens the book menu for every book itself, with text it asked `GetDescription` for **with the book as parent**: then without its font tags, so diaries showed in the wrong font (found 2026-10-04 from its log, "the engine raised no page -- showing it ourselves", and no "Opening diary" line in ours). The parent text now has its tags, but it's UTF-8 (the menu needs Win-1251 for Cyrillic) and isn't refreshed, and at that point the menu isn't opening yet, so `GetDescription` can't tell it from SkyrimNet. `OpenBookMenuHook` (MinHook on `BookMenu::OpenBookMenu`, `RELOCATION_ID(50122, 51053)`, VR reuses the SE id) sees **every** open with its book, the engine's and other mods' alike. For one of ours (a snapshot to show):

1. `RefreshBeforeOpen`: `RefreshVolumeOnOpen` (see [VOLUMES_AND_SYNC.md](VOLUMES_AND_SYNC.md#on-open-refreshvolumeonopen)), which touches `books_`, inline only on the main thread while the game is paused (an inventory or container menu), where SKSE also runs SNPD's tasks then. Anywhere else (a book read from the world, another mod's call off the main thread) it is queued as a task, so entries changed since the last render show on the next open.
2. The menu opens with the snapshot as the book menu takes it (`ForBookMenu`: Win-1251 if it has Cyrillic), whatever text the caller passed, logging "Opening diary". Anything else passes through unchanged. Physical Letters hooks it the same way for its letters.

Until 2026-10-04 the refresh ran in the no-parent `GetDescription` branch, which other mods' opens skip.

**VR's ninth argument.** VR's `OpenBookMenu` takes one SE and AE don't have: an `NiAVObject*` (on the stack at `[rsp+0x48]` on entry), the reference's 3D, which places the menu for world opens and has its refcount bumped. The hook takes and forwards it on every runtime; SE and AE callers don't pass one and never read it. Dropping it hands VR a junk pointer: crashes in `lock inc [rbx+0x08]`, invisible vanilla books, wrong placement. Until 2.0.0 the book menu's text was swapped only in this hook; it dropped the argument until 2026-09-27, was fixed, and was removed in 2.0.0 when `GetDescription` alone seemed to cover the book menu, then came back on 2026-10-04 for the mods that open it themselves.

## UTF-8 → Windows-1251

Scaleform GFx 4 paginates with a mix of byte offsets (`replaceText`) and character indices (`getLineOffset`). Two-byte UTF-8 Cyrillic makes them disagree, and the text overlaps more and more down the page. Win-1251 is single-byte Cyrillic, so bytes and characters line up again, as in vanilla Russian Skyrim.

`Utf8ToWin1251` converts Cyrillic (Russian, Ukrainian, Belarusian, Serbian and Macedonian letters), the Win-1251 punctuation `SanitizeBookText` doesn't replace (`«` `»` `„` `‚` `•` `‹` `›` `№`), and passes all other UTF-8 through unchanged. It only runs when the text **contains Cyrillic** (`HasCyrillic`); other books stay UTF-8, so French guillemets are no longer turned into invalid single bytes.

**Don't retry an AS2-side fix.** Overriding the book menu's `CreateDisplayPage` from C++ via a GFx function handler was tried and does not work: instance properties set with `SetVariable` do not shadow compiled AS2 prototype methods, even though `SetVariable` returns `ok=true`.
