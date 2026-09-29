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

#include "BookManager.h"
#include "BookText.h"
#include "BookTextHook.h"
#include "Config.h"
#include "Database.h"
#include "Localization.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <atomic>
#include <chrono>
#include <optional>
#include <thread>

namespace SkyrimNetDiaries::BookEditor {

    namespace {
        std::atomic<bool> g_active{ false };  // edit mode is on in the open book menu

        // Held-key repeat, like a text box (input thread only).
        constexpr auto kKeyRepeatDelay = std::chrono::milliseconds(400);
        constexpr auto kKeyRepeatRate = std::chrono::milliseconds(50);
        std::uint32_t g_lastScanCode = 0;
        std::chrono::steady_clock::time_point g_lastKeyTime{};
        bool g_keyRepeating = false;

        constexpr std::uint32_t kEscape = 0x01, kBackspace = 0x0E, kEnter = 0x1C, kDelete = 0xD3;

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
        constexpr std::uint32_t kF4 = 0x3E, kF5 = 0x3F;

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

        // The entries the editor was given, in page order: each one's SkyrimNet id and the text
        // it started with (main thread only).  Saving compares against these.
        struct LoadedEntry {
            int id = 0;
            std::string body;
            std::vector<std::string> tags;
        };
        std::vector<LoadedEntry> g_loaded;
        std::vector<DiaryEntry> g_entries;     // the same entries, whole, with saved edits applied
        std::string g_actorName;

        // Saves SkyrimNet hasn't finished, for one volume: until then SkyrimNet still has the
        // old text, so editing that volume again starts from the saved entries instead.
        struct PendingSave {
            RE::FormID bookFormId = 0;
            int count = 0;
            std::vector<DiaryEntry> entries;
        };
        PendingSave g_pending;  // game thread only
        RE::FormID g_bookFormId = 0;           // the diary volume being edited (0: not a diary)
        std::atomic<bool> g_prompting{ false };  // the save prompt is open: keys belong to it

        // "\r\n" and "\r" to "\n": what a body looks like after the SWF round trip (its field
        // uses "\r"), so an untouched entry compares equal to what was loaded.
        std::string NormalizeLineBreaks(const std::string& text) {
            std::string out;
            out.reserve(text.size());
            for (std::size_t i = 0; i < text.size(); ++i) {
                if (text[i] == '\r') {
                    out += '\n';
                    if (i + 1 < text.size() && text[i + 1] == '\n') ++i;
                } else {
                    out += text[i];
                }
            }
            return out;
        }

        // The open book, if it's one of the player's diary volumes: its entries go to the SWF,
        // laid out as the book shows them.  Any other book can't be edited; another actor's
        // diary least of all (its entries are that actor's memories).
        bool SendDiaryContent(RE::GFxMovieView* movie) {
            g_loaded.clear();
            g_entries.clear();
            g_bookFormId = 0;
            auto* book = RE::BookMenu::GetTargetForm();
            auto* manager = BookManager::GetSingleton();
            auto* vol = book && manager ? manager->GetBookForFormID(book->GetFormID()) : nullptr;
            if (!vol) return false;

            const std::string playerUuid = Database::GetUUIDFromFormID(0x14);
            if (playerUuid.empty() || vol->actorUuid != playerUuid) {
                SKSE::log::info("[BookEditor] {} vol {} isn't the player's diary: not editable", vol->actorName,
                                vol->volumeNumber);
                return false;
            }
            std::vector<DiaryEntry> entries;
            if (g_pending.count > 0 && g_pending.bookFormId == vol->bookFormId) {
                entries = g_pending.entries;
                SKSE::log::info("[BookEditor] A save is still in SkyrimNet: editing the saved text");
            } else {
                bool ok = false;
                entries = manager->GetShownEntries(*vol, &ok);
                if (!ok) {
                    SKSE::log::warn("[BookEditor] {} vol {}: couldn't read the entries from SkyrimNet", vol->actorName,
                                    vol->volumeNumber);
                    return false;
                }
            }

            // "heading\x1Fbody" per entry, joined by \x1E (BookMenu.as SetEditContent).
            std::string packed;
            std::string ids;
            for (const auto& entry : entries) {
                LoadedEntry loaded{ entry.id, NormalizeLineBreaks(EditableEntryText(entry)), entry.tags };
                if (!packed.empty()) packed += '\x1E';
                packed += EntryHeading(entry) + '\x1F' + loaded.body;
                ids += std::format("{}{}", ids.empty() ? "" : ",", entry.id);
                g_loaded.push_back(std::move(loaded));
            }
            auto* config = Config::GetSingleton();
            const std::string font = config->GetFontFace();
            const std::string title = Localization::GetSingleton()->FormatDiaryTitle(vol->actorName);
            const std::string dates = TitlePageDates(entries);
            RE::GFxValue args[8];
            args[0].SetString(font.c_str());
            args[1].SetNumber(config->GetFontSizeTitle());
            args[2].SetNumber(config->GetFontSizeSmall());
            args[3].SetNumber(config->GetFontSizeDate());
            args[4].SetNumber(config->GetFontSizeContent());
            args[5].SetString(title.c_str());
            args[6].SetString(dates.c_str());
            args[7].SetString(packed.c_str());
            if (!movie->Invoke("_root.BookMenu_mc.SetEditContent", nullptr, args, 8)) {
                SKSE::log::warn("[BookEditor] book.swf has no SetEditContent: it isn't SNPD's (check the load order)");
                return false;
            }
            g_bookFormId = vol->bookFormId;
            g_entries = entries;
            g_actorName = vol->actorName;
            SKSE::log::info("[BookEditor] {} vol {}: {} entries loaded for editing (ids {})", vol->actorName,
                            vol->volumeNumber, entries.size(), ids);
            return true;
        }

