/*
 * SkyrimNet Physical Diaries - a Skyrim SKSE plugin that turns SkyrimNet NPC
 * diary entries into books you can find and read in the world.
 * Copyright (C) 2026 Zevick
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#include "BookTextHook.h"
#include "BookManager.h"
#include "Localization.h"

#include <MinHook.h>
#include <thread>

// ---------------------------------------------------------------------------
// OpenBookMenu hook
//
// BookMenu::OpenBookMenu takes a BSString& a_description as its first
// argument — that is the text that will be rendered in the book UI.  We
// intercept the call, check whether the book being opened is one of our
// diary volumes (via BookManager), and if so substitute the cached diary
// text for a_description before handing off to the original function.
//
// This replaces Dynamic Book Framework's SetBookTextHook which performs the
// same job but gates itself out on VR ("Unsupported Skyrim version").
// RELOCATION_ID(50122, 51053) is (SE id, AE id); VR reuses the SE id through
// the VR Address Library, so one hook covers SE, AE and VR.
//
// VR's OpenBookMenu takes a NINTH argument that SE/AE don't have: an
// NiAVObject* (on the stack at [rsp+0x48] on entry).  When it is non-null the
// book menu is placed from that object's world transform (the world-activate
// caller passes the reference's 3D), and its refcount is bumped.  The thunk
// takes and forwards it on every runtime; SE/AE callers don't pass one and
// their OpenBookMenu never reads it, so there it is an ignored stack slot.
// Dropping it (as before 2026-09-27) handed VR a junk pointer: crashes in
// `lock inc [rbx+0x08]`, invisible vanilla books, wrong book placement.
// See docs/BOOK_TEXT.md.
//
// TESDescription::GetDescription hook
//
// Anything else that reads a book's text asks the form for its DESC field:
// SkyrimNet's book-read event, Immersive Reading on VR, the item card.  For our
// volumes that is the template's text, so this hook answers DESC for them with
// the cached diary text instead (see docs/BOOK_TEXT.md).
// ---------------------------------------------------------------------------

namespace
{
    // ── UTF-8 → Windows-1251 for Cyrillic ────────────────────────────
    // Scaleform's book pagination mixes byte and character offsets, so 2-byte
    // UTF-8 Cyrillic overlaps progressively; Win-1251 is one byte per character.
    // See docs/BOOK_TEXT.md.

    // True if the text contains Cyrillic letters (U+0400–U+04FF: UTF-8 lead bytes
    // 0xD0–0xD3).  Only such text is converted to Win-1251; converting other text
    // turned French guillemets into bytes that aren't valid UTF-8.
    static bool HasCyrillic(const std::string& text) {
        for (std::size_t i = 0; i + 1 < text.size(); ++i) {
            const auto b = static_cast<unsigned char>(text[i]);
            if (b >= 0xD0 && b <= 0xD3 && (static_cast<unsigned char>(text[i + 1]) & 0xC0) == 0x80) return true;
        }
        return false;
    }

    // Converts Cyrillic and the Win-1251 punctuation to Win-1251; anything else
    // stays UTF-8 (best effort).
    static std::string Utf8ToWin1251(const std::string& utf8) {
        std::string out;
        out.reserve(utf8.size());  // Will be smaller (2-byte → 1-byte)

        const unsigned char* p = reinterpret_cast<const unsigned char*>(utf8.c_str());
        const unsigned char* end = p + utf8.size();

        while (p < end) {
            if (*p < 0x80) {
                // ASCII pass-through (includes HTML tags, [pagebreak], \n)
                out += static_cast<char>(*p++);
            } else if ((*p & 0xE0) == 0xC0 && p + 1 < end && (*(p+1) & 0xC0) == 0x80) {
                // 2-byte UTF-8 sequence → decode codepoint
                uint32_t cp = (static_cast<uint32_t>(*p & 0x1F) << 6)
                            | static_cast<uint32_t>(*(p+1) & 0x3F);
                p += 2;

                // А-я (U+0410-U+044F) are contiguous in Win-1251 (0xC0-0xFF); the rest
                // are Ё/ё, Ukrainian, Belarusian, Serbian and Macedonian letters and « ».
                char mapped = 0;
                if (cp >= 0x0410 && cp <= 0x044F) {
                    mapped = static_cast<char>(cp - 0x0410 + 0xC0);
                } else {
                    switch (cp) {
                    case 0x0401: mapped = static_cast<char>(0xA8); break;  // Ё
                    case 0x0451: mapped = static_cast<char>(0xB8); break;  // ё
                    case 0x0404: mapped = static_cast<char>(0xAA); break;  // Є
                    case 0x0406: mapped = static_cast<char>(0xB2); break;  // І
                    case 0x0407: mapped = static_cast<char>(0xAF); break;  // Ї
                    case 0x0454: mapped = static_cast<char>(0xBA); break;  // є
                    case 0x0456: mapped = static_cast<char>(0xB3); break;  // і
                    case 0x0457: mapped = static_cast<char>(0xBF); break;  // ї
                    case 0x0490: mapped = static_cast<char>(0xA5); break;  // Ґ
                    case 0x0491: mapped = static_cast<char>(0xB4); break;  // ґ
                    case 0x040E: mapped = static_cast<char>(0xA1); break;  // Ў (Belarusian)
                    case 0x045E: mapped = static_cast<char>(0xA2); break;  // ў (Belarusian)
                    case 0x0402: mapped = static_cast<char>(0x80); break;  // Ђ (Serbian)
                    case 0x0452: mapped = static_cast<char>(0x90); break;  // ђ (Serbian)
                    case 0x0409: mapped = static_cast<char>(0x8A); break;  // Љ (Serbian, Macedonian)
                    case 0x0459: mapped = static_cast<char>(0x9A); break;  // љ (Serbian, Macedonian)
                    case 0x040A: mapped = static_cast<char>(0x8C); break;  // Њ (Serbian, Macedonian)
                    case 0x045A: mapped = static_cast<char>(0x9C); break;  // њ (Serbian, Macedonian)
                    case 0x040B: mapped = static_cast<char>(0x8D); break;  // Ћ (Serbian)
                    case 0x045B: mapped = static_cast<char>(0x9D); break;  // ћ (Serbian)
                    case 0x040F: mapped = static_cast<char>(0x8F); break;  // Џ (Serbian, Macedonian)
                    case 0x045F: mapped = static_cast<char>(0x9F); break;  // џ (Serbian, Macedonian)
                    case 0x0403: mapped = static_cast<char>(0x81); break;  // Ѓ (Macedonian)
                    case 0x0453: mapped = static_cast<char>(0x83); break;  // ѓ (Macedonian)
                    case 0x040C: mapped = static_cast<char>(0x8E); break;  // Ќ (Macedonian)
                    case 0x045C: mapped = static_cast<char>(0x9E); break;  // ќ (Macedonian)
                    case 0x0405: mapped = static_cast<char>(0xBD); break;  // Ѕ (Macedonian)
                    case 0x0455: mapped = static_cast<char>(0xBE); break;  // ѕ (Macedonian)
                    case 0x0408: mapped = static_cast<char>(0xA3); break;  // Ј (Serbian, Macedonian)
                    case 0x0458: mapped = static_cast<char>(0xBC); break;  // ј (Serbian, Macedonian)
                    case 0x00AB: mapped = static_cast<char>(0xAB); break;  // «
                    case 0x00BB: mapped = static_cast<char>(0xBB); break;  // »
                    default: break;
                    }
                }
                if (mapped) {
                    out += mapped;
                } else {
                    // Unmapped 2-byte char: pass through as UTF-8 bytes
                    out += static_cast<char>(*(p-2));
                    out += static_cast<char>(*(p-1));
                }
            } else if ((*p & 0xF0) == 0xE0 && p + 2 < end) {
                // 3-byte UTF-8: the Win-1251 punctuation SanitizeBookText doesn't
                // replace, else pass through unchanged (multi-byte text desyncs
                // pagination, so map what Win-1251 has).
                const uint32_t cp = (static_cast<uint32_t>(*p & 0x0F) << 12)
                                  | (static_cast<uint32_t>(*(p+1) & 0x3F) << 6)
                                  | static_cast<uint32_t>(*(p+2) & 0x3F);
                char mapped = 0;
                switch (cp) {
                case 0x201E: mapped = static_cast<char>(0x84); break;  // „
                case 0x201A: mapped = static_cast<char>(0x82); break;  // ‚
                case 0x2022: mapped = static_cast<char>(0x95); break;  // •
                case 0x2039: mapped = static_cast<char>(0x8B); break;  // ‹
                case 0x203A: mapped = static_cast<char>(0x9B); break;  // ›
                case 0x2116: mapped = static_cast<char>(0xB9); break;  // №
                default: break;
                }
                if (mapped) {
                    out += mapped;
                    p += 3;
                } else {
                    out += static_cast<char>(*p++);
                    out += static_cast<char>(*p++);
                    out += static_cast<char>(*p++);
                }
            } else if ((*p & 0xF8) == 0xF0 && p + 3 < end) {
                // 4-byte UTF-8: pass through unchanged
                out += static_cast<char>(*p++);
                out += static_cast<char>(*p++);
                out += static_cast<char>(*p++);
                out += static_cast<char>(*p++);
            } else {
                // Invalid sequence: skip byte
                out += '?';
                p++;
            }
        }
        return out;
    }

    // The id of the game's main thread (plugin load runs on it).  BookManager is
    // game-thread state, so the description hook only answers there.
    std::thread::id g_gameThread;

    // The text to show for `book` if it is one of our volumes ("" otherwise), in the
    // encoding the book renderer needs.  `refresh` re-reads SkyrimNet first; that
    // queries SkyrimNet, so it is only done when the book is actually opened.
    std::string DiaryTextFor(const RE::TESObjectBOOK* book, bool refresh) {
        auto* bookManager = SkyrimNetDiaries::BookManager::GetSingleton();
        auto* vol = bookManager->GetBookForFormID(book->GetFormID());
        if (!vol) return {};
        if (refresh) bookManager->RefreshVolumeOnOpen(vol);
        if (vol->cachedBookText.empty()) return {};
        if (refresh) {
            SKSE::log::info("[BookTextHook] Opening diary: formId=0x{:X} actor='{}' vol={} textLen={}",
                book->GetFormID(), vol->actorName, vol->volumeNumber, vol->cachedBookText.size());
        }
        // Win-1251 only for Cyrillic text (see Utf8ToWin1251).
        return HasCyrillic(vol->cachedBookText) ? Utf8ToWin1251(vol->cachedBookText) : vol->cachedBookText;
    }

    // The rendered text without its <font> tags.  Other readers (SkyrimNet's prompt,
    // Immersive Reading) get markup close to a vanilla book's; the book menu still
    // gets the styled text from OpenBookMenuHook.
    std::string StripFontTags(const std::string& text) {
        std::string out;
        out.reserve(text.size());
        for (std::size_t i = 0; i < text.size();) {
            if (text.compare(i, 6, "<font ") == 0 || text.compare(i, 7, "</font>") == 0) {
                const std::size_t close = text.find('>', i);
                if (close == std::string::npos) break;
                i = close + 1;
            } else {
                out += text[i++];
            }
        }
        return out;
    }

    struct OpenBookMenuHook
    {
        // BookMenu::OpenBookMenu, plus VR's ninth argument (see the file header).
        using func_t = void (*)(const RE::BSString&,
                                const RE::ExtraDataList*,
                                RE::TESObjectREFR*,
                                RE::TESObjectBOOK*,
                                const RE::NiPoint3&,
                                const RE::NiMatrix3&,
                                float,
                                bool,
                                RE::NiAVObject*);

        static inline func_t func{ nullptr };

        static void thunk(const RE::BSString&      a_desc,
                          const RE::ExtraDataList* a_extra,
                          RE::TESObjectREFR*       a_ref,
                          RE::TESObjectBOOK*       a_book,
                          const RE::NiPoint3&      a_pos,
                          const RE::NiMatrix3&     a_rot,
                          float                    a_scale,
                          bool                     a_useDefaultPos,
                          RE::NiAVObject*          a_vrNode)
        {
            SKSE::log::debug("[BookTextHook] thunk entry: book=0x{:X} ref=0x{:X} useDefaultPos={}",
                a_book ? a_book->GetFormID() : 0, a_ref ? a_ref->GetFormID() : 0, a_useDefaultPos);

            if (a_book) {
                // Prepare the diary text under try/catch: this runs inside the engine's
                // call stack, where an escaping exception is a crash.  On failure the
                // book opens with its own text instead.
                std::string textToInject;
                try {
                    // Refresh first: picks up entries added or deleted since the last render.
                    textToInject = DiaryTextFor(a_book, true);
                } catch (const std::exception& e) {
                    SKSE::log::error("[BookTextHook] Preparing diary text for 0x{:X} failed: {} — opening the book without it",
                                     a_book->GetFormID(), e.what());
                    textToInject.clear();
                } catch (...) {
                    SKSE::log::error("[BookTextHook] Preparing diary text for 0x{:X} failed — opening the book without it",
                                     a_book->GetFormID());
                    textToInject.clear();
                }

                if (!textToInject.empty()) {
                    RE::BSString injectedText{ textToInject.c_str() };
                    func(injectedText, a_extra, a_ref, a_book, a_pos, a_rot, a_scale, a_useDefaultPos, a_vrNode);
                    return;
                }
            }

            func(a_desc, a_extra, a_ref, a_book, a_pos, a_rot, a_scale, a_useDefaultPos, a_vrNode);
        }

        static void Install()
        {
            // SE id 50122 (also used by VR) | AE id 51053.  MinHook, like the other
            // hooks: it relocates the prologue itself, and copes with other plugins
            // hooking the same function.
            REL::Relocation<std::uintptr_t> target{ RELOCATION_ID(50122, 51053) };
            auto* targetPtr = reinterpret_cast<void*>(target.address());

            const auto init = MH_Initialize();
            if (init != MH_OK && init != MH_ERROR_ALREADY_INITIALIZED) {
                SKSE::log::error("[BookTextHook] MH_Initialize failed ({}) — diaries will show template text",
                                 MH_StatusToString(init));
                return;
            }
            auto status = MH_CreateHook(targetPtr, reinterpret_cast<void*>(&thunk),
                                        reinterpret_cast<void**>(&func));
            if (status == MH_OK) {
                status = MH_EnableHook(targetPtr);
            }
            if (status != MH_OK) {
                SKSE::log::error("[BookTextHook] OpenBookMenu hook failed ({}) — diaries will show template text",
                                 MH_StatusToString(status));
                return;
            }
            SKSE::log::info("Installed OpenBookMenu hook (RELOCATION_ID 50122/51053)");
        }
    };

    struct GetDescriptionHook
    {
        using func_t = void (*)(RE::TESDescription*, RE::BSString&, RE::TESForm*, std::uint32_t);
        static inline func_t original{ nullptr };

        static void thunk(RE::TESDescription* a_self, RE::BSString& a_out, RE::TESForm* a_parent,
                          std::uint32_t a_fieldType)
        {
            // Only DESC (the book's text; CNAM is the item card) of our volumes, and only
            // on the game thread.  Cheap checks first: this runs for every description.
            if (a_fieldType == 'CSED' && a_parent && a_parent->GetFormType() == RE::FormType::Book &&
                std::this_thread::get_id() == g_gameThread) {
                try {
                    // No refresh: callers can ask often, and the text was refreshed
                    // when the book was last opened.
                    const auto text = DiaryTextFor(a_parent->As<RE::TESObjectBOOK>(), false);
                    if (!text.empty()) {
                        a_out = StripFontTags(text).c_str();
                        return;
                    }
                } catch (...) {
                    // Fall through to the form's own text.
                }
            }
            original(a_self, a_out, a_parent, a_fieldType);
        }

        static void Install()
        {
            // SE id 14399 (also used by VR) | AE id 14552.  MinHook: other plugins
            // (e.g. Description Framework) hook this function too.
            REL::Relocation<std::uintptr_t> target{ RELOCATION_ID(14399, 14552) };
            auto* targetPtr = reinterpret_cast<void*>(target.address());

            const auto init = MH_Initialize();
            if (init != MH_OK && init != MH_ERROR_ALREADY_INITIALIZED) {
                SKSE::log::error("[BookTextHook] MH_Initialize failed ({}) — other mods will see template text",
                                 MH_StatusToString(init));
                return;
            }
            auto status = MH_CreateHook(targetPtr, reinterpret_cast<void*>(&thunk),
                                        reinterpret_cast<void**>(&original));
            if (status == MH_OK) {
                status = MH_EnableHook(targetPtr);
            }
            if (status != MH_OK) {
                SKSE::log::error("[BookTextHook] GetDescription hook failed ({}) — other mods will see template text",
                                 MH_StatusToString(status));
                return;
            }
            SKSE::log::info("Installed GetDescription hook (RELOCATION_ID 14399/14552)");
        }
    };

} // anonymous namespace

void SkyrimNetDiaries::BookTextHook::Install()
{
    SKSE::log::info("[BookTextHook] Game language: '{}'",
                    SkyrimNetDiaries::Localization::GetSingleton()->GetLanguageString());

    g_gameThread = std::this_thread::get_id();  // SKSEPlugin_Load runs on the main thread
    OpenBookMenuHook::Install();
    GetDescriptionHook::Install();
}
