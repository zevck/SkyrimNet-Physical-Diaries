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

#include "BookEditor.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <atomic>
#include <chrono>

namespace SkyrimNetDiaries::BookEditor {

    namespace {
        std::atomic<bool> g_armed{ false };   // F3 pressed: the next book opened enters edit mode
        std::atomic<bool> g_active{ false };  // edit mode is on in the open book menu

        // Held-key repeat, like a text box (input thread only).
        constexpr auto kKeyRepeatDelay = std::chrono::milliseconds(400);
        constexpr auto kKeyRepeatRate = std::chrono::milliseconds(50);
        std::uint32_t g_lastScanCode = 0;
        std::chrono::steady_clock::time_point g_lastKeyTime{};
        bool g_keyRepeating = false;

        constexpr std::uint32_t kF3 = 0x3D, kEscape = 0x01, kBackspace = 0x0E, kEnter = 0x1C, kDelete = 0xD3;

        // book.swf is its own movie, separate from bookmenu.swf.
        RE::GFxMovieView* BookMovie() {
            auto* ui = RE::UI::GetSingleton();
            auto bookMenu = ui ? ui->GetMenu<RE::BookMenu>() : nullptr;
            return bookMenu ? bookMenu->GetRuntimeData().book.get() : nullptr;
        }

        // DIAGNOSTIC: the SWF's own description of its display state (BookMenu.as DebugState).
        void DumpState(std::string_view when) {
            auto* ui = RE::UI::GetSingleton();
            auto bookMenu = ui ? ui->GetMenu<RE::BookMenu>() : nullptr;
            auto* movie = BookMovie();
            if (!movie) {
                SKSE::log::info("[BookEditor] state ({}): no book movie", when);
                return;
            }
            RE::GFxValue result;
            const bool ok = movie->Invoke("_root.BookMenu_mc.DebugState", &result, nullptr, 0);
            SKSE::log::info("[BookEditor] state ({}): movie visible={} menu flags=0x{:X} paused={} | {}", when,
                            movie->GetVisible(), bookMenu ? bookMenu->menuFlags.underlying() : 0,
                            ui && ui->GameIsPaused(), ok && result.IsString() ? result.GetString() : "DebugState failed");
        }
        int g_keysLogged = 0;  // input thread only
        constexpr std::uint32_t kF4 = 0x3E;

        void Invoke(const char* function, const char* argument = nullptr) {
            auto* movie = BookMovie();
            if (!movie) return;
            RE::GFxValue arg;
            if (argument) arg.SetString(argument);
            movie->Invoke(std::format("_root.BookMenu_mc.{}", function).c_str(), nullptr, argument ? &arg : nullptr,
                          argument ? 1 : 0);
        }

        // Keys reach the menu blanked (InputSink), so no controls need turning off; the mouse
        // keeps the book's own page turns (left click previous, right click next).
        void SetTextInput(bool enabled) {
            if (auto* controls = RE::ControlMap::GetSingleton()) controls->AllowTextInput(enabled);
        }

        void EnterEditMode() {
            auto* movie = BookMovie();
            if (!movie) {
                SKSE::log::warn("[BookEditor] No book movie to edit");
                return;
            }
            RE::GFxValue enabled;
            enabled.SetBoolean(true);
            // False when the loaded book.swf isn't ours (another mod's won): no edit mode.
            if (!movie->Invoke("_root.BookMenu_mc.SetEditMode", nullptr, &enabled, 1)) {
                SKSE::log::warn("[BookEditor] book.swf has no SetEditMode: it isn't SNPD's (check the load order)");
                return;
            }
            g_active = true;
            g_keysLogged = 0;
            SetTextInput(true);
            SKSE::log::info("[BookEditor] Edit mode on");
            DumpState("entered");
        }

        void LeaveEditMode() {
            if (!g_active.exchange(false)) return;
            SetTextInput(false);
            SKSE::log::info("[BookEditor] Edit mode off");
        }

        class MenuSink : public RE::BSTEventSink<RE::MenuOpenCloseEvent> {
        public:
            RE::BSEventNotifyControl ProcessEvent(const RE::MenuOpenCloseEvent* a_event,
                                                  RE::BSTEventSource<RE::MenuOpenCloseEvent>*) override {
                if (!a_event || a_event->menuName != RE::BookMenu::MENU_NAME) return RE::BSEventNotifyControl::kContinue;
                if (a_event->opening && g_armed.exchange(false)) {
                    SKSE::GetTaskInterface()->AddUITask([]() { EnterEditMode(); });
                } else if (!a_event->opening) {
                    LeaveEditMode();
                }
                return RE::BSEventNotifyControl::kContinue;
            }
        };

