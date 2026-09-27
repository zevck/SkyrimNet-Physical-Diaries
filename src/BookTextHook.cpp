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

#include <cstring>

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
// the VR Address Library, so one hook covers SE, AE and VR with no version check.
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

    struct OpenBookMenuHook
    {
        // Matches the static member signature of BookMenu::OpenBookMenu
        using func_t = void (*)(const RE::BSString&,
                                const RE::ExtraDataList*,
                                RE::TESObjectREFR*,
                                RE::TESObjectBOOK*,
                                const RE::NiPoint3&,
                                const RE::NiMatrix3&,
                                float,
                                bool);

        static inline func_t func{ nullptr };

        // SEH-guarded call to the original OpenBookMenu.
        //
        // In VR, books can be opened by physically grabbing them (HIGGS).  Some
        // world references — particularly physics-enabled placed books — can have
        // a dead/recycled handle by the time their activation reaches OpenBookMenu.
        // The engine doesn't null-check and faults on `lock inc [rbx+0x08]` with
        // rbx = 0xFFFFFFFF (the invalid-handle sentinel), producing a hard CTD.
        //
        // We can't reliably detect that dead-handle state in advance (GetHandle()
        // just wraps the live pointer), so we guard the call: on a normal open no
        // exception occurs and this is fully transparent; on the access violation
        // we swallow it and the book simply fails to open instead of crashing the
        // game.  The faulting instruction is a write that never landed, so no
        // memory was corrupted — bailing out is clean.
        //
        // MUST contain no C++ objects requiring unwinding (SEH constraint), so it
        // only forwards already-constructed arguments.  Returns true if the call
        // completed, false if an access violation was caught.
        static bool CallOriginalGuarded(const RE::BSString&     a_desc,
                                        const RE::ExtraDataList* a_extra,
                                        RE::TESObjectREFR*       a_ref,
                                        RE::TESObjectBOOK*       a_book,
                                        const RE::NiPoint3&      a_pos,
                                        const RE::NiMatrix3&     a_rot,
                                        float                    a_scale,
                                        bool                     a_useDefaultPos)
        {
            __try {
                func(a_desc, a_extra, a_ref, a_book, a_pos, a_rot, a_scale, a_useDefaultPos);
                return true;
            } __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION
                            ? EXCEPTION_EXECUTE_HANDLER
                            : EXCEPTION_CONTINUE_SEARCH) {
                return false;
            }
        }

        static void thunk(const RE::BSString&    a_desc,
                          const RE::ExtraDataList* a_extra,
                          RE::TESObjectREFR*    a_ref,
                          RE::TESObjectBOOK*   a_book,
                          const RE::NiPoint3&  a_pos,
                          const RE::NiMatrix3& a_rot,
                          float                a_scale,
                          bool                 a_useDefaultPos)
        {
            // Diagnostic: capture the full parameter state at entry.  This line is
            // flushed before control enters the engine's OpenBookMenu, so if the
            // engine then faults (the VR/HIGGS dead-handle CTD), the crash log's
            // preceding lines show exactly which book/reference triggered it and
            // whether a_ref was present.  Helps root-cause setup-specific reports.
            SKSE::log::debug("[BookTextHook] thunk entry: book=0x{:X} ref={} refFormId=0x{:X} useDefaultPos={}",
                a_book ? a_book->GetFormID() : 0,
                a_ref ? "yes" : "null",
                a_ref ? a_ref->GetFormID() : 0,
                a_useDefaultPos);

            // VR world-open crash workaround.
            //
            // Opening a book from the WORLD (activate / HIGGS hand-grab) passes the
            // book's world reference as a_ref.  On VR the engine's OpenBookMenu
            // faults inside its reference-handle refcount (`lock inc [rbx+0x08]`,
            // rbx = 0xFFFFFFFF) for these world references — a hard CTD on every
            // world book.  Opening from the INVENTORY passes a_ref = null and does
            // not hit that path (this is the only path that was tested pre-release).
            //
            // Routing VR world-opens through the null-ref path makes them behave
            // like inventory opens: the book still opens and diary text injection
            // is unaffected (it never depended on a_ref), and our diaries are
            // kCantTake so they lose nothing.  The only cost is that vanilla books
            // opened from the world lose their "take" association on VR — an
            // acceptable trade against a guaranteed crash.  SSE is untouched.
            //
            // Also force useDefaultPos: without the ref, VR places the 3D book off-screen.
            // Null ref + default position is the working inventory-open path
            // (see docs/BOOK_TEXT.md).
            RE::TESObjectREFR* safeRef = a_ref;
            bool safeUseDefaultPos = a_useDefaultPos;
            if (a_ref && REL::Module::IsVR()) {
                safeRef = nullptr;
                safeUseDefaultPos = true;
            }

            if (a_book) {
                // Prepare the diary text under try/catch: this runs inside the engine's
                // call stack, where an escaping exception is a crash.  On failure the
                // book opens with its own text instead.
                std::string textToInject;
                try {
                    auto* bookManager = SkyrimNetDiaries::BookManager::GetSingleton();
                    if (auto* vol = bookManager->GetBookForFormID(a_book->GetFormID())) {
                        // Refresh text before injection: detects new/deleted entries and
                        // reformats if the live count differs from the cached count.
                        bookManager->RefreshVolumeOnOpen(vol);
                        if (!vol->cachedBookText.empty()) {
                            SKSE::log::info("[BookTextHook] Opening diary: formId=0x{:X} actor='{}' vol={} textLen={}",
                                a_book->GetFormID(), vol->actorName, vol->volumeNumber, vol->cachedBookText.size());

                            // Win-1251 only for Cyrillic text (see Utf8ToWin1251).
                            textToInject = HasCyrillic(vol->cachedBookText)
                                               ? Utf8ToWin1251(vol->cachedBookText)
                                               : vol->cachedBookText;
                        }
                    }
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
                    // Stack-local BSString — must NOT be static.  A shared static
                    // BSString was responsible for an EXCEPTION_ACCESS_VIOLATION in
                    // BookMenu::OpenBookMenu on at least one VR user (RBX = 0x43534544
                    // "DESC", consistent with reading uninitialized memory through a
                    // dangling reference): nested book opens or other mods with hooks
                    // on the same code path could re-enter thunk and mutate the static
                    // BSString's internal buffer mid-call, leaving the engine holding
                    // a stale pointer.  A stack-local instance per call eliminates the
                    // cross-call aliasing entirely.
                    RE::BSString injectedText{ textToInject.c_str() };

                    if (!CallOriginalGuarded(injectedText, a_extra, safeRef, a_book,
                                             a_pos, a_rot, a_scale, safeUseDefaultPos)) {
                        SKSE::log::warn("[BookTextHook] OpenBookMenu faulted (caught) for diary "
                                        "formId=0x{:X} — book not opened, game continues",
                                        a_book->GetFormID());
                    }
                    return;
                }
            }

            if (!CallOriginalGuarded(a_desc, a_extra, safeRef, a_book,
                                     a_pos, a_rot, a_scale, safeUseDefaultPos)) {
                SKSE::log::warn("[BookTextHook] OpenBookMenu faulted (caught) — book not opened, "
                                "game continues (likely a dead reference handle from a VR/HIGGS grab)");
            }
        }

        static void Install()
        {
            // SE id 50122 (also used by VR) | AE id 51053
            REL::Relocation<std::uintptr_t> target{ RELOCATION_ID(50122, 51053) };
            auto& trampoline = SKSE::GetTrampoline();

            // Reserve enough for all cases + write_branch<5> relay stub: SKSE's branch
            // pool if available, otherwise our own block near the game module
            // (CommonLib v9's SKSE::AllocTrampoline silently allocates nothing
            // without a TrampolineInterface).
            {
                constexpr std::size_t trampolineSize = 20 + 14 + 8;
                void* mem = nullptr;
                if (const auto* intfc = SKSE::GetTrampolineInterface()) {
                    mem = intfc->AllocateFromBranchPool(trampolineSize);
                }
                if (mem) {
                    trampoline.set_trampoline(mem, trampolineSize);
                } else {
                    trampoline.create(trampolineSize);
                }
            }

            const auto targetAddr = target.address();
            const auto* p = reinterpret_cast<const std::uint8_t*>(targetAddr);

            // Always log the first 8 bytes so we can diagnose prologue issues.
            SKSE::log::info(
                "OpenBookMenu hook target {:016X} prologue: "
                "{:02X} {:02X} {:02X} {:02X} {:02X} {:02X} {:02X} {:02X}",
                targetAddr, p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7]);

            // ---- Build the "call original" stub ----
            //
            // The goal: a stub that, when called, behaves like the unpatched
            // original function (executes whatever 5 bytes we're about to
            // overwrite, then continues from targetAddr+5).
            //
            // The tricky part is that several 5-byte encodings use RIP-relative
            // addressing whose displacement would be WRONG once the bytes are
            // copied to a different address in the trampoline pool.  We must
            // detect and handle those cases explicitly.
            //
            // Known problematic encodings:
            //   EB rel8       — short JMP:   follow it to the real body
            //   E9 rel32      — long JMP:    follow it to the real body
            //   FF 25 rel32   — JMP [RIP+x]: dereference the pointer table entry
            //
            // For all other encodings (regular push/mov prologues) the bytes are
            // position-independent and safe to copy verbatim.

            std::uint8_t* origStub = nullptr;
            std::uintptr_t realBody = 0;

            if (p[0] == 0xEB) {
                // EB rel8 — 2-byte short JMP, e.g. "EB 16"
                realBody = targetAddr + 2 + static_cast<std::int8_t>(p[1]);
                SKSE::log::info("  -> EB short-JMP; realBody = {:016X}", realBody);

                origStub = static_cast<std::uint8_t*>(trampoline.allocate(14));
                origStub[0] = 0xFF; origStub[1] = 0x25;
                *reinterpret_cast<std::int32_t*>(origStub + 2) = 0;
                *reinterpret_cast<std::uint64_t*>(origStub + 6) = realBody;

            } else if (p[0] == 0xE9) {
                // E9 rel32 — 5-byte long JMP
                realBody = targetAddr + 5 + *reinterpret_cast<const std::int32_t*>(p + 1);
                SKSE::log::info("  -> E9 long-JMP; realBody = {:016X}", realBody);

                origStub = static_cast<std::uint8_t*>(trampoline.allocate(14));
                origStub[0] = 0xFF; origStub[1] = 0x25;
                *reinterpret_cast<std::int32_t*>(origStub + 2) = 0;
                *reinterpret_cast<std::uint64_t*>(origStub + 6) = realBody;

            } else if (p[0] == 0xFF && p[1] == 0x25) {
                // FF 25 rel32 — JMP QWORD PTR [RIP+rel32], 6 bytes
                // The pointer slot is at: targetAddr + 6 + *(int32*)(p+2)
                const std::uintptr_t ptrSlot =
                    targetAddr + 6 + *reinterpret_cast<const std::int32_t*>(p + 2);
                realBody = *reinterpret_cast<const std::uintptr_t*>(ptrSlot);
                SKSE::log::info("  -> FF25 indirect-JMP; ptrSlot={:016X} realBody={:016X}",
                                ptrSlot, realBody);

                origStub = static_cast<std::uint8_t*>(trampoline.allocate(14));
                origStub[0] = 0xFF; origStub[1] = 0x25;
                *reinterpret_cast<std::int32_t*>(origStub + 2) = 0;
                *reinterpret_cast<std::uint64_t*>(origStub + 6) = realBody;

            } else {
                // Regular prologue — copy bytes verbatim, then absolute JMP to
                // continue past the copied region.
                //
                // IMPORTANT: If byte 4 is a REX prefix (0x40-0x4F) AND byte 5 is a
                // short-form PUSH/POP opcode (0x50-0x5F), then bytes [4,5] form a single
                // 2-byte instruction straddling our 5-byte patch boundary.
                // Example prologue "40 53 56 57 41 56 41 57":
                //   bytes 4-5 = "41 56" = PUSH R14 (2 bytes)
                //   if we copy only 5 bytes and JMP to targetAddr+5, the lone "56"
                //   decodes as PUSH RSI instead of PUSH R14 → R14 never saved →
                //   POP R14 at epilogue restores garbage → R14=0xFFFFFFFF crash.
                // Fix: copy one extra byte and JMP to targetAddr+6.
                //
                // NOTE: REX + non-PUSH/POP opcodes (e.g. "48 83" = SUB RSP) need
                // MORE than 6 bytes, so we only special-case the 2-byte PUSH/POP form
                // here.  Anything else that lands mid-instruction would require a
                // proper disassembler.
                const bool byte4IsRexPushPop =
                    (p[4] >= 0x40 && p[4] <= 0x4F) && (p[5] >= 0x50 && p[5] <= 0x5F);
                const std::size_t copyCount = byte4IsRexPushPop ? 6 : 5;
                const std::uintptr_t resumeAddr = targetAddr + copyCount;
                SKSE::log::info("  -> generic prologue; copy {} bytes (byte4=0x{:02X}{}) + abs-JMP to {:016X}",
                                copyCount, p[4], byte4IsRexPushPop ? " REX+PUSH/POP" : "", resumeAddr);

                origStub = static_cast<std::uint8_t*>(trampoline.allocate(copyCount + 14));
                std::memcpy(origStub, p, copyCount);
                origStub[copyCount + 0] = 0xFF; origStub[copyCount + 1] = 0x25;
                *reinterpret_cast<std::int32_t*>(origStub + copyCount + 2) = 0;
                *reinterpret_cast<std::uint64_t*>(origStub + copyCount + 6) = resumeAddr;
            }

            func = reinterpret_cast<func_t>(origStub);

            // ---- Patch game function entry with JMP to our thunk ----
            // write_branch<5> writes E9 <rel32> at targetAddr and allocates a
            // 14-byte relay stub near the game binary so the 32-bit displacement
            // can reach our DLL.  We discard the return value.
            (void)trampoline.write_branch<5>(targetAddr, thunk);

            SKSE::log::info("Installed OpenBookMenu hook (RELOCATION_ID 50122/51053)");
        }
    };

} // anonymous namespace

void SkyrimNetDiaries::BookTextHook::Install()
{
    SKSE::log::info("[BookTextHook] Game language: '{}'",
                    SkyrimNetDiaries::Localization::GetSingleton()->GetLanguageString());

    OpenBookMenuHook::Install();
}