        // Each entry's text as the editor has it now, in page order; nullopt if the SWF can't
        // say or the count doesn't match what was loaded.
        std::optional<std::vector<std::string>> ReadBodies() {
            auto* movie = BookMovie();
            RE::GFxValue result;
            if (!movie || !movie->Invoke("_root.BookMenu_mc.EditGetBodies", &result, nullptr, 0) ||
                !result.IsString()) {
                SKSE::log::warn("[BookEditor] No text from the SWF");
                return std::nullopt;
            }
            std::vector<std::string> bodies;
            std::string_view all = result.GetString();
            for (std::size_t start = 0;;) {
                const auto end = all.find('\x1E', start);
                bodies.emplace_back(all.substr(start, end == std::string_view::npos ? all.npos : end - start));
                if (end == std::string_view::npos) break;
                start = end + 1;
            }
            if (bodies.size() != g_loaded.size()) {
                SKSE::log::warn("[BookEditor] {} texts from the SWF for {} loaded entries", bodies.size(),
                                g_loaded.size());
                return std::nullopt;
            }
            return bodies;
        }

        // DIAGNOSTIC (F5): what saving would do now, without writing anything.
        void PreviewSave() {
            const auto bodies = ReadBodies();
            if (!bodies) return;
            int changed = 0;
            for (std::size_t i = 0; i < bodies->size(); ++i) {
                const auto& body = (*bodies)[i];
                if (body == g_loaded[i].body) continue;
                ++changed;
                SKSE::log::info("[BookEditor] save preview: entry {} (id {}) {}: {} -> {} chars | {}", i,
                                g_loaded[i].id, body.empty() ? "emptied (left as is)" : "changed (update)",
                                g_loaded[i].body.size(), body.size(), body.substr(0, 200));
            }
            SKSE::log::info("[BookEditor] save preview: {} of {} entries changed", changed, bodies->size());
        }

        // DIAGNOSTIC: where an entry's text first differs from what was loaded, with the
        // bytes around it, to find anything the SWF round trip changes.
        std::string Escaped(std::string_view text) {
            std::string out;
            for (const unsigned char c : text) {
                if (c >= 32 && c < 127) out += static_cast<char>(c);
                else out += std::format("\\x{:02X}", c);
            }
            return out;
        }

        void LogDifference(std::size_t index, const std::string& loaded, const std::string& now) {
            std::size_t at = 0;
            while (at < loaded.size() && at < now.size() && loaded[at] == now[at]) ++at;
            const std::size_t from = at > 20 ? at - 20 : 0;
            SKSE::log::info("[BookEditor] entry {} differs at byte {} ({} -> {} bytes): loaded '{}' | now '{}'", index,
                            at, loaded.size(), now.size(), Escaped(std::string_view(loaded).substr(from, 40)),
                            Escaped(std::string_view(now).substr(from, 40)));
        }

