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
#include "ActorLookup.h"
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
#include <condition_variable>
#include <deque>
#include <mutex>
#include <optional>
#include <thread>
#include <unordered_map>
#include <unordered_set>

namespace SkyrimNetDiaries::BookEditor {

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

        // One entry being edited: SkyrimNet's entry (with saved edits applied), the text the
        // editor was given or last saved (what "changed" compares against), and whether its
        // last save failed (then it's written again even if unchanged).
        struct EditedEntry {
            DiaryEntry entry;
            std::string savedBody;
            bool unsaved = false;

            // Added in the editor and not in SkyrimNet yet (no id): entry.localKey names it.
            bool IsNew() const { return entry.id == 0 && entry.localKey != 0; }
        };
        std::vector<EditedEntry> g_edit;  // page order, as the SWF's EditGetBodies
        std::string g_actorName;
        RE::FormID g_bookFormId = 0;      // the diary volume being edited

        // New entries: each gets a local key until SkyrimNet gives it an id (game thread).
        int g_nextLocalKey = 1;
        std::unordered_set<int> g_addQueued;  // local keys whose add is in the write queue

        // An entry's key in the pending-write bookkeeping: its id, or its local key (negated).
        int EntryKey(const DiaryEntry& entry) { return entry.id != 0 ? entry.id : -entry.localKey; }

        // The entries as the book shows them: a new entry that was never given text isn't one.
        std::vector<DiaryEntry> EditedEntries() {
            std::vector<DiaryEntry> entries;
            entries.reserve(g_edit.size());
            for (const auto& e : g_edit) {
                if (e.IsNew() && e.entry.content.empty()) continue;
                entries.push_back(e.entry);
            }
            return entries;
        }

        // ---- Writes to SkyrimNet ----

        // Writes SkyrimNet hasn't finished, per volume (game thread).  Until they are done
        // SkyrimNet has the old text, so editing that volume again starts from `entries`.
        // `unsaved`: entries whose update failed; they stay in `entries` and are written again
        // on the next save.  `stale`: a delete failed, so `entries` is missing an entry
        // SkyrimNet still has; dropped when the writes finish.
        struct PendingWrites {
            int count = 0;
            std::vector<DiaryEntry> entries;
            std::unordered_set<int> unsaved;
            bool stale = false;
        };
        std::unordered_map<RE::FormID, PendingWrites> g_pending;
        std::uint32_t g_generation = 0;  // bumped when a session ends (Reset)

        struct WriteJob {
            enum class Kind { Update, Delete, Add };
            Kind kind = Kind::Update;
            std::uint32_t generation = 0;
            RE::FormID bookFormId = 0;
            int entryId = 0;   // 0: a new entry, found by localKey once its add has run
            int localKey = 0;
            double entryDate = 0.0;  // Add only
            std::string text;
            std::string tagsCSV;
        };

        // Called on the game thread when a job is done.  An add's job carries its new id.
        void CompleteWrite(const WriteJob& job, bool ok) {
            if (job.generation != g_generation) {
                SKSE::log::info("[BookEditor] Write for entry {} finished after a load: ignored", job.entryId);
                return;
            }
            const bool isDelete = job.kind == WriteJob::Kind::Delete;
            const int key = job.entryId != 0 ? job.entryId : -job.localKey;
            auto* loc = Localization::GetSingleton();
            auto it = g_pending.find(job.bookFormId);
            if (job.kind == WriteJob::Kind::Add && ok) {
                // The new entry has its id now: later edits and deletes use it.
                const auto setId = [&job](DiaryEntry& entry) {
                    if (entry.localKey == job.localKey && entry.id == 0) entry.id = job.entryId;
                };
                for (auto& e : g_edit) setId(e.entry);
                if (it != g_pending.end()) {
                    for (auto& entry : it->second.entries) setId(entry);
                    it->second.unsaved.erase(-job.localKey);
                }
            } else if (job.kind == WriteJob::Kind::Add) {
                g_addQueued.erase(job.localKey);  // not in SkyrimNet: the next save adds it again
            }
            if (it != g_pending.end()) {
                auto& pending = it->second;
                --pending.count;
                if (isDelete) {
                    if (!ok) pending.stale = true;
                } else if (ok) {
                    pending.unsaved.erase(key);
                } else {
                    pending.unsaved.insert(key);
                }
            }
            if (!ok) {
                RE::SendHUDMessage::ShowHUDMessage(
                    (isDelete ? loc->GetEditDeleteFailed() : loc->GetEditSaveFailed()).c_str());
            }
            if (it == g_pending.end() || it->second.count > 0) return;  // the last write reconciles

            // Keep a refused save's text for the next edit; otherwise SkyrimNet has it all.
            if (it->second.unsaved.empty() || it->second.stale) g_pending.erase(it);
            auto* manager = BookManager::GetSingleton();
            if (auto* vol = manager->GetBookForFormID(job.bookFormId)) manager->ReconcileAfterWrite(*vol);
        }

