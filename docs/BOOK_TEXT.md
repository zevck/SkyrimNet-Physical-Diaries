# Book Text

How a volume's entries become the text the player reads, and how that text reaches the book menu.

Code: `src/BookText.cpp` (`FormatDiaryEntries`, `SanitizeBookText`, `FormatGameDate`, `FormatGameDateShort`), `src/BookTextHook.cpp`, `src/Localization.cpp`.

---

## Rendering (`FormatDiaryEntries`)

The rendered text is Skyrim book markup: HTML-like `<font>` / `<p align>` tags and `[pagebreak]`. Layout:

| Page | Content |
|---|---|
| 0 | Blank |
| 1 | Title (`Localization::FormatDiaryTitle`) centred at `TitleSize`, then the date range at `SmallSize` |
| 2 … | One entry per page: an optional date header (`DateSize`), then the entry at `ContentSize` |

- **Every paragraph gets its own `<font face size>` tag.** Skyrim resets the font after `\n\n`, so one outer tag does not carry through.
- The date header is optional (`[Diary] ShowDateHeaders`). It is preceded by an empty font tag so the title-page size does not carry across the page break.
- **Empty volume:** if there are no entries, page 2 is the localized "all entries removed" text, prefixed with the sentinel `<!-- SNPD_EMPTY -->` (`Localization::kEmptySentinel`). The inter-plugin API looks for the sentinel so it can report `NoEntries` instead of handing placeholder text to TTS mods.
- Font face and sizes come from `Config` (see [CONFIG_AND_MCM.md](CONFIG_AND_MCM.md)).

