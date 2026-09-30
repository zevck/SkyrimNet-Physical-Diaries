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
#include "EditorInternal.h"

#include "BookText.h"
#include "BookTextHook.h"
#include "Config.h"
#include "DiaryDB.h"
#include "Localization.h"
#include "WritingTools.h"

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

// The edit session: the SWF, keys, saving, prompts, ink and blood (docs/EDITING.md).  Writes to
// SkyrimNet are in EditorWrites.cpp, choosing and starting journals in EditorJournals.cpp.
namespace SkyrimNetDiaries::BookEditor {

    std::vector<DiaryEntry> EditedEntries() {
        std::vector<DiaryEntry> entries;
        entries.reserve(g_edit.size());
        for (const auto& e : g_edit) {
            if (e.IsNew() && e.entry.content.empty()) continue;
            entries.push_back(e.entry);
        }
        return entries;
    }

    // book.swf is its own movie, separate from bookmenu.swf.
    RE::GFxMovieView* BookMovie() {
        auto* ui = RE::UI::GetSingleton();
        auto bookMenu = ui ? ui->GetMenu<RE::BookMenu>() : nullptr;
        return bookMenu ? bookMenu->GetRuntimeData().book.get() : nullptr;
    }

    void Notify(const std::string& text) { RE::SendHUDMessage::ShowHUDMessage(text.c_str()); }

    namespace {
        // ---- Keys (DirectX scan codes) ----
        constexpr std::uint32_t kEscape = 0x01, kBackspace = 0x0E, kEnter = 0x1C, kDelete = 0xD3;
        constexpr std::uint32_t kLeft = 0xCB, kRight = 0xCD, kUp = 0xC8, kDown = 0xD0, kHome = 0xC7, kEnd = 0xCF;
        constexpr std::uint32_t kF4 = 0x3E, kF5 = 0x3F;
        // Modifiers only change other keys: Shift, Ctrl, Alt (left and right), Caps Lock.
        constexpr std::uint32_t kModifiers[] = { 0x2A, 0x36, 0x1D, 0x9D, 0x38, 0xB8, 0x3A };

        constexpr std::string_view kDatePlaceholder = "{Date}";

        // Held-key repeat, like a text box (input thread only).
        constexpr auto kKeyRepeatDelay = std::chrono::milliseconds(400);
        constexpr auto kKeyRepeatRate = std::chrono::milliseconds(50);
        std::uint32_t g_lastScanCode = 0;
        std::chrono::steady_clock::time_point g_lastKeyTime{};
        bool g_keyRepeating = false;

        // ---- Edit session (main thread: the book menu pauses the game) ----
        std::atomic<bool> g_active{ false };     // edit mode is on in the open book menu
        std::atomic<bool> g_prompting{ false };  // a prompt is open over the book: keys belong to it
        bool g_inked = false;                    // this writing session has used its ink
        bool g_blood = false;                    // this writing session is in blood (no ink)
        bool g_bloodChosen = false;              // the player chose blood: the next edit mode is in blood
        bool g_newEntryAfterPrompt = false;      // the blood prompt came from the new-entry key
        std::unordered_set<int> g_bloodPaid;     // entries (SessionKey) this session has bled for
        std::string g_actorName;
        int g_journal = 0;                       // the journal's number (its entries' tag)
        int g_nextLocalKey = 1;                  // new entries' local keys, until SkyrimNet gives an id

        void ShowNotice(const std::string& body);
        void StartNewEntry();
        void BeginWriting(bool a_newEntry);
        void ShowBloodPrompt();

        // Like EntryKey, but a new entry keeps its local key after its add returns an id mid-session.
        int SessionKey(const DiaryEntry& entry) { return entry.localKey != 0 ? -entry.localKey : entry.id; }

        // An entry's heading for the SWF: between the blood markers if it was begun in blood.
        std::string HeadingFor(const DiaryEntry& entry) {
            const std::string heading = EntryHeading(entry);
            if (!entry.bloodHeading || heading.empty()) return heading;
            return std::format("{}{}{}", kBloodOpen, heading, kBloodClose);
        }

        // ---- The book movie ----

        void Invoke(const char* function, const char* argument = nullptr) {
            auto* movie = BookMovie();
            if (!movie) return;
            RE::GFxValue arg;
            if (argument) arg.SetString(argument);
            movie->Invoke(std::format("_root.BookMenu_mc.{}", function).c_str(), nullptr, argument ? &arg : nullptr,
                          argument ? 1 : 0);
        }