        bool HasChanges() {
            if (g_bookFormId == 0) return false;
            const auto bodies = ReadBodies();
            if (!bodies) return false;
            bool changed = false;
            for (std::size_t i = 0; i < bodies->size(); ++i) {
                if ((*bodies)[i] != g_loaded[i].body) {
                    LogDifference(i, g_loaded[i].body, (*bodies)[i]);
                    changed = true;
                }
            }
            return changed;
        }

        struct EntryUpdate {
            int id = 0;
            std::string text;
            std::string tagsCSV;
        };

        // Writes the changed entries to SkyrimNet.  Only entries whose text changed are written;
        // each gets the player-written tag (keeping its other tags).  The volume shows the edit
        // at once (rendered from the edited entries), and is re-rendered from SkyrimNet when the
        // writes are done.  An emptied entry is left as it is: deleting one moves the volume
        // boundaries, which isn't handled yet.
        void Save() {
            if (g_bookFormId == 0) return;
            const auto bodies = ReadBodies();
            if (!bodies) return;
            std::vector<EntryUpdate> updates;
            for (std::size_t i = 0; i < bodies->size(); ++i) {
                auto& loaded = g_loaded[i];
                const auto& body = (*bodies)[i];
                if (body == loaded.body) continue;
                if (body.empty()) {
                    SKSE::log::info("[BookEditor] Entry {} emptied: not deleted (not supported yet), left as it was",
                                    loaded.id);
                    continue;
                }
                if (loaded.id == 0) {
                    SKSE::log::warn("[BookEditor] Entry {} has no SkyrimNet id: can't save it", i);
                    continue;
                }
                if (std::ranges::find(loaded.tags, kPlayerWrittenTag) == loaded.tags.end()) {
                    loaded.tags.emplace_back(kPlayerWrittenTag);
                }
                std::string tags;
                for (const auto& tag : loaded.tags) tags += (tags.empty() ? "" : ",") + tag;
                updates.push_back({ loaded.id, body, std::move(tags) });
                loaded.body = body;
                g_entries[i].content = body;
                g_entries[i].tags = loaded.tags;
            }
            if (updates.empty()) return;

            auto* manager = BookManager::GetSingleton();
            if (auto* vol = manager->GetBookForFormID(g_bookFormId)) manager->SetVolumeText(*vol, g_entries);
            if (g_pending.bookFormId != g_bookFormId) g_pending.count = 0;
            g_pending.bookFormId = g_bookFormId;
            g_pending.entries = g_entries;
            ++g_pending.count;

            // Off the game thread: SkyrimNet re-embeds each entry's memory.
            std::thread([updates = std::move(updates), bookFormId = g_bookFormId]() {
                std::size_t saved = 0;
                for (const auto& update : updates) {
                    if (Database::UpdateDiaryEntry(update.id, update.text, update.tagsCSV)) {
                        ++saved;
                        SKSE::log::info("[BookEditor] Saved entry {} ({} chars)", update.id, update.text.size());
                    } else {
                        SKSE::log::error("[BookEditor] SkyrimNet didn't save entry {}", update.id);
                    }
                }
                SKSE::GetTaskInterface()->AddTask([bookFormId, failed = saved < updates.size()]() {
                    if (g_pending.bookFormId == bookFormId && g_pending.count > 0) --g_pending.count;
                    if (failed) RE::SendHUDMessage::ShowHUDMessage(Localization::GetSingleton()->GetEditSaveFailed().c_str());
                    auto* manager = BookManager::GetSingleton();
                    auto* vol = manager->GetBookForFormID(bookFormId);
                    if (!vol) return;
                    bool ok = false;
                    const auto entries = manager->GetShownEntries(*vol, &ok);
                    if (ok) manager->SetVolumeText(*vol, entries);
                });
            }).detach();
        }