The inter-plugin API splits the rendered text back apart on `"[pagebreak]\n\n"` and relies on this exact page layout (entries start at page index 2). Changing the layout changes that API. See [PAPYRUS_AND_API.md](PAPYRUS_AND_API.md#inter-plugin-api-skse-messaging).

## Dates

`FormatGameDate` (long: weekday, day, month, year) and `FormatGameDateShort` (title page) convert `entry_date` seconds to days and count forward from **17 Last Seed, 4E 201, Sundas** (the game start) with one helper, `ToGameDate`. Skyrim's months have the Gregorian lengths (31, 28, 31, 30, …) and there are no leap years. Names come from `Localization`. (Before 2026-09-27 every month was 30 days, so dates drifted after the first month.)

## Cleaning LLM output (`SanitizeBookText`)

Applied to each entry's content:

1. **Only when date headers are on** (SNPD adds its own, so the LLM's would be duplicates):
   - a leading `#` heading line, and a leading line that is entirely `**bold**`;
   - a leading `9:28 AM`-style time line;
   - a leading date: Tamrielic day or month names (English plus the active locale), era markers (`4E `, `4Э `, `第四紀`), or ordinal-first forms (`17th of Last Seed`). A whole first line is removed. A date that starts a sentence is cut up to the first sentence end, but only if that is within 200 characters.
   - Single-character (CJK) day names are skipped to avoid false matches.
2. **Always:** em and en dashes → `-`, curly quotes → straight, `…` → `...`, and Markdown `**`, `*`, `__` and leading `_` removed (the handwriting fonts have no bold or italic).

**Last step: making the prose safe for the markup.** A literal `[pagebreak]` in an entry becomes `[page break]` (it would add a page and shift the entry numbering the inter-plugin API uses), and `&`, `<` and `>` are escaped (`&amp;` `&lt;` `&gt;`), because a `<` in prose ("<sigh>", "<3") would start a tag and swallow text. The NPC's name in the title is escaped the same way. The inter-plugin API turns the escapes back, so other mods get the prose as before.

## Delivery: the `OpenBookMenu` hook

`BookTextHook::Install()` patches the entry of `BookMenu::OpenBookMenu` (`RELOCATION_ID(50122, 51053)`, see [DEVELOPMENT.md](DEVELOPMENT.md#engine-touchpoints)). The first argument is the text the menu will render. For our books the thunk swaps it out:

1. `GetBookForFormID(a_book)`. Not ours → call the original with every argument unchanged.
2. `RefreshVolumeOnOpen` (see [VOLUMES_AND_SYNC.md](VOLUMES_AND_SYNC.md#on-open-refreshvolumeonopen)).
3. `Utf8ToWin1251(cachedBookText)` if the text has Cyrillic (below), put into a stack-local `RE::BSString`, and call the original with it in place of the description.

**VR's `OpenBookMenu` has a ninth argument**, an `NiAVObject*` that SE and AE don't have (found in the unpacked VR 1.4.15 binary: all three callers store it at `[rsp+0x40]`; the world-activate caller passes the reference's 3D). When it is non-null the menu takes the book's placement from that object's world transform and bumps its refcount (`lock inc [rbx+0x08]`). The thunk takes and forwards nine arguments on every runtime; on SE/AE the ninth is an unused stack slot that the original never reads.

Until 2026-09-27 the hook declared CommonLib's eight-argument signature, so on VR the original read a junk pointer from that stack slot. That one bug explains every VR report: a crash in `lock inc [rbx+0x08]` (junk that isn't a pointer), vanilla books opening invisible (junk that is a readable pointer, used as the placement transform), and the "static `BSString`" crash (`RBX = 0x43534544`, also junk). The workarounds built on the wrong diagnosis were removed: passing a null reference and forcing `useDefaultPos` for VR world-opens (which left the world book visible in front of the player and laid menu books flat), and an SEH guard that swallowed access violations halfway through the engine function, leaving its book-menu state half-written.

### Other readers: the `GetDescription` hook

Everything else that reads a book's text asks the form: `TESDescription::GetDescription(out, book, 'DESC')`. SkyrimNet's book-read event does this, as do Immersive Reading on VR (it renders pages on the held book and never opens the book menu) and any other book-text mod. For our volumes that returns the template's text, so a second hook (`GetDescriptionHook`, MinHook on `RELOCATION_ID(14399, 14552)`; VR reuses the SE id) answers `DESC` for our volumes with the cached diary text in **UTF-8**, not the book menu's Win-1251: SkyrimNet and Immersive Reading both decode UTF-8, and SkyrimNet reads stray bytes as cp1252, so Win-1251 Cyrillic reached its prompt as mojibake. Other fields (`CNAM`, the item card) and every other form go to the original. It only answers on the game thread (BookManager is game-thread state; the thread id is recorded at plugin load), and it doesn't refresh from SkyrimNet, because callers can ask often: the text is as of the last render or open. The text is the rendered markup **without its `<font>` tags** (`StripFontTags`): `[pagebreak]`, `<p align>` and escaped `&` `<` `>` stay, which is close to a vanilla book's description. SkyrimNet puts `book_text` into its prompt as-is, and the font tags were about a quarter of it. The book menu still shows the styled text, because `OpenBookMenuHook` replaces the description after the caller has read it. Immersive Reading draws diaries in its default font, like every other book.

This hook replaced Dynamic Book Framework, which refused to run on VR. **Setting a book's own description at runtime does not work**: `TESObjectBOOK`'s description is a `BGSLocalizedStringDL`, an ID into the plugin's string table, not a string buffer.

## UTF-8 → Windows-1251

Scaleform GFx 4 paginates with a mix of byte offsets (`replaceText`) and character indices (`getLineOffset`). Two-byte UTF-8 Cyrillic makes them disagree, and the text overlaps more and more down the page. Win-1251 is single-byte Cyrillic, so bytes and characters line up again, as in vanilla Russian Skyrim.

`Utf8ToWin1251` converts Cyrillic (Russian, Ukrainian, Belarusian, Serbian and Macedonian letters), the Win-1251 punctuation `SanitizeBookText` doesn't replace (`«` `»` `„` `‚` `•` `‹` `›` `№`), and passes all other UTF-8 through unchanged. It only runs when the text **contains Cyrillic** (`HasCyrillic`); other books stay UTF-8, so French guillemets are no longer turned into invalid single bytes.

**Don't retry an AS2-side fix.** Overriding the book menu's `CreateDisplayPage` from C++ via a GFx function handler was tried and does not work: instance properties set with `SetVariable` do not shadow compiled AS2 prototype methods, even though `SetVariable` returns `ok=true`.
