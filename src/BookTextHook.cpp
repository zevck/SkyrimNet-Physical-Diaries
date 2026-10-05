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

#include "Detour.h"
#include <thread>

// TESDescription::GetDescription hook: our books' text (no parent: the book menu's; with the book as parent, UTF-8).
// OpenBookMenu: every open, whoever makes it, refreshed with the book menu's text.  docs/BOOK_TEXT.md (Delivery)

namespace
{
    // ── UTF-8 → Windows-1251 for Cyrillic ────────────────────────────
    // Scaleform's book pagination mixes byte and character offsets.  See docs/BOOK_TEXT.md#utf-8--windows-1251.

    // Cyrillic lead bytes 0xD0–0xD3.  Only such text is converted: converting other text
    // turned French guillemets into invalid UTF-8.
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
                // 3-byte UTF-8: map the Win-1251 punctuation SanitizeBookText leaves (multi-byte text
                // desyncs pagination), else pass through.
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

    // The thread that loaded the plugin: the main thread.
    std::thread::id g_mainThread;

    // OpenBookMenuHook is in: it refreshes every open.  Without it, the no-parent GetDescription branch does.
    bool g_openHooked = false;

    // books_ is touched only where SKSE runs our tasks: inline on the main thread while paused (menus),
    // else queued (shows on the next open).  See docs/ARCHITECTURE.md#threading.
    void RefreshBeforeOpen(RE::FormID bookId)
    {
        const auto refresh = [bookId]() {
            auto* books = SkyrimNetDiaries::BookManager::GetSingleton();
            if (auto* vol = books->GetBookForFormID(bookId)) books->RefreshVolumeOnOpen(vol);
        };
        auto* ui = RE::UI::GetSingleton();
        if (std::this_thread::get_id() == g_mainThread && ui && ui->GameIsPaused()) {
            refresh();
        } else {
            SKSE::GetTaskInterface()->AddTask(refresh);
        }
    }

    struct GetDescriptionHook
    {
        using func_t = void (*)(RE::TESDescription*, RE::BSString&, RE::TESForm*, std::uint32_t);
        static inline func_t original{ nullptr };

        static void thunk(RE::TESDescription* a_self, RE::BSString& a_out, RE::TESForm* a_parent,
                          std::uint32_t a_fieldType)
        {
            // Only DESC (the book's text; CNAM is the item card).  Cheap checks first:
            // this runs for every description.
            if (a_fieldType == 'CSED') {
                try {
                    auto* books = SkyrimNetDiaries::BookManager::GetSingleton();
                    if (!a_parent) {
                        // The book menu is opening one of our books (see the file header); OpenBookMenu refreshes.
                        if (const auto bookId = books->FindBookByDescription(a_self)) {
                            if (!g_openHooked) RefreshBeforeOpen(bookId);
                            if (const auto text = books->GetBookTextSnapshot(bookId); !text.empty()) {
                                a_out = SkyrimNetDiaries::BookTextHook::ForBookMenu(text).c_str();
                                return;
                            }
                        }
                    } else if (a_parent->GetFormType() == RE::FormType::Book) {
                        // Another reader, any thread: the snapshot, no refresh, UTF-8, tags and all as a
                        // vanilla book's (docs/BOOK_TEXT.md#delivery-the-getdescription-and-openbookmenu-hooks).
                        if (const auto text = books->GetBookTextSnapshot(a_parent->GetFormID()); !text.empty()) {
                            a_out = text.c_str();
                            return;
                        }
                    }
                } catch (const std::exception& e) {
                    SKSE::log::error("[BookTextHook] Preparing diary text failed: {} — showing the book's own text", e.what());
                } catch (...) {
                    SKSE::log::error("[BookTextHook] Preparing diary text failed — showing the book's own text");
                }
            }
            original(a_self, a_out, a_parent, a_fieldType);
        }

