# Book Text

How a volume's entries become the text the player reads, and how that text reaches the book menu.

Code: `src/BookText.cpp` (`FormatDiaryEntries`, `SanitizeBookText`, `FormatGameDate`, `FormatGameDateShort`), `src/BookTextHook.cpp`, `src/Localization.cpp`.

---

## Rendering (`FormatDiaryEntries`)

The rendered text is Skyrim book markup: HTML-like `<font>` / `<p align>` tags and `[pagebreak]`. Layout:

| Page | Content |
|---|---|
| 0 | Blank |
| 1 | Title (`Localization::FormatTitle`: `DiaryTitle`, or `JournalTitle` for the player's journal) centred at `TitleSize`, then the date range at `SmallSize` |
| 2 … | One entry per page: an optional date header (`DateSize`), then the entry at `ContentSize` |

- **Every paragraph gets its own `<font face size>` tag.** Skyrim resets the font after `\n\n`, so one outer tag does not carry through.
- The date header is optional (`[Diary] ShowDateHeaders`). It is preceded by an empty font tag so the title-page size does not carry across the page break.
- **Empty volume:** if there are no entries, page 2 is the localized "all entries removed" text, prefixed with the sentinel `<!-- SNPD_EMPTY -->` (`Localization::kEmptySentinel`). The inter-plugin API looks for the sentinel so it can report `NoEntries` instead of handing placeholder text to TTS mods. **A journal** (new, or its entries torn out in the editor) gets the sentinel alone: a blank page, not the notice.
- Font face and sizes come from `Config` (see [CONFIG_AND_MCM.md](CONFIG_AND_MCM.md)).

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

## Delivery: the `GetDescription` hook

Everything that shows a book's text asks the form for it: `TESDescription::GetDescription(out, parent, 'DESC')`. For our volumes that would be the template's text, so `BookTextHook` (MinHook on `RELOCATION_ID(14399, 14552)`; VR reuses the SE id, see [DEVELOPMENT.md](DEVELOPMENT.md#engine-touchpoints)) answers `DESC` for them. Other fields (`CNAM`, the item card) and every other form go to the original. Mods can ask from any thread at any time, and during play a book read from the world asks from the engine's "Poll controls" job, not the main thread (found on AE, 2026-09-28: books read from the ground came up blank). So the hook reads only `BookManager`'s thread-safe snapshot: the description index (`FindBookByDescription`) and each book's text (`GetBookTextSnapshot`, updated wherever a volume's text changes), never `books_`. See [ARCHITECTURE.md](ARCHITECTURE.md#threading). Physical Letters hooks the same function the same way for its letters; each plugin answers only for its own forms and passes the rest on.

**The book menu: no parent.** All three engine callers of `BookMenu::OpenBookMenu` build the menu's text with `GetDescription(book's description component, out, nullptr, 'DESC')`, and they are the only callers that pass a book's component with no parent (checked in the SE, AE and VR binaries, 2026-09-28; AE's item card also passes no parent, but for weapon, armor, scroll and spell descriptions, never a book's own). So our book's component with no parent means the book is opening. The component is matched by identity (`FindBookByDescription`), then:

1. `RefreshVolumeOnOpen` (see [VOLUMES_AND_SYNC.md](VOLUMES_AND_SYNC.md#on-open-refreshvolumeonopen)), which touches `books_`: inline only on the main thread while the game is paused (an inventory or container menu), where SKSE also runs SNPD's tasks then. Anywhere else (a book read from the world) it is queued as a task, so entries changed since the last render show on the next open.
2. The snapshot text, styled, through `Utf8ToWin1251` if it has Cyrillic (below). A book with no volume (retired or unclaimed, see [BOOK_FORMS.md](BOOK_FORMS.md#retirement)) has the "all entries removed" page.

**Everyone else: the book as parent.** SkyrimNet's book-read event, Immersive Reading on VR (it renders pages on the held book and never opens the book menu) and other book-text mods get the cached text in **UTF-8**, not the book menu's Win-1251: SkyrimNet and Immersive Reading both decode UTF-8, and SkyrimNet reads stray bytes as cp1252. No refresh, because callers can ask often: the text is as of the last render or open. The text is the rendered markup **without its `<font>` tags** (`StripFontTags`): `[pagebreak]`, `<p align>` and escaped `&` `<` `>` stay, which is close to a vanilla book's description. SkyrimNet puts `book_text` into its prompt as-is, and the font tags were about a quarter of it. Immersive Reading draws diaries in its default font, like every other book.

This replaced Dynamic Book Framework, which refused to run on VR. **Setting a book's own description at runtime does not work**: `TESObjectBOOK`'s description is a `BGSLocalizedStringDL`, an ID into the plugin's string table, not a string buffer (and our books have no plugin).

**History: the `OpenBookMenu` hook (until 2.0.0).** The book menu's text used to be swapped in a hook on `OpenBookMenu` itself. VR's `OpenBookMenu` takes a ninth argument SE and AE don't have (an `NiAVObject*`, the reference's 3D for world opens), and until 2026-09-27 the hook dropped it, so VR read a junk pointer: crashes in `lock inc [rbx+0x08]`, invisible vanilla books, and the "static `BSString`" crash. The hook was fixed to forward nine arguments and then removed in 2.0.0, since `GetDescription` alone covers the book menu.

## UTF-8 → Windows-1251

Scaleform GFx 4 paginates with a mix of byte offsets (`replaceText`) and character indices (`getLineOffset`). Two-byte UTF-8 Cyrillic makes them disagree, and the text overlaps more and more down the page. Win-1251 is single-byte Cyrillic, so bytes and characters line up again, as in vanilla Russian Skyrim.

`Utf8ToWin1251` converts Cyrillic (Russian, Ukrainian, Belarusian, Serbian and Macedonian letters), the Win-1251 punctuation `SanitizeBookText` doesn't replace (`«` `»` `„` `‚` `•` `‹` `›` `№`), and passes all other UTF-8 through unchanged. It only runs when the text **contains Cyrillic** (`HasCyrillic`); other books stay UTF-8, so French guillemets are no longer turned into invalid single bytes.

**Don't retry an AS2-side fix.** Overriding the book menu's `CreateDisplayPage` from C++ via a GFx function handler was tried and does not work: instance properties set with `SetVariable` do not shadow compiled AS2 prototype methods, even though `SetVariable` returns `ok=true`.
