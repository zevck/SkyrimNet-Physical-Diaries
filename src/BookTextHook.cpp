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
                auto* vol = SkyrimNetDiaries::BookManager::GetSingleton()
                                ->GetBookForFormID(a_book->GetFormID());
                if (vol && !vol->cachedBookText.empty()) {
                    RE::BSString ourText(vol->cachedBookText.c_str());
                    return func(ourText, a_extra, a_ref, a_book,
                                a_pos, a_rot, a_scale, a_useDefaultPos);
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

            // We need two allocations from the trampoline pool:
            //   (a) 19 bytes — our "call original" stub:
            //       [5 original prologue bytes copied verbatim]
            //       [FF 25 00 00 00 00]  — JMP QWORD PTR [RIP+0]
            //       [8-byte absolute address of target+5]
            //   (b) 14 bytes — the SKSE relay that write_branch<5> allocates
            //       automatically when it writes E9 <rel32> at target.address()
            // Total: 33 bytes (add padding to be safe)
            SKSE::AllocTrampoline(33 + 8);

            const auto targetAddr = target.address();

            // ---- Build the "call original" stub (19 bytes) ----
            // This lets our thunk fall through to the real game function:
            //   It executes the 5 prologue bytes we're about to overwrite,
            //   then jumps to targetAddr+5 (the rest of the original function).
            //
            // NOTE: write_branch<5>() returns (target+5 + *(int32*)(target+1)),
            // which interprets prologue bytes as a displacement — always garbage
            // unless the function happened to start with E9.  We ignore that
            // return value and use this manually-built stub instead.
            auto* origStub = static_cast<std::uint8_t*>(trampoline.allocate(5 + 14));

            // Copy the 5 bytes we're about to overwrite
            std::memcpy(origStub, reinterpret_cast<const void*>(targetAddr), 5);

            // Append:  FF 25 00 00 00 00  (JMP QWORD PTR [RIP+0])
            //          <8-byte absolute destination>
            origStub[5] = 0xFF;
            origStub[6] = 0x25;
            *reinterpret_cast<std::int32_t*>(origStub + 7) = 0;           // RIP-relative offset 0
            *reinterpret_cast<std::uint64_t*>(origStub + 11) = targetAddr + 5; // continue past hook

            func = reinterpret_cast<func_t>(origStub);

            // ---- Patch game function entry with JMP to our thunk ----
            // write_branch<5> writes E9 <rel32> at targetAddr and allocates a
            // 14-byte relay stub near the game binary so the 32-bit displacement
            // can reach our DLL.  We discard the (garbage) return value.
            (void)trampoline.write_branch<5>(targetAddr, thunk);

            SKSE::log::info("Installed OpenBookMenu hook (RELOCATION_ID 50122/51053)");
        }
    };

} // anonymous namespace

void BookTextHook::Install()
{
    OpenBookMenuHook::Install();
}