        static void Install()
        {
            // SE id 14399 (also used by VR) | AE id 14552.  Other plugins (e.g.
            // Description Framework) hook this function too.
            REL::Relocation<std::uintptr_t> target{ RELOCATION_ID(14399, 14552) };
            SkyrimNetDiaries::InstallDetour(target.address(), reinterpret_cast<void*>(&thunk), reinterpret_cast<void**>(&original),
                          "GetDescription", "14399/14552", "diaries will show template text");
        }
    };

    // BookMenu::OpenBookMenu, every open whoever makes it (Grid Inventory opens books itself, with the parent text):
    // our books are refreshed and get the book menu's text here.
    struct OpenBookMenuHook
    {
        // Plus VR's ninth argument, the reference's 3D: forwarded on every runtime (docs/BOOK_TEXT.md).
        using func_t = void (*)(const RE::BSString&, const RE::ExtraDataList*, RE::TESObjectREFR*, RE::TESObjectBOOK*,
                                const RE::NiPoint3&, const RE::NiMatrix3&, float, bool, RE::NiAVObject*);
        static inline func_t original{ nullptr };

        static void thunk(const RE::BSString& a_description, const RE::ExtraDataList* a_extraList, RE::TESObjectREFR* a_ref,
                          RE::TESObjectBOOK* a_book, const RE::NiPoint3& a_pos, const RE::NiMatrix3& a_rot, float a_scale,
                          bool a_useDefaultPos, RE::NiAVObject* a_vrNode)
        {
            std::string styled;
            try {
                auto* books = SkyrimNetDiaries::BookManager::GetSingleton();
                const RE::FormID bookId = a_book ? a_book->GetFormID() : 0;
                if (bookId && !books->GetBookTextSnapshot(bookId).empty()) {
                    RefreshBeforeOpen(bookId);
                    // Read again: an inline refresh (paused, main thread) has just changed it.  Win-1251 for Cyrillic:
                    // Scaleform's pagination needs one byte per character.
                    styled = SkyrimNetDiaries::BookTextHook::ForBookMenu(books->GetBookTextSnapshot(bookId));
                    SKSE::log::info("[BookTextHook] Opening diary 0x{:X} (textLen={})", bookId, styled.size());
                }
            } catch (const std::exception& e) {
                SKSE::log::error("[BookTextHook] Preparing diary text failed: {} — showing the text given", e.what());
                styled.clear();
            } catch (...) {
                SKSE::log::error("[BookTextHook] Preparing diary text failed — showing the text given");
                styled.clear();
            }
            if (!styled.empty()) {
                const RE::BSString text{ styled.c_str() };
                original(text, a_extraList, a_ref, a_book, a_pos, a_rot, a_scale, a_useDefaultPos, a_vrNode);
                return;
            }
            original(a_description, a_extraList, a_ref, a_book, a_pos, a_rot, a_scale, a_useDefaultPos, a_vrNode);
        }

        static void Install()
        {
            // SE id 50122 (also used by VR) | AE id 51053; Physical Letters hooks it too.
            REL::Relocation<std::uintptr_t> target{ RELOCATION_ID(50122, 51053) };
            g_openHooked = SkyrimNetDiaries::InstallDetour(target.address(), reinterpret_cast<void*>(&thunk),
                reinterpret_cast<void**>(&original), "OpenBookMenu", "50122/51053",
                "books opened by other mods show the text they were given (no Win-1251 for Cyrillic, no refresh)");
        }
    };

} // anonymous namespace

std::string SkyrimNetDiaries::BookTextHook::ForBookMenu(const std::string& text)
{
    return HasCyrillic(text) ? Utf8ToWin1251(text) : text;
}

void SkyrimNetDiaries::BookTextHook::Install()
{
    SKSE::log::info("[BookTextHook] Game language: '{}'",
                    SkyrimNetDiaries::Localization::GetSingleton()->GetLanguageString());

    g_mainThread = std::this_thread::get_id();  // SKSEPlugin_Load runs on the main thread
    GetDescriptionHook::Install();
    OpenBookMenuHook::Install();
}