        // One worker, so SkyrimNet gets the editor's writes in the order they were made (an
        // update blocks while SkyrimNet re-embeds the entry's memory).  It lives for the
        // process; each job's completion runs as a game-thread task.  A new entry's later
        // jobs name it by local key: its add ran first, and ids_ has the id it got.
        class WriteQueue {
        public:
            void Push(WriteJob job) {
                {
                    std::lock_guard lock(mutex_);
                    jobs_.push_back(std::move(job));
                    if (!started_) {
                        started_ = true;
                        std::thread([this]() { Run(); }).detach();
                    }
                }
                ready_.notify_one();
            }

        private:
            void Run() {
                for (;;) {
                    WriteJob job;
                    {
                        std::unique_lock lock(mutex_);
                        ready_.wait(lock, [this]() { return !jobs_.empty(); });
                        job = std::move(jobs_.front());
                        jobs_.pop_front();
                    }
                    bool ok = false;
                    try {
                        if (job.entryId == 0 && job.localKey != 0) {
                            if (const auto id = ids_.find(job.localKey); id != ids_.end()) job.entryId = id->second;
                        }
                        switch (job.kind) {
                        case WriteJob::Kind::Add:
                            job.entryId = Database::AddDiaryEntry(0x14, job.text, job.entryDate, job.tagsCSV);
                            ok = job.entryId != 0;
                            if (ok) ids_[job.localKey] = job.entryId;
                            break;
                        case WriteJob::Kind::Update:
                            // No id: the entry's add failed, so there's nothing to update.
                            ok = job.entryId != 0 && Database::UpdateDiaryEntry(job.entryId, job.text, job.tagsCSV);
                            break;
                        case WriteJob::Kind::Delete:
                            // No id: the entry's add failed, so there's nothing to delete.
                            ok = job.entryId == 0 || Database::DeleteDiaryEntry(job.entryId);
                            break;
                        }
                    } catch (...) {
                        ok = false;
                    }
                    static constexpr const char* kDone[] = { "Saved", "Tore out", "Added" };
                    static constexpr const char* kVerb[] = { "save", "delete", "add" };
                    const auto kind = static_cast<std::size_t>(job.kind);
                    if (ok) {
                        SKSE::log::info("[BookEditor] {} entry {}", kDone[kind], job.entryId);
                    } else {
                        SKSE::log::error("[BookEditor] SkyrimNet didn't {} entry {} (local {})", kVerb[kind],
                                         job.entryId, job.localKey);
                    }
                    SKSE::GetTaskInterface()->AddTask([job = std::move(job), ok]() { CompleteWrite(job, ok); });
                }
            }

            std::mutex mutex_;
            std::condition_variable ready_;
            std::deque<WriteJob> jobs_;
            bool started_ = false;
            std::unordered_map<int, int> ids_;  // worker only: local key → SkyrimNet id
        };
        WriteQueue g_writes;

        // Queue writes for the volume being edited, and show the edited entries in its book now.
        void QueueWrites(std::vector<WriteJob> jobs) {
            if (jobs.empty()) return;
            auto& pending = g_pending[g_bookFormId];
            pending.entries = EditedEntries();
            pending.count += static_cast<int>(jobs.size());
            for (const auto& e : g_edit) {
                if (!e.unsaved) pending.unsaved.erase(EntryKey(e.entry));
            }
            auto* manager = BookManager::GetSingleton();
            if (auto* vol = manager->GetBookForFormID(g_bookFormId)) manager->SetVolumeText(*vol, pending.entries);
            for (auto& job : jobs) {
                job.generation = g_generation;
                job.bookFormId = g_bookFormId;
                g_writes.Push(std::move(job));
            }
        }

        // ---- The book movie ----

        // book.swf is its own movie, separate from bookmenu.swf.
        RE::GFxMovieView* BookMovie() {
            auto* ui = RE::UI::GetSingleton();
            auto bookMenu = ui ? ui->GetMenu<RE::BookMenu>() : nullptr;
            return bookMenu ? bookMenu->GetRuntimeData().book.get() : nullptr;
        }

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