        // True if this press (or held repeat) should produce input now.
        bool ShouldProcess(const RE::ButtonEvent* button, std::uint32_t scanCode) {
            if (button->IsUp()) {
                if (scanCode == g_lastScanCode) {
                    g_lastScanCode = 0;
                    g_keyRepeating = false;
                }
                return false;
            }
            const auto now = std::chrono::steady_clock::now();
            if (button->IsDown()) {
                g_lastScanCode = scanCode;
                g_lastKeyTime = now;
                g_keyRepeating = false;
                return true;
            }
            if (!button->IsHeld()) return false;
            if (now - g_lastKeyTime < (g_keyRepeating ? kKeyRepeatRate : kKeyRepeatDelay)) return false;
            g_keyRepeating = true;
            g_lastKeyTime = now;
            return true;
        }


        void HandleKey(std::uint32_t scanCode) {
            if (scanCode == kEscape) {
                // One press closes the book: text input would otherwise swallow it.
                LeaveEditMode();
                if (auto* queue = RE::UIMessageQueue::GetSingleton()) {
                    queue->AddMessage(RE::BookMenu::MENU_NAME, RE::UI_MESSAGE_TYPE::kHide, nullptr);
                }
                return;
            }
            static const std::unordered_map<std::uint32_t, const char*> kCursorKeys{
                { 0xCB, "left" }, { 0xCD, "right" }, { 0xC8, "up" }, { 0xD0, "down" }, { 0xC7, "home" }, { 0xCF, "end" },
            };
            if (const auto key = kCursorKeys.find(scanCode); key != kCursorKeys.end()) {
                Invoke("EditMoveCursor", key->second);
                return;
            }
            if (scanCode == kBackspace) return Invoke("EditBackspace");
            if (scanCode == kDelete) return Invoke("EditDelete");
            if (scanCode == kEnter) return Invoke("AppendEditChar", "\n");

            // The character for this key with the active keyboard layout and modifiers.
            BYTE keyState[256] = {};
            GetKeyboardState(keyState);
            WCHAR chars[4] = {};
            const auto vk = MapVirtualKeyW(scanCode, MAPVK_VSC_TO_VK);
            if (ToUnicode(vk, scanCode, keyState, chars, 4, 0) > 0 && chars[0] >= 32) {
                char utf8[8] = {};
                WideCharToMultiByte(CP_UTF8, 0, chars, 1, utf8, sizeof(utf8), nullptr, nullptr);
                Invoke("AppendEditChar", utf8);
            }
        }

        class InputSink : public RE::BSTEventSink<RE::InputEvent*> {
        public:
            RE::BSEventNotifyControl ProcessEvent(RE::InputEvent* const* a_event,
                                                  RE::BSTEventSource<RE::InputEvent*>*) override {
                for (auto* event = a_event ? *a_event : nullptr; event; event = event->next) {
                    auto* button = event->AsButtonEvent();
                    if (!button) continue;
                    const auto code = button->GetIDCode();
                    if (button->GetDevice() == RE::INPUT_DEVICE::kMouse) {
                        // DIAGNOSTIC: what the book menu gets for a click (PrevPage/NextPage expected)
                        if (g_active && button->IsDown()) {
                            SKSE::log::info("[BookEditor] mouse {} user event '{}'", code, button->QUserEvent().c_str());
                        }
                        continue;
                    }
                    if (button->GetDevice() != RE::INPUT_DEVICE::kKeyboard) continue;
                    if (g_active) {
                        // The book menu turns pages on the arrow keys, A and D by key code, not by
                        // user event, so it can't be kept from seeing them. Instead the SWF refuses
                        // turns while keys are in use: every event (press, held repeat, release)
                        // renews the block; a click never comes with one.
                        Invoke("EditSuppressTurn");
                        // Keys are text: blank the user event for anything that does go by it.
                        button->SetUserEvent("");
                    }
                    if (code == kF3 && button->IsDown()) {
                        g_armed = true;
                        SKSE::log::info("[BookEditor] F3: edit mode armed, open any book or note");
                        continue;
                    }
                    if (code == kF4 && button->IsDown()) {
                        DumpState("F4");
                        continue;
                    }
                    // Modifiers (Shift, Ctrl, Alt, Caps Lock) only change other keys.
                    if (!g_active || code == 0x2A || code == 0x36 || code == 0x1D || code == 0x9D || code == 0x38 ||
                        code == 0xB8 || code == 0x3A) {
                        continue;
                    }
                    // Directly: the book menu pauses the game, so input arrives on the
                    // main thread, and the keyboard state still matches this press.
                    if (ShouldProcess(button, code)) {
                        HandleKey(code);
                        if (g_keysLogged < 3) DumpState(std::format("after key {} (0x{:X})", ++g_keysLogged, code));
                    }
                }
                return RE::BSEventNotifyControl::kContinue;
            }
        };
    }

    void Register() {
        static MenuSink menuSink;
        static InputSink inputSink;
        if (auto* ui = RE::UI::GetSingleton()) ui->AddEventSink<RE::MenuOpenCloseEvent>(&menuSink);
        // First in line, ahead of MenuControls, so blanked keys never reach the menu.
        if (auto* input = RE::BSInputDeviceManager::GetSingleton()) input->PrependEventSink(&inputSink);
        SKSE::log::info("[BookEditor] Dev harness registered: F3 arms edit mode for the next book opened");
    }

} // namespace SkyrimNetDiaries::BookEditor