        // DIAGNOSTIC: the SWF's own description of its display state (BookMenu.as DebugState).
        int g_keysLogged = 0;
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

        // Keys reach the menu blanked (InputSink), so no controls need turning off; the mouse
        // keeps the book's own page turns (left click previous, right click next).
        void SetTextInput(bool enabled) {
            if (auto* controls = RE::ControlMap::GetSingleton()) controls->AllowTextInput(enabled);
        }

        // ---- Loading ----

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
        // laid out as the book shows them (docs/EDITING.md#opening).
        bool SendDiaryContent(RE::GFxMovieView* movie) {
            g_edit.clear();
            g_bookFormId = 0;
            auto* book = RE::BookMenu::GetTargetForm();
            auto* manager = BookManager::GetSingleton();
            auto* vol = book && manager ? manager->GetBookForFormID(book->GetFormID()) : nullptr;
            if (!vol) return false;

            // Only the player's journal is editable: an NPC's diary holds that NPC's memories, and
            // the player's diary SkyrimNet's (docs/EDITING.md#writing-mode).
            if (vol->kind != VolumeKind::Written) {
                SKSE::log::info("[BookEditor] {} vol {} isn't the player's journal: not editable", vol->actorName,
                                vol->volumeNumber);
                return false;
            }
            if (!Database::CanWriteDiaries()) {
                SKSE::log::warn("[BookEditor] SkyrimNet can't save diary edits (needs public API v11): not editable");
                Notify(Localization::GetSingleton()->GetEditNeedsSkyrimNet());
                return false;
            }
            if (auto* ui = RE::UI::GetSingleton(); !ui || !ui->GameIsPaused()) {
                // Key handling assumes the paused book menu's main-thread input (Skyrim Souls
                // RE, for one, can unpause it).
                SKSE::log::warn("[BookEditor] The book menu doesn't pause the game: not editable");
                Notify(Localization::GetSingleton()->GetEditNeedsPause());
                return false;
            }
            if (!WritingTools::HasQuill()) {
                SKSE::log::info("[BookEditor] No quill: can't write");
                ShowNotice(Localization::GetSingleton()->GetEditNeedsQuill());
                return false;
            }
            if (!g_bloodChosen && !WritingTools::HasInk()) {
                SKSE::log::info("[BookEditor] No ink: offering blood");
                ShowBloodPrompt();
                return false;
            }

            std::vector<DiaryEntry> entries;
            const PendingWrites* pending = nullptr;
            if (auto it = g_pending.find(vol->bookFormId); it != g_pending.end() && !it->second.stale) {
                pending = &it->second;
                entries = pending->entries;
                SKSE::log::info("[BookEditor] Editing the saved text ({} write(s) still in SkyrimNet, {} unsaved)",
                                pending->count, pending->unsaved.size());
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
                EditedEntry edited{ entry, NormalizeLineBreaks(EditableEntryText(entry)),
                                    pending && pending->unsaved.contains(EntryKey(entry)) };
                if (!packed.empty()) packed += '\x1E';
                packed += HeadingFor(entry) + '\x1F' + edited.savedBody;
                ids += std::format("{}{}", ids.empty() ? "" : ",", entry.id);
                g_edit.push_back(std::move(edited));
            }
            auto* config = Config::GetSingleton();
            const std::string font = config->GetFontFace();
            const std::string title = Localization::GetSingleton()->FormatTitle(vol->actorName, vol->kind);
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
                g_edit.clear();
                return false;
            }
            g_bookFormId = vol->bookFormId;
            g_journal = vol->volumeNumber;
            g_actorName = vol->actorName;
            SKSE::log::info("[BookEditor] {} vol {}: {} entries loaded for editing (ids {})", vol->actorName,
                            vol->volumeNumber, entries.size(), ids);
            return true;
        }

        // ---- The editor's text ----

        // Each entry's text as the editor has it now, in page order; nullopt if the SWF can't
        // say or the count doesn't match g_edit.
        std::optional<std::vector<std::string>> ReadBodies() {
            auto* movie = BookMovie();
            RE::GFxValue result;
            if (!movie || !movie->Invoke("_root.BookMenu_mc.EditGetBodies", &result, nullptr, 0) ||
                !result.IsString()) {
                SKSE::log::error("[BookEditor] No text from the SWF");
                return std::nullopt;
            }
            std::vector<std::string> bodies;
            std::string_view all = result.GetString();
            if (all.empty() && g_edit.empty()) return bodies;  // every entry was torn out
            for (std::size_t start = 0;;) {
                const auto end = all.find('\x1E', start);
                bodies.emplace_back(all.substr(start, end == std::string_view::npos ? all.npos : end - start));
                if (end == std::string_view::npos) break;
                start = end + 1;
            }
            if (bodies.size() != g_edit.size()) {
                SKSE::log::error("[BookEditor] {} texts from the SWF for {} entries", bodies.size(), g_edit.size());
                return std::nullopt;
            }
            return bodies;
        }

