#include "BookTextHook.h"
#include "BookManager.h"

#include "RE/B/BookMenu.h"
#include "RE/B/BSString.h"
#include "RE/E/ExtraDataList.h"
#include "RE/N/NiPoint3.h"
#include "RE/N/NiMatrix3.h"
#include "RE/T/TESObjectBOOK.h"
#include "RE/T/TESObjectREFR.h"
#include "REL/Relocation.h"
#include "SKSE/SKSE.h"

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
                        // Use a static BSString so the underlying storage persists beyond
                        // this stack frame. VR defers book rendering to the next frame, so a
                        // stack-allocated BSString would be a dangling reference by then.
                        // We also write to BookMenu::GetDescription() — the engine-side global
                        // that VR's renderer reads directly rather than using the parameter.
                        static RE::BSString s_injectedText;
                        s_injectedText = vol->cachedBookText.c_str();
                        // GetDescription() uses RELOCATION_ID(519297, 405837) — the VR side
                        // (405837) is absent from the VR address library, so only call it on SSE.
                        if (!REL::Module::IsVR()) {
                            RE::BookMenu::GetDescription() = s_injectedText;
                        }
                        return func(s_injectedText, a_extra, a_ref, a_book,
                                    a_pos, a_rot, a_scale, a_useDefaultPos);
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
    OpenBookMenuHook::Install();
}