        void Notify(const std::string& text) { RE::SendHUDMessage::ShowHUDMessage(text.c_str()); }

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
                packed += EntryHeading(entry) + '\x1F' + edited.savedBody;
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
                if (body.empty()) {
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
                std::string tags;
                for (const auto& tag : e.entry.tags) tags += (tags.empty() ? "" : ",") + tag;
                e.entry.content = body;
                e.savedBody = body;
                e.unsaved = false;
                // A new entry's first save adds it; later ones update it (by local key until
                // the add's id is back).
                const bool add = e.IsNew() && g_addQueued.insert(e.entry.localKey).second;
                jobs.push_back({ .kind = add ? WriteJob::Kind::Add : WriteJob::Kind::Update,
                                 .entryId = e.entry.id,
                                 .localKey = e.entry.localKey,
                                 .entryDate = e.entry.entry_date,
                                 .text = body,
                                 .tagsCSV = std::move(tags) });
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
            if (!SendDiaryContent(movie)) return;
            // False when the loaded book.swf isn't ours (another mod's won): no edit mode.
            if (!movie->Invoke("_root.BookMenu_mc.EnterEditMode", nullptr, nullptr, 0)) {
                SKSE::log::warn("[BookEditor] book.swf has no EnterEditMode: it isn't SNPD's (check the load order)");
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
            auto* movie = BookMovie();
            RE::GFxValue result;
            if (!movie || !movie->Invoke("_root.BookMenu_mc.EditCurrentEntry", &result, nullptr, 0) ||
                !result.IsNumber() || result.GetNumber() < 0) {
                return;  // the caret isn't in an entry
            }
            const auto entry = static_cast<std::size_t>(result.GetNumber());
            if (entry >= g_edit.size()) return;
            auto* loc = Localization::GetSingleton();
            std::string body = loc->GetEditDeletePrompt();
            if (const auto at = body.find(kDatePlaceholder); at != std::string::npos) {
                body.replace(at, kDatePlaceholder.size(), EntryDate(g_edit[entry].entry));
            }
            ShowPrompt(body, { &loc->GetEditDelete(), &loc->GetEditKeep() }, 1, new TearOutCallback(entry));
        }

        // ---- New entries (docs/EDITING.md#new-entries) ----

        // How far after a volume's start its first new entry is dated, in game seconds.
        constexpr double kInsideStart = 0.5;

        RE::FormID g_openNext = 0;        // a new volume to open once this book has closed
        RE::FormID g_newEntryOnOpen = 0;  // start a new entry once this book is open with its text

        bool IsLatestVolume(const DiaryBookData& vol) {
            const auto* latest = BookManager::GetSingleton()->GetBookForActor(vol.actorUuid, vol.kind);
            return latest && latest->bookFormId == vol.bookFormId;
        }

        // The volume after `vol` (full, or not held), empty, in the player's inventory.  Its
        // book's FormID, or 0.  `entries`: vol's, saved.
        RE::FormID CreateNextVolume(DiaryBookData& vol, const std::vector<DiaryEntry>& entries) {
            auto* manager = BookManager::GetSingleton();
            // The menu pauses game time, so entries just written here are dated now.  They stay
            // in this volume: its endTime covers them (a newer volume bounds it by endTime), and
            // the next volume starts on the next whole game second, clear of them.
            double end = vol.endTime;
            for (const auto& e : entries) end = std::max(end, e.entry_date);
            if (end != vol.endTime) manager->UpdateBookEndTime(vol, end);
            const double start = std::floor(std::max(CurrentGameTimeSeconds(), end)) + 1.0;
            const double lastCreation = entries.empty() ? 0.0 : entries.back().creation_time;
            // Copies: creating a volume adds to the list `vol` lives in.
            const std::string uuid = vol.actorUuid, name = vol.actorName, bio = vol.bioTemplateName;
            const int number = vol.volumeNumber + 1;
            const RE::FormID next = manager->CreateEmptyVolume(uuid, name, start, number, 0x14, bio, lastCreation, 0);
            if (next == 0) {
                SKSE::log::error("[BookEditor] Couldn't make volume {} of {}", number, name);
            } else {
                SKSE::log::info("[BookEditor] Made volume {} (0x{:X})", number, next);
            }
            return next;
        }

        // The volume is full: make the next one, close this book and open that one with a new
        // entry.  `entries`: this volume's, saved.
        void StartNextVolume(DiaryBookData& vol, const std::vector<DiaryEntry>& entries) {
            const RE::FormID next = CreateNextVolume(vol, entries);
            if (next == 0) return;
            Notify(Localization::GetSingleton()->GetEditNewVolume());
            g_openNext = next;
            CloseBook();
        }

        // Open a book and start a new entry in it once its text is in (BookMenuAdvanceMovie).
        // Game thread, no book menu open.
        void OpenForNewEntry(RE::FormID bookFormId) {
            auto* book = RE::TESForm::LookupByID<RE::TESObjectBOOK>(bookFormId);
            if (!book) return;
            g_newEntryOnOpen = bookFormId;
            RE::BookMenu::OpenMenuFromBaseForm(book);
        }

        // The new-entry key during play (docs/EDITING.md#outside-the-book): open the player's
        // latest volume with a new entry, or start the next volume if it's full or the player
        // doesn't carry it (as when an NPC's diary is taken), or volume 1 if there's none.
        void NewEntryFromPlay() {
            auto* loc = Localization::GetSingleton();
            if (!Database::CanWriteDiaries()) {
                Notify(loc->GetEditNeedsSkyrimNet());
                return;
            }
            const std::string uuid = Database::GetUUIDFromFormID(0x14);
            auto* player = RE::PlayerCharacter::GetSingleton();
            if (uuid.empty() || !player) return;
            auto* manager = BookManager::GetSingleton();
            auto* latest = manager->GetBookForActor(uuid, VolumeKind::Written);
            RE::FormID target = 0;
            if (!latest) {
                std::string name = Database::GetActorName(uuid);
                if (name.empty()) name = player->GetName();
                target = manager->CreateEmptyVolume(uuid, name, 0.0, 1, 0x14, Database::GetTemplateNameByUUID(uuid),
                                                    0.0, 0);
                if (target) Notify(loc->GetEditStartedVolume());
            } else {
                bool ok = false;
                const auto entries = manager->GetShownEntries(*latest, &ok);
                if (!ok) return;
                const auto* book = RE::TESForm::LookupByID<RE::TESBoundObject>(latest->bookFormId);
                if (CountInInventory(player, book) <= 0) {
                    SKSE::log::info("[BookEditor] The player doesn't carry volume {}: starting the next",
                                    latest->volumeNumber);
                    target = CreateNextVolume(*latest, entries);
                    if (target) Notify(loc->GetEditStartedVolume());
                } else if (static_cast<int>(entries.size()) >= Config::GetSingleton()->GetEntriesPerVolume()) {
                    target = CreateNextVolume(*latest, entries);
                    if (target) Notify(loc->GetEditNewVolume());
                } else {
                    target = latest->bookFormId;
                }
            }
            if (target) OpenForNewEntry(target);
        }

        // While writing: a new, empty entry at the end of this (latest) volume, caret in it.
        void AppendNewEntry() {
            auto* manager = BookManager::GetSingleton();
            auto* vol = manager->GetBookForFormID(g_bookFormId);
            auto* movie = BookMovie();
            if (!vol || !movie) return;
            if (!IsLatestVolume(*vol)) {
                Notify(Localization::GetSingleton()->GetEditNotLatest());
                return;
            }
            // One new entry at a time: another press goes back to the one not saved yet.
            for (std::size_t i = g_edit.size(); i-- > 0;) {
                if (g_edit[i].IsNew() && !g_addQueued.contains(g_edit[i].entry.localKey)) {
                    RE::GFxValue arg;
                    arg.SetNumber(static_cast<double>(i));
                    movie->Invoke("_root.BookMenu_mc.EditFocusEntry", nullptr, &arg, 1);
                    return;
                }
            }
            if (static_cast<int>(g_edit.size()) >= Config::GetSingleton()->GetEntriesPerVolume()) {
                if (!SavedOrStay()) return;
                StartNextVolume(*vol, EditedEntries());
                return;
            }
            DiaryEntry entry;
            entry.localKey = g_nextLocalKey++;
            entry.actor_uuid = vol->actorUuid;
            // Never on the volume's start: SkyrimNet truncates the date it stores (to its clock's
            // tick, then six decimals), and an entry a hair before the start is in no volume.
            entry.entry_date = vol->startTime > 0.0 ? std::max(CurrentGameTimeSeconds(), vol->startTime + kInsideStart)
                                                    : CurrentGameTimeSeconds();
            const std::string heading = EntryHeading(entry);
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

        // The new-entry key while reading (or a new volume just opened): write a new entry in the
        // player's latest volume, or start the next volume if it's full.
        void StartNewEntry() {
            auto* book = RE::BookMenu::GetTargetForm();
            auto* manager = BookManager::GetSingleton();
            auto* vol = book ? manager->GetBookForFormID(book->GetFormID()) : nullptr;
            if (!vol || vol->kind != VolumeKind::Written) return;
            if (!IsLatestVolume(*vol)) {
                Notify(Localization::GetSingleton()->GetEditNotLatest());
                return;
            }
            if (vol->lastKnownEntryCount >= Config::GetSingleton()->GetEntriesPerVolume()) {
                bool ok = false;
                const auto entries = manager->GetShownEntries(*vol, &ok);
                if (ok && static_cast<int>(entries.size()) >= Config::GetSingleton()->GetEntriesPerVolume()) {
                    if (!Database::CanWriteDiaries()) {
                        Notify(Localization::GetSingleton()->GetEditNeedsSkyrimNet());
                        return;
                    }
                    StartNextVolume(*vol, entries);
                    return;
                }
            }
            EnterEditMode();
            if (g_active) AppendNewEntry();
        }

        // ---- Menu and input ----

        class MenuSink : public RE::BSTEventSink<RE::MenuOpenCloseEvent> {
        public:
            RE::BSEventNotifyControl ProcessEvent(const RE::MenuOpenCloseEvent* a_event,
                                                  RE::BSTEventSource<RE::MenuOpenCloseEvent>*) override {
                if (!a_event || a_event->menuName != RE::BookMenu::MENU_NAME) return RE::BSEventNotifyControl::kContinue;
                if (a_event->opening) return RE::BSEventNotifyControl::kContinue;
                LeaveEditMode();
                // A full volume's new-entry key closed it: open the next volume, which starts a
                // new entry once its text is in (BookMenuAdvanceMovie).
                g_newEntryOnOpen = 0;
                if (const auto next = std::exchange(g_openNext, 0)) {
                    SKSE::GetTaskInterface()->AddTask([next]() { OpenForNewEntry(next); });
                }
                return RE::BSEventNotifyControl::kContinue;
            }
        };

        // Every frame the book menu is open: once a book waiting for a new entry has its text
        // (the engine's SetBookText comes after the menu opens), start the entry.
        struct BookMenuAdvanceMovie {
            static void thunk(RE::IMenu* a_menu, float a_interval, std::uint32_t a_currentTime) {
                func(a_menu, a_interval, a_currentTime);
                if (g_newEntryOnOpen == 0 || g_active) return;
                auto* book = RE::BookMenu::GetTargetForm();
                auto* movie = BookMovie();
                if (!book || book->GetFormID() != g_newEntryOnOpen || !movie) return;
                RE::GFxValue ready;
                if (movie->Invoke("_root.BookMenu_mc.EditReady", &ready, nullptr, 0) && ready.IsBool() &&
                    ready.GetBool()) {
                    g_newEntryOnOpen = 0;
                    SKSE::GetTaskInterface()->AddUITask([]() { StartNewEntry(); });
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
            if (scanCode == kBackspace) return Invoke("EditBackspace");
            if (scanCode == kDelete) return Invoke("EditDelete");
            if (scanCode == kEnter) return Invoke("AppendEditChar", "\n");

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
                                    nullptr, nullptr) > 0) {
                Invoke("AppendEditChar", utf8);
            }
        }

        // Every way of closing the book reaches the menu here (docs/EDITING.md#closing): with
        // unsaved changes, Cancel is consumed and kHide refused (kIgnore keeps the menu open;
        // kHandled would let the UI remove it) and the save prompt opens.  kForceHide passes.
        struct BookMenuProcessMessage {
            // Hold back a close?  With nothing to save (or text that can't be read), edit mode
            // ends: the menu sends another hide after its close animation, when the SWF has
            // already dropped the editor.
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
                                    SKSE::GetTaskInterface()->AddUITask([]() { EnterEditMode(); });
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

                    // Writing (the game is paused: this is the main thread).  The menu turns
                    // pages on the arrows, A and D by key code, so the SWF refuses turns while
                    // keys are in use; blanking the user event stops anything that goes by it.
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
        g_openNext = 0;
        g_newEntryOnOpen = 0;
    }

} // namespace SkyrimNetDiaries::BookEditor