        bool NeedsWrite(const EditedEntry& e, const std::string& body) { return e.unsaved || body != e.savedBody; }

        // DIAGNOSTIC: where an entry's text first differs from what was loaded.
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

        // DIAGNOSTIC (F5): what saving would do now, without writing anything.
        void PreviewSave() {
            const auto bodies = ReadBodies();
            if (!bodies) return;
            int changed = 0;
            for (std::size_t i = 0; i < bodies->size(); ++i) {
                const auto& body = (*bodies)[i];
                if (!NeedsWrite(g_edit[i], body)) continue;
                ++changed;
                SKSE::log::info("[BookEditor] save preview: entry {} (id {}) {}: {} -> {} chars | {}", i,
                                g_edit[i].entry.id, body.empty() ? "emptied (left as is)" : "changed (update)",
                                g_edit[i].savedBody.size(), body.size(), body.substr(0, 200));
            }
            SKSE::log::info("[BookEditor] save preview: {} of {} entries changed", changed, bodies->size());
        }

        // true: something to save; false: nothing; nullopt: the editor's text can't be read.
        std::optional<bool> HasChanges() {
            if (g_bookFormId == 0) return false;
            const auto bodies = ReadBodies();
            if (!bodies) return std::nullopt;
            bool changed = false;
            for (std::size_t i = 0; i < bodies->size(); ++i) {
                if (NeedsWrite(g_edit[i], (*bodies)[i])) {
                    LogDifference(i, g_edit[i].savedBody, (*bodies)[i]);
                    changed = true;
                }
            }
            return changed;
        }

        // ---- Saving ----

        enum class SaveResult { Nothing, Queued, Unreadable };

        // Queue the changed entries' writes (docs/EDITING.md#saving).  An emptied entry is
        // left as it is: removing one is tearing it out, which asks first.
        SaveResult Save() {
            if (g_bookFormId == 0) return SaveResult::Nothing;
            const auto bodies = ReadBodies();
            if (!bodies) return SaveResult::Unreadable;
            std::vector<WriteJob> jobs;
            bool emptied = false;
            for (std::size_t i = 0; i < bodies->size(); ++i) {
                auto& e = g_edit[i];
                const auto& body = (*bodies)[i];
                if (!NeedsWrite(e, body)) continue;
                // The text as SkyrimNet stores it, and the red ranges kept apart from it.
                auto [content, blood] = SplitBlood(body);
                if (content.empty()) {
                    // A new entry left empty is simply not written; an existing one is kept.
                    if (!e.IsNew() || g_addQueued.contains(e.entry.localKey)) emptied = true;
                    continue;
                }
                if (e.entry.id == 0 && e.entry.localKey == 0) {
                    SKSE::log::warn("[BookEditor] Entry {} has no SkyrimNet id: can't save it", i);
                    continue;
                }
                if (std::ranges::find(e.entry.tags, kPlayerWrittenTag) == e.entry.tags.end()) {
                    e.entry.tags.emplace_back(kPlayerWrittenTag);
                }
                if (JournalOf(e.entry) != g_journal) SetJournal(e.entry, g_journal);
                const bool anyBlood = !blood.empty() || e.entry.bloodHeading;
                const auto bloodTag = std::ranges::find(e.entry.tags, kBloodTag);
                if (!anyBlood && bloodTag != e.entry.tags.end()) {
                    e.entry.tags.erase(bloodTag);
                } else if (anyBlood && bloodTag == e.entry.tags.end()) {
                    e.entry.tags.emplace_back(kBloodTag);
                }
                std::string tags;
                for (const auto& tag : e.entry.tags) tags += (tags.empty() ? "" : ",") + tag;
                e.entry.content = content;
                e.entry.blood = blood;
                e.savedBody = body;
                e.unsaved = false;
                // A new entry's first save adds it; later ones update it (by local key until
                // the add's id is back).
                const bool add = e.IsNew() && g_addQueued.insert(e.entry.localKey).second;
                jobs.push_back({ .kind = add ? WriteJob::Kind::Add : WriteJob::Kind::Update,
                                 .entryId = e.entry.id,
                                 .localKey = e.entry.localKey,
                                 .entryDate = e.entry.entry_date,
                                 .text = std::move(content),
                                 .tagsCSV = std::move(tags),
                                 .blood = std::move(blood),
                                 .bloodHeading = e.entry.bloodHeading });
            }
            if (emptied) Notify(Localization::GetSingleton()->GetEditEmptiedHint());
            if (jobs.empty()) return SaveResult::Nothing;
            QueueWrites(std::move(jobs));
            return SaveResult::Queued;
        }