        void EnterEditMode() {
            auto* movie = BookMovie();
            if (!movie) {
                SKSE::log::warn("[BookEditor] No book movie to edit");
                return;
            }
            if (!SendDiaryContent(movie)) return;
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
            g_prompting = false;
            SetTextInput(false);
            SKSE::log::info("[BookEditor] Edit mode off");
        }

        // Ask the book menu to close.  While editing, the close hook turns it into the save
        // prompt if there are unsaved changes.
        void RequestClose() {
            if (auto* queue = RE::UIMessageQueue::GetSingleton()) {
                queue->AddMessage(RE::BookMenu::MENU_NAME, RE::UI_MESSAGE_TYPE::kHide, nullptr);
            }
        }

        // Close without asking: edit mode ends first, so the close hook lets it through.
        void CloseBook() {
            LeaveEditMode();
            RequestClose();
        }

        // The edit key while writing: save, and turn the book back into reading on the same
        // spread, with the saved text.
        void SaveAndRead() {
            Save();
            auto* movie = BookMovie();
            const std::string text = BookTextHook::ForBookMenu(FormatDiaryEntries(g_entries, g_actorName));
            LeaveEditMode();
            RE::GFxValue arg;
            arg.SetString(text.c_str());
            if (!movie || !movie->Invoke("_root.BookMenu_mc.ReturnToReading", nullptr, &arg, 1)) {
                SKSE::log::warn("[BookEditor] Couldn't return to reading: closing the book");
                RequestClose();
            }
        }

        // Save / Discard / Keep writing, over the open book.
        class SavePromptCallback : public RE::IMessageBoxCallback {
        public:
            void Run(std::uint8_t a_button) override {
                if (!g_active) return;
                if (a_button == 0) {
                    Save();
                    CloseBook();
                } else if (a_button == 1) {
                    SKSE::log::info("[BookEditor] Changes discarded");
                    CloseBook();
                } else {
                    g_prompting = false;
                    SetTextInput(true);
                }
            }
        };

        void ShowSavePrompt() {
            auto* data = RE::UIMessageDataFactory::Create<RE::MessageBoxData>();
            if (!data) return;
            auto* loc = Localization::GetSingleton();
            data->bodyText = loc->GetEditSavePrompt().c_str();
            data->buttonText.push_back(loc->GetEditSave().c_str());
            data->buttonText.push_back(loc->GetEditDiscard().c_str());
            data->buttonText.push_back(loc->GetEditKeepWriting().c_str());
            data->cancelButtonIndex = 2;  // Escape on the prompt: keep writing
            data->callback = RE::BSTSmartPointer<RE::IMessageBoxCallback>(new SavePromptCallback());
            // The prompt's own keys (Enter, Escape) must reach it.
            g_prompting = true;
            SetTextInput(false);
            RE::MessageBoxMenu::QueueMessage(data);
        }

        class MenuSink : public RE::BSTEventSink<RE::MenuOpenCloseEvent> {
        public:
            RE::BSEventNotifyControl ProcessEvent(const RE::MenuOpenCloseEvent* a_event,
                                                  RE::BSTEventSource<RE::MenuOpenCloseEvent>*) override {
                if (!a_event || a_event->menuName != RE::BookMenu::MENU_NAME) return RE::BSEventNotifyControl::kContinue;
                if (!a_event->opening) LeaveEditMode();
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
            if (!button->IsHeld() || scanCode != g_lastScanCode) return false;
            if (now - g_lastKeyTime < (g_keyRepeating ? kKeyRepeatRate : kKeyRepeatDelay)) return false;
            g_keyRepeating = true;
            g_lastKeyTime = now;
            return true;
        }


