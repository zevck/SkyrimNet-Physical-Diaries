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
#include "BookText.h"

#include "Detour.h"
#include <thread>

// TESDescription::GetDescription hook: our books' text (no parent: the book menu's; with the book as parent, UTF-8).
// OpenBookMenu: every open, whoever makes it, refreshed with the book menu's text.  docs/BOOK_TEXT.md (Delivery)

namespace
{
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
            SKSE::log::info("[BookTextHook] Refresh of 0x{:X} queued: main thread {}, paused {}", bookId,
                            std::this_thread::get_id() == g_mainThread, ui && ui->GameIsPaused());
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
                                a_out = text.c_str();
                                return;
                            }
                        }
                    } else if (a_parent->GetFormType() == RE::FormType::Book) {
                        // Another reader, any thread: the snapshot, no refresh, UTF-8, tags and all as a
                        // vanilla book's (docs/BOOK_TEXT.md#delivery-the-getdescription-and-openbookmenu-hooks).
                        if (const auto text = books->GetBookTextSnapshot(a_parent->GetFormID()); !text.empty()) {
                            a_out = SkyrimNetDiaries::StripBookmarks(text).c_str();  // the tags are the book menu's only
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
                    // Read again: an inline refresh (paused, main thread) has just changed it.
                    styled = books->GetBookTextSnapshot(bookId);
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
                "books opened by other mods show the text they were given (no refresh)");
        }
    };

} // anonymous namespace


void SkyrimNetDiaries::BookTextHook::Install()
{
    if (std::this_thread::get_id() != g_mainThread) {
        SKSE::log::info("[BookTextHook] Installed off the main thread (kDataLoaded)");
    }
    GetDescriptionHook::Install();
    OpenBookMenuHook::Install();
}

void SkyrimNetDiaries::BookTextHook::RecordMainThread()
{
    g_mainThread = std::this_thread::get_id();
}