        // ---- Edit mode ----

        void EnterEditMode() {
            auto* movie = BookMovie();
            if (!movie) {
                SKSE::log::warn("[BookEditor] No book movie to edit");
                return;
            }
            if (!SendDiaryContent(movie)) {
                g_bloodChosen = false;
                return;
            }
            // False when the loaded book.swf isn't ours (another mod's won): no edit mode.
            if (!movie->Invoke("_root.BookMenu_mc.EnterEditMode", nullptr, nullptr, 0)) {
                SKSE::log::warn("[BookEditor] book.swf has no EnterEditMode: it isn't SNPD's (check the load order)");
                g_bloodChosen = false;
                return;
            }
            g_active = true;
            g_inked = false;
            g_blood = std::exchange(g_bloodChosen, false);
            g_bloodPaid.clear();
            if (g_blood) {
                RE::GFxValue on;
                on.SetBoolean(true);
                movie->Invoke("_root.BookMenu_mc.EditSetBlood", nullptr, &on, 1);
                SKSE::log::info("[BookEditor] Writing in blood");
            }
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

        // Save can't read the editor's text: say so and keep writing, rather than lose it.
        bool SavedOrStay() {
            if (Save() != SaveResult::Unreadable) return true;
            Notify(Localization::GetSingleton()->GetEditSaveFailed());
            return false;
        }

        // The edit key while writing: save, and read again on the same spread.
        void SaveAndRead() {
            if (!SavedOrStay()) return;
            auto* movie = BookMovie();
            const std::string text =
                BookTextHook::ForBookMenu(FormatDiaryEntries(EditedEntries(), g_actorName, VolumeKind::Written));
            LeaveEditMode();
            RE::GFxValue arg;
            arg.SetString(text.c_str());
            if (!movie || !movie->Invoke("_root.BookMenu_mc.ReturnToReading", nullptr, &arg, 1)) {
                SKSE::log::warn("[BookEditor] Couldn't return to reading: closing the book");
                RequestClose();
            }
        }

        // ---- Prompts over the book ----

        void ShowPrompt(const std::string& body, std::initializer_list<const std::string*> buttons,
                        std::int32_t cancelButton, RE::IMessageBoxCallback* callback) {
            auto* data = RE::UIMessageDataFactory::Create<RE::MessageBoxData>();
            if (!data) return;
            data->bodyText = body.c_str();
            for (const auto* button : buttons) data->buttonText.push_back(button->c_str());
            data->cancelButtonIndex = cancelButton;  // Escape on the prompt
            data->callback = RE::BSTSmartPointer<RE::IMessageBoxCallback>(callback);
            // The prompt's own keys (Enter, Escape) must reach it.
            g_prompting = true;
            SetTextInput(false);
            RE::MessageBoxMenu::QueueMessage(data);
        }

        void EndPrompt() {
            g_prompting = false;
            if (g_active) SetTextInput(true);
        }

        // An OK-only message box over the book.
        class NoticeCallback : public RE::IMessageBoxCallback {
        public:
            void Run(std::uint8_t) override { EndPrompt(); }
        };

        void ShowNotice(const std::string& body) {
            ShowPrompt(body, { &Localization::GetSingleton()->GetEditOk() }, 0, new NoticeCallback());
        }


        // The entry the caret is in (page order), if any.
        std::optional<std::size_t> CaretEntry() {
            auto* movie = BookMovie();
            RE::GFxValue result;
            if (!movie || !movie->Invoke("_root.BookMenu_mc.EditCurrentEntry", &result, nullptr, 0) ||
                !result.IsNumber() || result.GetNumber() < 0) {
                return std::nullopt;
            }
            const auto index = static_cast<std::size_t>(result.GetNumber());
            return index < g_edit.size() ? std::optional(index) : std::nullopt;
        }

        // Writing in blood: the first change to each entry costs this much of the player's health,
        // unless it would leave them with less than kMinHealthAfterBleeding.
        constexpr float kBloodCost = 0.1f;
        constexpr float kMinHealthAfterBleeding = 1.0f;

        // docs/EDITING.md#writing-in-blood
        bool BledFor(std::size_t index) {
            const int key = SessionKey(g_edit[index].entry);
            if (g_bloodPaid.contains(key)) return true;
            auto* player = RE::PlayerCharacter::GetSingleton();
            auto* health = player ? player->AsActorValueOwner() : nullptr;
            if (!health) return false;
            const float cost = health->GetPermanentActorValue(RE::ActorValue::kHealth) * kBloodCost;
            if (health->GetActorValue(RE::ActorValue::kHealth) - cost < kMinHealthAfterBleeding) {
                SKSE::log::info("[BookEditor] Too weak to write in blood");
                ShowNotice(Localization::GetSingleton()->GetEditTooWeak());
                return false;
            }
            health->DamageActorValue(RE::ActorValue::kHealth, cost);
            g_bloodPaid.insert(key);
            SKSE::log::info("[BookEditor] Bled {:.0f} health for entry {}", cost, key);
            return true;
        }

        enum class Change { Type, EraseBack, EraseForward };

        // Ink once per session, or blood once per entry, for a key that changes the text
        // (docs/EDITING.md#quill-and-ink).  False: the key does nothing.
        bool CanWrite(Change change = Change::Type) {
            const auto index = CaretEntry();
            if (!index) return false;
            if (change != Change::Type) {
                RE::GFxValue forward;
                forward.SetBoolean(change == Change::EraseForward);
                RE::GFxValue can;
                if (!BookMovie()->Invoke("_root.BookMenu_mc.EditCanErase", &can, &forward, 1) || !can.IsBool() ||
                    !can.GetBool()) {
                    return false;
                }
            }
            if (g_blood) return BledFor(*index);
            if (g_inked) return true;
            auto* loc = Localization::GetSingleton();
            switch (WritingTools::UseInk()) {
            case WritingTools::Ink::None:
                // Checked when writing started; with the menu pausing the game it can't be gone.
                SKSE::log::warn("[BookEditor] The ink is gone: can't write");
                ShowNotice(loc->GetEditNeedsQuill());
                return false;
            case WritingTools::Ink::RanDry:
                Notify(loc->GetEditInkRanDry());
                break;
            case WritingTools::Ink::Used:
                break;
            }
            g_inked = true;
            return true;
        }

        // Save / Discard / Keep writing.
        class SavePromptCallback : public RE::IMessageBoxCallback {
        public:
            void Run(std::uint8_t a_button) override {
                EndPrompt();
                if (!g_active) return;
                if (a_button == 0) {
                    if (SavedOrStay()) CloseBook();
                } else if (a_button == 1) {
                    SKSE::log::info("[BookEditor] Changes discarded");
                    CloseBook();
                }
            }
        };

        void ShowSavePrompt() {
            auto* loc = Localization::GetSingleton();
            ShowPrompt(loc->GetEditSavePrompt(), { &loc->GetEditSave(), &loc->GetEditDiscard(), &loc->GetEditKeepWriting() },
                       2, new SavePromptCallback());
        }

        // ---- Tearing out an entry ----

        // Delete entry i (page order) now: from the editor, the book and SkyrimNet (the entry
        // and its memory).  Other entries' unsaved changes stay in the editor.
        void TearOut(std::size_t i) {
            if (i >= g_edit.size()) return;
            const int id = g_edit[i].entry.id;
            const int localKey = g_edit[i].entry.localKey;
            if (id == 0 && localKey == 0) return;
            RE::GFxValue arg;
            arg.SetNumber(static_cast<double>(i));
            auto* movie = BookMovie();
            RE::GFxValue removed;
            if (!movie || !movie->Invoke("_root.BookMenu_mc.EditRemoveEntry", &removed, &arg, 1) ||
                !removed.IsBool() || !removed.GetBool()) {
                SKSE::log::warn("[BookEditor] The SWF couldn't remove entry {}", i);
                return;
            }
            g_edit.erase(g_edit.begin() + static_cast<std::ptrdiff_t>(i));
            // The title page's date range follows the entries left.
            const std::string dates = TitlePageDates(EditedEntries());
            RE::GFxValue datesArg;
            datesArg.SetString(dates.c_str());
            movie->Invoke("_root.BookMenu_mc.EditSetDates", nullptr, &datesArg, 1);
            if (id == 0 && !g_addQueued.contains(localKey)) {
                SKSE::log::info("[BookEditor] Discarded a new entry that was never saved");
                return;  // never reached SkyrimNet
            }
            SKSE::log::info("[BookEditor] Tearing out entry {} (local {})", id, localKey);
            QueueWrites({ WriteJob{ .kind = WriteJob::Kind::Delete, .entryId = id, .localKey = localKey } });
        }

        class TearOutCallback : public RE::IMessageBoxCallback {
        public:
            explicit TearOutCallback(std::size_t a_entry) : entry_(a_entry) {}
            void Run(std::uint8_t a_button) override {
                EndPrompt();
                if (g_active && a_button == 0) TearOut(entry_);
            }

        private:
            std::size_t entry_;
        };

        // The delete key while writing: ask about the entry under the caret.
        void ShowTearOutPrompt() {
            const auto entry = CaretEntry();
            if (!entry) return;  // the caret isn't in an entry
            auto* loc = Localization::GetSingleton();
            std::string body = loc->GetEditDeletePrompt();
            if (const auto at = body.find(kDatePlaceholder); at != std::string::npos) {
                body.replace(at, kDatePlaceholder.size(), EntryDate(g_edit[*entry].entry));
            }
            ShowPrompt(body, { &loc->GetEditDelete(), &loc->GetEditKeep() }, 1, new TearOutCallback(*entry));
        }

        // While writing: a new, empty entry at the end of this journal, caret in it.
        void AppendNewEntry() {
            auto* manager = BookManager::GetSingleton();
            auto* vol = manager->GetBookForFormID(g_bookFormId);
            auto* movie = BookMovie();
            if (!vol || !movie) return;
            // One new entry at a time: another press goes back to the one not saved yet.
            for (std::size_t i = g_edit.size(); i-- > 0;) {
                if (g_edit[i].IsNew() && !g_addQueued.contains(g_edit[i].entry.localKey)) {
                    RE::GFxValue arg;
                    arg.SetNumber(static_cast<double>(i));
                    movie->Invoke("_root.BookMenu_mc.EditFocusEntry", nullptr, &arg, 1);
                    return;
                }
            }
            if (g_edit.size() >= static_cast<std::size_t>(Config::GetSingleton()->GetEntriesPerVolume())) {
                Notify(Localization::GetSingleton()->GetEditJournalFull());
                return;
            }
            DiaryEntry entry;
            entry.localKey = g_nextLocalKey++;
            entry.actor_uuid = vol->actorUuid;
            entry.entry_date = CurrentGameTimeSeconds();
            SetJournal(entry, vol->volumeNumber);
            entry.bloodHeading = g_blood;
            const std::string heading = HeadingFor(entry);
            RE::GFxValue arg;
            arg.SetString(heading.c_str());
            RE::GFxValue index;
            if (!movie->Invoke("_root.BookMenu_mc.EditAppendEntry", &index, &arg, 1) || !index.IsNumber() ||
                static_cast<std::size_t>(index.GetNumber()) != g_edit.size()) {
                SKSE::log::warn("[BookEditor] The SWF couldn't add a new entry");
                return;
            }
            g_edit.push_back({ entry, std::string(), false });
            SKSE::log::info("[BookEditor] New entry (local {}) in {} vol {}", entry.localKey, vol->actorName,
                            vol->volumeNumber);
        }

        // The new-entry key while reading (or a journal just opened by it): write a new entry in
        // this journal, if it has room (docs/EDITING.md#new-entries).
        void StartNewEntry() {
            auto* book = RE::BookMenu::GetTargetForm();
            auto* manager = BookManager::GetSingleton();
            auto* vol = book ? manager->GetBookForFormID(book->GetFormID()) : nullptr;
            if (!vol || vol->kind != VolumeKind::Written) return;
            if (JournalFull(*vol)) {
                Notify(Localization::GetSingleton()->GetEditJournalFull());
                return;
            }
            BeginWriting(true);
        }

        // Writing starts (the edit key, the new-entry key): a quill, then ink or the blood prompt
        // (SendDiaryContent), then edit mode, and a new entry if the new-entry key asked.
        void BeginWriting(bool a_newEntry) {
            g_newEntryAfterPrompt = a_newEntry;
            EnterEditMode();
            // A journal with no entries has nowhere to type: writing in it is a new entry.
            if (g_active && (a_newEntry || g_edit.empty())) AppendNewEntry();
        }

        // Write in blood / Put the quill down: the player has a quill but no ink.
        class BloodCallback : public RE::IMessageBoxCallback {
        public:
            void Run(std::uint8_t a_button) override {
                EndPrompt();
                const bool newEntry = std::exchange(g_newEntryAfterPrompt, false);
                if (a_button != 0) return;
                g_bloodChosen = true;
                BeginWriting(newEntry);
            }
        };

        void ShowBloodPrompt() {
            auto* loc = Localization::GetSingleton();
            ShowPrompt(loc->GetEditBloodPrompt(), { &loc->GetEditBloodYes(), &loc->GetEditBloodNo() }, 1,
                       new BloodCallback());
        }

        // ---- Menu and input ----

        class MenuSink : public RE::BSTEventSink<RE::MenuOpenCloseEvent> {
        public:
            RE::BSEventNotifyControl ProcessEvent(const RE::MenuOpenCloseEvent* a_event,
                                                  RE::BSTEventSource<RE::MenuOpenCloseEvent>*) override {
                if (!a_event || a_event->menuName != RE::BookMenu::MENU_NAME) return RE::BSEventNotifyControl::kContinue;
                if (a_event->opening) {
                    SKSE::GetTaskInterface()->AddUITask([]() { NoteBlankJournal(); });
                    return RE::BSEventNotifyControl::kContinue;
                }
                LeaveEditMode();
                g_newEntryOnOpen = 0;
                g_blankOnOpen = 0;
                return RE::BSEventNotifyControl::kContinue;
            }
        };

        // Every frame the book menu is open: once a book waiting for a new entry, or a blank journal,
        // has its text (SetBookText comes after the menu opens), start the entry or convert it.
        struct BookMenuAdvanceMovie {
            static void thunk(RE::IMenu* a_menu, float a_interval, std::uint32_t a_currentTime) {
                func(a_menu, a_interval, a_currentTime);
                const RE::FormID waiting = g_blankOnOpen ? g_blankOnOpen : g_newEntryOnOpen;
                if (waiting == 0 || g_active) return;
                auto* book = RE::BookMenu::GetTargetForm();
                auto* movie = BookMovie();
                if (!book || book->GetFormID() != waiting || !movie) return;
                RE::GFxValue ready;
                if (movie->Invoke("_root.BookMenu_mc.EditReady", &ready, nullptr, 0) && ready.IsBool() &&
                    ready.GetBool()) {
                    if (std::exchange(g_blankOnOpen, 0)) {
                        SKSE::GetTaskInterface()->AddUITask([]() { ConvertBlankJournal(); });
                    } else {
                        g_newEntryOnOpen = 0;
                        SKSE::GetTaskInterface()->AddUITask([]() { StartNewEntry(); });
                    }
                }
            }
            static inline REL::Relocation<decltype(thunk)> func;
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
                // One press closes the book (text input would swallow it); the close hook
                // asks first if there are unsaved changes.
                RequestClose();
                return;
            }
            static const std::unordered_map<std::uint32_t, const char*> kCursorKeys{
                { kLeft, "left" }, { kRight, "right" }, { kUp, "up" }, { kDown, "down" }, { kHome, "home" }, { kEnd, "end" },
            };
            if (const auto key = kCursorKeys.find(scanCode); key != kCursorKeys.end()) {
                Invoke("EditMoveCursor", key->second);
                return;
            }
            if (scanCode == kBackspace) {
                if (CanWrite(Change::EraseBack)) Invoke("EditBackspace");
                return;
            }
            if (scanCode == kDelete) {
                if (CanWrite(Change::EraseForward)) Invoke("EditDelete");
                return;
            }
            if (scanCode == kEnter) {
                if (CanWrite()) Invoke("AppendEditChar", "\n");
                return;
            }

            // The characters for this key with the active keyboard layout and modifiers (a dead
            // key that doesn't combine gives two).
            BYTE keyState[256] = {};
            GetKeyboardState(keyState);
            WCHAR chars[8] = {};
            const auto vk = MapVirtualKeyW(scanCode, MAPVK_VSC_TO_VK);
            const int count = ToUnicode(vk, scanCode, keyState, chars, 8, 0);
            std::wstring typed;
            for (int i = 0; i < count; ++i) {
                if (chars[i] >= 32) typed += chars[i];
            }
            if (typed.empty()) return;
            char utf8[32] = {};
            if (WideCharToMultiByte(CP_UTF8, 0, typed.c_str(), static_cast<int>(typed.size()), utf8, sizeof(utf8) - 1,
                                    nullptr, nullptr) > 0 &&
                CanWrite()) {
                Invoke("AppendEditChar", utf8);
            }
        }