        void HandleKey(std::uint32_t scanCode) {
            if (scanCode == kEscape) {
                // One press closes the book (text input would swallow it); with unsaved changes
                // the close hook asks first.
                RequestClose();
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

        // Every way of closing the book (Escape, the menu's own buttons, a gamepad, another mod)
        // reaches the menu as a kHide message.  While editing with unsaved changes it's held
        // back and the save prompt opens instead; Save and Discard end edit mode and close
        // again.  kForceHide (a load, the game shutting menus) always goes through.
        struct BookMenuProcessMessage {
            static RE::UI_MESSAGE_RESULTS thunk(RE::IMenu* a_menu, RE::UIMessage& a_message) {
                const auto type = a_message.type.get();
                if (g_active) {
                    // DIAGNOSTIC: which messages each way of closing sends
                    if (type != RE::UI_MESSAGE_TYPE::kUpdate && type != RE::UI_MESSAGE_TYPE::kScaleformEvent &&
                        !(g_prompting && type == RE::UI_MESSAGE_TYPE::kHide)) {
                        const auto* data = type == RE::UI_MESSAGE_TYPE::kUserEvent
                                               ? static_cast<RE::BSUIMessageData*>(a_message.data)
                                               : nullptr;
                        SKSE::log::info("[BookEditor] book menu message {} {}", std::to_underlying(type),
                                        data ? data->fixedStr.c_str() : "");
                    }
                    // A gamepad's B (and anything else sending Cancel): the menu would play its
                    // close animation before its hide, so catch Cancel itself.  kHandled here
                    // consumes it.
                    if (type == RE::UI_MESSAGE_TYPE::kUserEvent) {
                        const auto* data = static_cast<RE::BSUIMessageData*>(a_message.data);
                        if (data && data->fixedStr == "Cancel") {
                            if (g_prompting) return RE::UI_MESSAGE_RESULTS::kHandled;
                            if (HasChanges()) {
                                SKSE::log::info("[BookEditor] Cancel held back: unsaved changes");
                                ShowSavePrompt();
                                return RE::UI_MESSAGE_RESULTS::kHandled;
                            }
                            // Nothing to save: the book closes, and its SWF drops the editor
                            // (PrepForClose), so later hides must not look for changes.
                            LeaveEditMode();
                        }
                    }
                    // kIgnore keeps the menu open; kHandled would tell the UI the hide was done,
                    // and it removes the menu itself (found in game, 2026-09-28).
                    if (type == RE::UI_MESSAGE_TYPE::kHide) {
                        if (g_prompting) return RE::UI_MESSAGE_RESULTS::kIgnore;
                        if (HasChanges()) {
                            SKSE::log::info("[BookEditor] Close held back: unsaved changes");
                            ShowSavePrompt();
                            return RE::UI_MESSAGE_RESULTS::kIgnore;
                        }
                        // Letting it close: the menu sends another hide after its close
                        // animation, by when its SWF has dropped the editor.
                        LeaveEditMode();
                    }
                }
                return func(a_menu, a_message);
            }
            static inline REL::Relocation<decltype(thunk)> func;
        };

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
                    // The save prompt is open: its keys are its own.
                    if (g_prompting) continue;
                    if (g_active) {
                        // The book menu turns pages on the arrow keys, A and D by key code, not by
                        // user event, so it can't be kept from seeing them. Instead the SWF refuses
                        // turns while keys are in use: every event (press, held repeat, release)
                        // renews the block; a click never comes with one.
                        Invoke("EditSuppressTurn");
                        // Keys are text: blank the user event for anything that does go by it.
                        button->SetUserEvent("");
                    }
                    // The edit key while writing: save and go back to reading.
                    if (g_active && code == Config::GetSingleton()->GetEditKey()) {
                        if (button->IsDown()) SaveAndRead();
                        continue;
                    }
                    // The edit key while a book is open and not being edited: edit it, if it's
                    // one of the player's diaries (EnterEditMode checks).  The menu doesn't get
                    // the key.
                    if (!g_active && code == Config::GetSingleton()->GetEditKey() && button->IsDown()) {
                        auto* ui = RE::UI::GetSingleton();
                        if (ui && ui->IsMenuOpen(RE::BookMenu::MENU_NAME)) {
                            button->SetUserEvent("");
                            SKSE::GetTaskInterface()->AddUITask([]() { EnterEditMode(); });
                            continue;
                        }
                    }
                    if (code == kF5 && button->IsDown()) {
                        PreviewSave();
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
        REL::Relocation<std::uintptr_t> vtable{ RE::VTABLE_BookMenu[0] };
        BookMenuProcessMessage::func = vtable.write_vfunc(0x4, BookMenuProcessMessage::thunk);
        SKSE::log::info("[BookEditor] Registered: key 0x{:X} edits the player's diary while it's open",
                        Config::GetSingleton()->GetEditKey());
    }

} // namespace SkyrimNetDiaries::BookEditor
