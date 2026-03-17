#include "BookTextHook.h"
#include "BookManager.h"

#include "RE/B/BookMenu.h"
#include "RE/B/BSString.h"
#include <cstring>
#include "RE/E/ExtraDataList.h"
#include "RE/N/NiPoint3.h"
#include "RE/N/NiMatrix3.h"
#include "RE/T/TESObjectBOOK.h"
#include "RE/T/TESObjectREFR.h"
#include "REL/Relocation.h"
#include "SKSE/SKSE.h"
#include <string>
#include <cstdint>
#include <algorithm>
#include <fstream>
#include <shlobj.h>

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
// same job but gates itself out on VR ("Unsupported Skyrim version").  By
// using RELOCATION_ID(50122, 51053) — which has both the SSE and VR address
// library entries — this hook works on both runtimes with no version check.
// ---------------------------------------------------------------------------

namespace
{
    // ── Language detection ────────────────────────────────────────────
    // Read sLanguage from the user's Skyrim.ini.  Returns an uppercase
    // string like "ENGLISH" or "RUSSIAN".  Called once during Install().
    static std::string DetectSkyrimLanguage() {
        char docsPath[MAX_PATH] = {};
        if (FAILED(SHGetFolderPathA(nullptr, CSIDL_PERSONAL, nullptr,
                                    SHGFP_TYPE_CURRENT, docsPath))) {
            SKSE::log::warn("[BookTextHook] SHGetFolderPath failed; assuming ENGLISH");
            return "ENGLISH";
        }
        std::string iniPath = std::string(docsPath)
                            + "\\My Games\\Skyrim Special Edition\\Skyrim.ini";
        std::ifstream f(iniPath);
        if (!f.is_open()) {
            SKSE::log::warn("[BookTextHook] Could not open '{}'; assuming ENGLISH", iniPath);
            return "ENGLISH";
        }
        std::string line;
        while (std::getline(f, line)) {
            // Trim leading whitespace
            auto ns = line.find_first_not_of(" \t");
            if (ns == std::string::npos) continue;
            line = line.substr(ns);
            if (line.empty() || line[0] == ';' || line[0] == '[') continue;
            if (line.size() >= 9 && line.substr(0, 9) == "sLanguage") {
                auto eq = line.find('=');
                if (eq != std::string::npos) {
                    std::string val = line.substr(eq + 1);
                    while (!val.empty() && (val.back() == ' ' || val.back() == '\r'
                                        || val.back() == '\n' || val.back() == '\t'))
                        val.pop_back();
                    auto vs = val.find_first_not_of(" \t");
                    if (vs != std::string::npos) val = val.substr(vs);
                    std::transform(val.begin(), val.end(), val.begin(), ::toupper);
                    return val;
                }
            }
        }
        return "ENGLISH";
    }

    // Cached game language (set during Install(), UPPERCASE).
    static std::string s_gameLanguage;

    // ── UTF-8 → Windows-1251 conversion ──────────────────────────────
    // Scaleform GFx 4 (Skyrim's Flash engine) treats string bytes as character
    // indices in replaceText/getLineOffset/length. For UTF-8 multibyte text
    // (like Cyrillic, 2 bytes per char), this causes a byte/char index mismatch
    // in the BookMenu pagination code, resulting in progressive text overlap.
    //
    // Vanilla Russian Skyrim uses Windows-1251 (single-byte Cyrillic encoding)
    // where byte == char, so pagination works correctly. This function converts
    // UTF-8 Cyrillic to Win-1251 so Scaleform can paginate properly.
    //
    // Characters outside Win-1251 Cyrillic are left as UTF-8 (best effort).
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