        // Every way of closing the book reaches the menu here: with unsaved changes it's held back
        // and the save prompt opens (docs/EDITING.md#closing).
        struct BookMenuProcessMessage {
            // Hold back a close?  If not, edit mode ends now: the menu's second hide, after its close
            // animation, comes when the SWF has dropped the editor.
            static bool HoldClose() {
                if (g_prompting) return true;
                const auto changes = HasChanges();
                if (changes.value_or(false)) {
                    ShowSavePrompt();
                    return true;
                }
                if (!changes) SKSE::log::error("[BookEditor] Can't read the editor's text: closing without saving");
                LeaveEditMode();
                return false;
            }

            static RE::UI_MESSAGE_RESULTS thunk(RE::IMenu* a_menu, RE::UIMessage& a_message) {
                const auto type = a_message.type.get();
                if (g_active) {
                    const auto* data = type == RE::UI_MESSAGE_TYPE::kUserEvent
                                           ? static_cast<RE::BSUIMessageData*>(a_message.data)
                                           : nullptr;
                    // DIAGNOSTIC: which messages each way of closing sends
                    if (type != RE::UI_MESSAGE_TYPE::kUpdate && type != RE::UI_MESSAGE_TYPE::kScaleformEvent &&
                        !(g_prompting && type == RE::UI_MESSAGE_TYPE::kHide)) {
                        SKSE::log::info("[BookEditor] book menu message {} {}", std::to_underlying(type),
                                        data ? data->fixedStr.c_str() : "");
                    }
                    // A gamepad's B: the menu would play its close animation before its hide.
                    if (data && data->fixedStr == "Cancel" && HoldClose()) return RE::UI_MESSAGE_RESULTS::kHandled;
                    if (type == RE::UI_MESSAGE_TYPE::kHide && HoldClose()) return RE::UI_MESSAGE_RESULTS::kIgnore;
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
                    if (!button || button->GetDevice() != RE::INPUT_DEVICE::kKeyboard) continue;
                    const auto code = button->GetIDCode();
                    if (g_prompting) continue;  // a prompt's keys are its own
                    auto* config = Config::GetSingleton();

                    if (!g_active) {
                        // The edit or new-entry key while a book is open: write in it, if it's
                        // one of the player's diaries (checked there).  The menu doesn't get it.
                        const bool edit = code == config->GetEditKey();
                        const bool newEntry = config->GetNewEntryKey() != 0 && code == config->GetNewEntryKey();
                        if ((edit || newEntry) && button->IsDown()) {
                            auto* ui = RE::UI::GetSingleton();
                            if (ui && ui->IsMenuOpen(RE::BookMenu::MENU_NAME)) {
                                button->SetUserEvent("");
                                if (edit) {
                                    SKSE::GetTaskInterface()->AddUITask([]() { BeginWriting(false); });
                                } else {
                                    SKSE::GetTaskInterface()->AddUITask([]() { StartNewEntry(); });
                                }
                            } else if (newEntry && ui && !ui->GameIsPaused()) {
                                // During play (no menu pausing the game): this runs on the input
                                // job, so the work goes to the game thread.
                                SKSE::GetTaskInterface()->AddTask([]() { NewEntryFromPlay(); });
                            }
                        }
                        continue;
                    }

                    // Writing (paused: the main thread).  The menu turns pages by key code, so the SWF
                    // refuses turns while keys are used; the blanked user event stops everything else.
                    Invoke("EditSuppressTurn");
                    button->SetUserEvent("");
                    if (code == config->GetDeleteKey()) {
                        if (button->IsDown()) ShowTearOutPrompt();
                        continue;
                    }
                    if (code == config->GetEditKey()) {
                        if (button->IsDown()) SaveAndRead();
                        continue;
                    }
                    if (config->GetNewEntryKey() != 0 && code == config->GetNewEntryKey()) {
                        if (button->IsDown()) AppendNewEntry();
                        continue;
                    }
                    if (code == kF5 && button->IsDown()) {
                        PreviewSave();
                        continue;
                    }
                    if (code == kF4 && button->IsDown()) {
                        DumpState("F4");
                        continue;
                    }
                    if (std::ranges::find(kModifiers, code) != std::end(kModifiers)) continue;
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
        BookMenuAdvanceMovie::func = vtable.write_vfunc(0x5, BookMenuAdvanceMovie::thunk);
        SKSE::log::info("[BookEditor] Registered: key 0x{:X} edits the player's journal while it's open",
                        Config::GetSingleton()->GetEditKey());
    }

    void Reset() {
        ++g_generation;
        g_pending.clear();
        g_edit.clear();
        g_addQueued.clear();
        g_bookFormId = 0;
        g_lastJournal = 0;
        g_newEntryOnOpen = 0;
        g_blankOnOpen = 0;
    }

} // namespace SkyrimNetDiaries::BookEditor