                // Cyrillic block: А-п U+0410-U+043F → 0xC0-0xEF
                //                 р-я U+0440-U+044F → 0xF0-0xFF
                if (cp >= 0x0410 && cp <= 0x044F) {
                    out += static_cast<char>(cp - 0x0410 + 0xC0);
                } else if (cp == 0x0401) {  // Ё → 0xA8
                    out += static_cast<char>(0xA8);
                } else if (cp == 0x0451) {  // ё → 0xB8
                    out += static_cast<char>(0xB8);
                // Ukrainian / Belarusian Cyrillic in Win-1251
                } else if (cp == 0x0404) {  // Є → 0xAA
                    out += static_cast<char>(0xAA);
                } else if (cp == 0x0406) {  // І → 0xB2
                    out += static_cast<char>(0xB2);
                } else if (cp == 0x0407) {  // Ї → 0xAF
                    out += static_cast<char>(0xAF);
                } else if (cp == 0x0454) {  // є → 0xBA
                    out += static_cast<char>(0xBA);
                } else if (cp == 0x0456) {  // і → 0xB3
                    out += static_cast<char>(0xB3);
                } else if (cp == 0x0457) {  // ї → 0xBF
                    out += static_cast<char>(0xBF);
                } else if (cp == 0x0490) {  // Ґ → 0xA5
                    out += static_cast<char>(0xA5);
                } else if (cp == 0x0491) {  // ґ → 0xB4
                    out += static_cast<char>(0xB4);
                } else if (cp == 0x040E) {  // Ў → 0xA1 (Belarusian)
                    out += static_cast<char>(0xA1);
                } else if (cp == 0x045E) {  // ў → 0xA2 (Belarusian)
                    out += static_cast<char>(0xA2);
                // Serbian / Macedonian / Bosnian Cyrillic in Win-1251
                } else if (cp == 0x0402) {  // Ђ → 0x80 (Serbian)
                    out += static_cast<char>(0x80);
                } else if (cp == 0x0452) {  // ђ → 0x90 (Serbian)
                    out += static_cast<char>(0x90);
                } else if (cp == 0x0409) {  // Љ → 0x8A (Serbian, Macedonian)
                    out += static_cast<char>(0x8A);
                } else if (cp == 0x0459) {  // љ → 0x9A (Serbian, Macedonian)
                    out += static_cast<char>(0x9A);
                } else if (cp == 0x040A) {  // Њ → 0x8C (Serbian, Macedonian)
                    out += static_cast<char>(0x8C);
                } else if (cp == 0x045A) {  // њ → 0x9C (Serbian, Macedonian)
                    out += static_cast<char>(0x9C);
                } else if (cp == 0x040B) {  // Ћ → 0x8D (Serbian)
                    out += static_cast<char>(0x8D);
                } else if (cp == 0x045B) {  // ћ → 0x9D (Serbian)
                    out += static_cast<char>(0x9D);
                } else if (cp == 0x040F) {  // Џ → 0x8F (Serbian, Macedonian)
                    out += static_cast<char>(0x8F);
                } else if (cp == 0x045F) {  // џ → 0x9F (Serbian, Macedonian)
                    out += static_cast<char>(0x9F);
                } else if (cp == 0x0403) {  // Ѓ → 0x81 (Macedonian)
                    out += static_cast<char>(0x81);
                } else if (cp == 0x0453) {  // ѓ → 0x83 (Macedonian)
                    out += static_cast<char>(0x83);
                } else if (cp == 0x040C) {  // Ќ → 0x8E (Macedonian)
                    out += static_cast<char>(0x8E);
                } else if (cp == 0x045C) {  // ќ → 0x9E (Macedonian)
                    out += static_cast<char>(0x9E);
                } else if (cp == 0x0405) {  // Ѕ → 0xBD (Macedonian)
                    out += static_cast<char>(0xBD);
                } else if (cp == 0x0455) {  // ѕ → 0xBE (Macedonian)
                    out += static_cast<char>(0xBE);
                } else if (cp == 0x0408) {  // Ј → 0xA3 (Serbian, Macedonian)
                    out += static_cast<char>(0xA3);
                } else if (cp == 0x0458) {  // ј → 0xBC (Serbian, Macedonian)
                    out += static_cast<char>(0xBC);
                } else if (cp == 0x2013) {  // en-dash → 0x96
                    out += static_cast<char>(0x96);
                } else if (cp == 0x2014) {  // em-dash → 0x97
                    out += static_cast<char>(0x97);
                } else if (cp == 0x201C) {  // left double quote → 0x93
                    out += static_cast<char>(0x93);
                } else if (cp == 0x201D) {  // right double quote → 0x94
                    out += static_cast<char>(0x94);
                } else if (cp == 0x201E) {  // bottom double quote → 0x84
                    out += static_cast<char>(0x84);
                } else if (cp == 0x2026) {  // ellipsis → 0x85
                    out += static_cast<char>(0x85);
                } else if (cp == 0x2116) {  // № → 0xB9
                    out += static_cast<char>(0xB9);
                } else if (cp == 0x00AB) {  // « → 0xAB
                    out += static_cast<char>(0xAB);
                } else if (cp == 0x00BB) {  // » → 0xBB
                    out += static_cast<char>(0xBB);
                } else {
                    // Unmapped 2-byte char: pass through as UTF-8 bytes
                    out += static_cast<char>(*(p-2));
                    out += static_cast<char>(*(p-1));
                }
            } else if ((*p & 0xF0) == 0xE0 && p + 2 < end) {
                // 3-byte UTF-8: pass through unchanged
                out += static_cast<char>(*p++);
                out += static_cast<char>(*p++);
                out += static_cast<char>(*p++);
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

        static void thunk(const RE::BSString&    a_desc,
                          const RE::ExtraDataList* a_extra,
                          RE::TESObjectREFR*    a_ref,
                          RE::TESObjectBOOK*   a_book,
                          const RE::NiPoint3&  a_pos,
                          const RE::NiMatrix3& a_rot,
                          float                a_scale,
                          bool                 a_useDefaultPos)
        {
            if (a_book) {
                auto* bookManager = SkyrimNetDiaries::BookManager::GetSingleton();
                auto* vol = bookManager->GetBookForFormID(a_book->GetFormID());
                if (vol) {
                    // Refresh text before injection: detects new/deleted entries and
                    // reformats if the live count differs from the cached count.
                    bookManager->RefreshVolumeOnOpen(vol);
                    if (!vol->cachedBookText.empty()) {

                        SKSE::log::info("[BookTextHook] Opening diary: formId=0x{:X} actor='{}' vol={} textLen={}",
                            a_book->GetFormID(), vol->actorName, vol->volumeNumber, vol->cachedBookText.size());

                        // Always apply Win-1251 conversion for any Cyrillic content.
                        // Scaleform GFx's replaceText() uses BYTE offsets but
                        // getLineOffset() returns CHARACTER indices. For 2-byte UTF-8
                        // Cyrillic this causes progressive text overlay. Win-1251 is
                        // single-byte Cyrillic so byte == char, fixing the mismatch.
                        // The conversion is a no-op for ASCII, so it is safe for all
                        // locales — covers Russian, Ukrainian, Belarusian, and any
                        // user who has a Cyrillic-capable font mod installed.
                        std::string textToInject = Utf8ToWin1251(vol->cachedBookText);

                        static RE::BSString s_injectedText;
                        s_injectedText = textToInject.c_str();

                        func(s_injectedText, a_extra, a_ref, a_book,
                             a_pos, a_rot, a_scale, a_useDefaultPos);
                        return;
                    }
                }
            }
            return func(a_desc, a_extra, a_ref, a_book,
                        a_pos, a_rot, a_scale, a_useDefaultPos);
        }

        static inline func_t func{ nullptr };

        static void Install()
        {
            // SSE id: 50122  |  VR id: 51053
            REL::Relocation<std::uintptr_t> target{ RELOCATION_ID(50122, 51053) };
            auto& trampoline = SKSE::GetTrampoline();

            // Reserve enough for all cases + write_branch<5> relay stub.
            SKSE::AllocTrampoline(20 + 14 + 8);

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

void BookTextHook::Install()
{
    // Detect game language once and cache it.
    // Must be done before the hook is installed so that s_gameLanguage
    // is ready when the first diary book is opened.
    s_gameLanguage = DetectSkyrimLanguage();
    SKSE::log::info("[BookTextHook] Detected game language: '{}'", s_gameLanguage);

    OpenBookMenuHook::Install();
}
