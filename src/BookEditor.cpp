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

#include "BlankJournals.h"
#include "BookText.h"
#include "BookTextHook.h"
#include "Config.h"
#include "Localization.h"
#include "WritingMode.h"

#include <numeric>

// The player's journals as an Ink & Quill client: sessions, marked text, saving, tearing out, the new-entry
// key (docs/EDITING.md).  Writes to SkyrimNet are in EditorWrites.cpp, journals in EditorJournals.cpp.
namespace SkyrimNetDiaries::BookEditor {

    std::vector<DiaryEntry> EditedEntries(int skip) {
        std::vector<DiaryEntry> entries;
        entries.reserve(g_edit.size());
        for (int i = 0; i < static_cast<int>(g_edit.size()); ++i) {
            const auto& e = g_edit[i];
            if (i != skip && !(e.IsNew() && e.entry.content.empty())) entries.push_back(e.entry);
        }
        return entries;
    }

    void Notify(const std::string& text) { RE::SendHUDMessage::ShowHUDMessage(text.c_str()); }

    namespace {
        // Ink & Quill holds at most one of our sessions (asking about blood, or writing); its `user` is the
        // session's number, so a callback from an older one is ignored.  0: none.
        std::uint32_t g_session = 0;
        std::uint32_t g_sessions = 0;
        std::string g_actorName;
        int g_journal = 0;        // the journal's number (its entries' tag)
        constexpr std::string_view kDatePlaceholder = "{Date}";
        int g_nextLocalKey = 1;   // new entries' local keys, until SkyrimNet gives an id
        std::string g_blankLook;  // a blank journal's session (its look): no journal until the first save
        bool g_asking = false;    // our tear-out question is open over the book: our keys wait
        int g_askedRun = -1;      // the entry it asks about

        void* UserOf(std::uint32_t session) { return reinterpret_cast<void*>(static_cast<std::uintptr_t>(session)); }
        bool IsCurrent(void* user) { return g_session != 0 && user == UserOf(g_session); }

        const IQ_API& API() { return *WritingMode::API(); }

        // "\r\n" and "\r" to "\n", as the editor hands runs back.
        std::string NormalizeLineBreaks(std::string_view text) {
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

        // An entry's run as the editor reads it back: FormatDiaryEntries' paragraphs between "\n\n", so an
        // untouched entry compares equal at a save.
        std::string RunText(const DiaryEntry& entry) {
            const std::string text = EditableEntryText(entry);
            std::string run;
            bool first = true;
            for (const auto paragraph : Paragraphs(text)) {
                run += (std::exchange(first, false) ? "" : "\n\n") + NormalizeLineBreaks(paragraph);
            }
            return run;
        }

        std::vector<DiaryEntry> SessionEntries() {
            std::vector<DiaryEntry> entries;
            entries.reserve(g_edit.size());
            for (const auto& e : g_edit) entries.push_back(e.entry);
            return entries;
        }

        std::string Marked(const std::vector<DiaryEntry>& entries) {
            return FormatDiaryEntries(entries, g_actorName, VolumeKind::Written, true);
        }

        // A new, empty entry dated now, in journal `journal` (0: a blank's, tagged when its first save makes it).
        DiaryEntry NewEntry(const std::string& actorUuid, int journal, bool blood) {
            DiaryEntry entry;
            entry.localKey = g_nextLocalKey++;
            entry.actor_uuid = actorUuid;
            entry.entry_date = CurrentGameTimeSeconds();
            entry.tags.emplace_back(kPlayerWrittenTag);
            if (journal > 0) SetJournal(entry, journal);
            entry.bloodHeading = blood;
            return entry;
        }

        // ---- While writing: structure changes ----

        // The session's entries with their text as the player has it now, unsaved changes included.
        std::optional<std::vector<DiaryEntry>> EntriesAsWritten() {
            std::vector<std::string> runs;
            const auto count = API().CurrentRuns(
                [](void* user, std::int32_t, const char* text) { static_cast<std::vector<std::string>*>(user)->emplace_back(text); },
                &runs);
            if (count < 0 || runs.size() != g_edit.size()) {
                SKSE::log::error("[BookEditor] {} runs from the editor for {} entries", count, g_edit.size());
                return std::nullopt;
            }
            auto entries = SessionEntries();
            for (std::size_t i = 0; i < entries.size(); ++i) {
                std::tie(entries[i].content, entries[i].blood) = SplitBlood(runs[i]);
            }
            return entries;
        }

        // The journal as the book menu reads it: the session's saved entries (a new one never given text isn't
        // one), without entry `skip`.
        std::string ReadingText(int skip = -1) {
            return BookTextHook::ForBookMenu(FormatDiaryEntries(EditedEntries(skip), g_actorName, VolumeKind::Written));
        }

        // `reading`: what the edit key goes back to if no run changes after this (a tear-out changes none).
        bool Reload(const std::vector<DiaryEntry>& entries, const std::string& reading, const std::vector<std::int32_t>& from,
                    int caretRun, int caretOffset) {
            const std::string marked = Marked(entries);
            if (API().Reload(marked.c_str(), reading.c_str(), from.data(), static_cast<std::int32_t>(from.size()), caretRun,
                             caretOffset)) {
                return true;
            }
            SKSE::log::error("[BookEditor] Ink & Quill couldn't reload the journal");
            return false;
        }

        // The new-entry key while writing: an entry at the end, caret in it.  One unsaved new entry at a time:
        // another press goes back to it.
        void AppendNewEntry() {
            auto* vol = BookManager::GetSingleton()->GetBookForFormID(g_bookFormId);
            auto entries = vol ? EntriesAsWritten() : std::nullopt;
            if (!entries) return;
            std::vector<std::int32_t> from(entries->size());
            std::iota(from.begin(), from.end(), 0);
            for (std::size_t i = g_edit.size(); i-- > 0;) {
                if (g_edit[i].IsNew() && !g_addQueued.contains(g_edit[i].entry.localKey)) {
                    Reload(*entries, ReadingText(), from, static_cast<int>(i), -1);
                    return;
                }
            }
            if (g_edit.size() >= static_cast<std::size_t>(Config::GetSingleton()->GetEntriesPerVolume())) {
                Notify(Localization::GetSingleton()->GetEditJournalFull());
                return;
            }
            const DiaryEntry entry = NewEntry(vol->actorUuid, vol->volumeNumber, API().InBlood());
            entries->push_back(entry);
            from.push_back(-1);
            if (!Reload(*entries, ReadingText(), from, static_cast<int>(entries->size()) - 1, -1)) return;
            g_edit.push_back({ entry, std::string(), false });
            SKSE::log::info("[BookEditor] New entry (local {}) in {} vol {}", entry.localKey, vol->actorName, vol->volumeNumber);
        }

        // ---- Ink & Quill's callbacks ----

        // Every way in from Ink & Quill or SKSE's task queues: an exception must not unwind into either.
        template <class F>
        void Guarded(const char* what, F&& body) {
            try {
                body();
            } catch (const std::exception& e) {
                SKSE::log::error("[BookEditor] {}: {}", what, e.what());
            } catch (...) {
                SKSE::log::error("[BookEditor] {}: unknown exception", what);
            }
        }

        // Queue the changed entries' writes (docs/EDITING.md#saving).  An emptied entry is left as it is:
        // removing one is tearing it out, which asks first.
        void OnSave(void* user, const char* const* runs, std::int32_t count, IQ_SaveReply* reply) {
            if (!IsCurrent(user)) return;  // no answer: refused
            auto* loc = Localization::GetSingleton();
            if (count < 0 || static_cast<std::size_t>(count) != g_edit.size()) {
                SKSE::log::error("[BookEditor] {} runs to save for {} entries", count, g_edit.size());
                API().ReplySave(reply, false, loc->GetEditSaveFailed().c_str(), nullptr);
                return;
            }
            // A blank journal's first save makes the journal (its only entry is the new one).
            const bool blank = !g_blankLook.empty();
            if (blank) {
                if (SplitBlood(runs[0] ? runs[0] : "").first.empty()) return;  // nothing to keep: refused
                const RE::FormID journal = StartJournal(g_blankLook);
                const auto* vol = journal ? BookManager::GetSingleton()->GetBookForFormID(journal) : nullptr;
                if (!vol) {
                    SKSE::log::error("[BookEditor] Couldn't start a journal from a blank journal ({})", g_blankLook);
                    API().ReplySave(reply, false, loc->GetBlankJournalFailed().c_str(), nullptr);
                    return;
                }
                g_blankLook.clear();
                g_bookFormId = journal;
                g_journal = vol->volumeNumber;
            }
            std::vector<WriteJob> jobs;
            bool emptied = false;
            for (std::size_t i = 0; i < g_edit.size(); ++i) {
                auto& e = g_edit[i];
                const std::string body = runs[i] ? runs[i] : "";
                if (!e.unsaved && body == e.savedBody) continue;
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
                if (!IsPlayerWritten(e.entry)) e.entry.tags.emplace_back(kPlayerWrittenTag);
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
                // A new entry's first save adds it; later ones update it (by local key until the add's id is back).
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
            if (jobs.empty() && emptied) {
                // Nothing else changed: writing goes on, and nothing is charged.
                API().ReplySave(reply, false, loc->GetEditEmptiedHint().c_str(), nullptr);
                return;
            }
            if (emptied) Notify(loc->GetEditEmptiedHint());
            if (jobs.empty()) SKSE::log::warn("[BookEditor] A save with no entry changed (the runs read back differently)");
            QueueWrites(std::move(jobs));
            const std::string text = ReadingText();
            if (!blank) {
                API().ReplySave(reply, true, "", text.c_str());
                return;
            }
            // Ink & Quill removes the blank and shows the journal in the open menu.
            API().ReplySaveAsBook(reply, g_bookFormId, text.c_str());
            Notify(loc->GetEditStartedVolume());
        }

        void OnDiscard(void* user) {
            if (IsCurrent(user)) SKSE::log::info("[BookEditor] Changes discarded");
        }

        // Tear out entry `run` now: from the editor, the book and SkyrimNet (the entry and its memory).  Other
        // entries' unsaved changes stay in the editor.
        void TearOut(std::int32_t run) {
            if (g_session == 0 || run < 0 || static_cast<std::size_t>(run) >= g_edit.size()) return;
            auto entries = EntriesAsWritten();
            if (!entries) return;
            entries->erase(entries->begin() + run);
            std::vector<std::int32_t> from(entries->size());
            for (std::int32_t i = 0; i < static_cast<std::int32_t>(from.size()); ++i) from[i] = i < run ? i : i + 1;
            // The caret ends the entry before it, or starts the one after.
            const bool before = run > 0;
            if (!Reload(*entries, ReadingText(run), from, before ? run - 1 : (entries->empty() ? -1 : 0), before ? -1 : 0)) return;
            const int id = g_edit[run].entry.id;
            const int localKey = g_edit[run].entry.localKey;
            g_edit.erase(g_edit.begin() + run);
            if (id == 0 && !g_addQueued.contains(localKey)) {
                SKSE::log::info("[BookEditor] Discarded a new entry that was never saved");
                return;  // never reached SkyrimNet
            }
            SKSE::log::info("[BookEditor] Tearing out entry {} (local {})", id, localKey);
            QueueWrites({ WriteJob{ .kind = WriteJob::Kind::Delete, .entryId = id, .localKey = localKey } });
        }

        // Ink & Quill calls it only while the session lasts (never after OnEnd).
        void TearOutAnswered(void* user, std::int32_t button) {
            g_asking = false;
            if (IsCurrent(user) && button == 0) TearOut(g_askedRun);
        }

        // The tear-out key while writing in a journal: ask about the entry under the caret.  A blank journal's
        // session has nothing to tear out yet.
        void AskTearOut() {
            if (g_session == 0 || g_asking || !g_blankLook.empty() || !API().IsWriting()) return;
            const auto run = API().CaretRun();
            if (run < 0 || static_cast<std::size_t>(run) >= g_edit.size()) return;
            auto* loc = Localization::GetSingleton();
            std::string question = loc->GetEditDeletePrompt();
            if (const auto at = question.find(kDatePlaceholder); at != std::string::npos) {
                question.replace(at, kDatePlaceholder.size(), EntryDate(g_edit[run].entry));
            }
            const char* buttons[] = { loc->GetEditDelete().c_str(), loc->GetEditKeep().c_str() };
            g_askedRun = run;
            const auto answered = [](void* u, std::int32_t button) { Guarded("Tearing out", [&] { TearOutAnswered(u, button); }); };
            g_asking = API().Prompt(question.c_str(), buttons, 2, 1, answered, UserOf(g_session));
        }

        void OnEnd(void* user) {
            if (!IsCurrent(user)) return;
            g_asking = false;
            g_session = 0;
            g_edit.clear();
            g_bookFormId = 0;
            g_blankLook.clear();
        }

        // ---- Starting ----

        // What one of our sessions holds, kept aside while a new one begins.
        struct SessionState {
            std::uint32_t session = 0;
            std::vector<EditedEntry> edit;
            RE::FormID book = 0;
            int journal = 0;
            std::string actorName;
            std::string blankLook;
        };

        SessionState TakeState() {
            return { std::exchange(g_session, 0),       std::exchange(g_edit, {}),      std::exchange(g_bookFormId, 0),
                     std::exchange(g_journal, 0),       std::exchange(g_actorName, {}), std::exchange(g_blankLook, {}) };
        }

        void PutState(SessionState state) {
            g_session = state.session;
            g_edit = std::move(state.edit);
            g_bookFormId = state.book;
            g_journal = state.journal;
            g_actorName = std::move(state.actorName);
            g_blankLook = std::move(state.blankLook);
        }

        // Ink & Quill's session over `next`, caret at the end of run `caretRun` (-1: the page being read).
        bool BeginSession(SessionState next, int caretRun) {
            auto previous = TakeState();
            next.session = ++g_sessions;
            PutState(std::move(next));
            auto* config = Config::GetSingleton();
            const std::string marked = Marked(SessionEntries());
            const std::string font = config->GetFontFace();
            IQ_Session session{};
            session.size = sizeof(IQ_Session);
            session.markedText = marked.c_str();
            session.runFont = font.c_str();
            session.runSize = config->GetFontSizeContent();
            session.caretRun = caretRun;
            session.user = UserOf(g_session);
            // Unanswered (an exception), a save is refused.
            session.onSave = [](void* u, const char* const* runs, std::int32_t count, IQ_SaveReply* reply) {
                Guarded("Saving", [&] { OnSave(u, runs, count, reply); });
            };
            session.onDiscard = [](void* u) { Guarded("Discarding", [&] { OnDiscard(u); }); };
            session.onEnd = [](void* u) { Guarded("Ending", [&] { OnEnd(u); }); };
            const bool begun = API().BeginSession(&session);
            // Refused (its OnEnd has run): Ink & Quill still has the one before, if there was one.
            if (!begun && g_session == 0) PutState(std::move(previous));
            return begun;
        }

        // Ink & Quill's session for this journal, with its entries as last saved, and a new one if asked (or
        // the journal is empty: nowhere else to type).
        bool Begin(DiaryBookData& vol, bool newEntry) {
            if (!Database::CanWriteDiaries()) {
                SKSE::log::warn("[BookEditor] SkyrimNet can't save diary edits (needs public API v11): not editable");
                Notify(Localization::GetSingleton()->GetEditNeedsSkyrimNet());
                return false;
            }
            std::vector<DiaryEntry> entries;
            const PendingWrites* pending = nullptr;
            if (auto it = g_pending.find(vol.bookFormId); it != g_pending.end() && !it->second.stale) {
                pending = &it->second;
                entries = pending->entries;
                SKSE::log::info("[BookEditor] Editing the saved text ({} write(s) still in SkyrimNet, {} unsaved)",
                                pending->count, pending->unsaved.size());
            } else {
                bool ok = false;
                entries = BookManager::GetSingleton()->GetShownEntries(vol, &ok);
                if (!ok) {
                    SKSE::log::warn("[BookEditor] {} vol {}: couldn't read the entries from SkyrimNet", vol.actorName,
                                    vol.volumeNumber);
                    return false;
                }
            }
            if (newEntry && entries.size() >= static_cast<std::size_t>(Config::GetSingleton()->GetEntriesPerVolume())) {
                Notify(Localization::GetSingleton()->GetEditJournalFull());
                return false;
            }
            SessionState next{ .book = vol.bookFormId, .journal = vol.volumeNumber, .actorName = vol.actorName };
            std::string ids;
            for (const auto& entry : entries) {
                next.edit.push_back({ entry, RunText(entry), pending && pending->unsaved.contains(EntryKey(entry)) });
                ids += std::format("{}{}", ids.empty() ? "" : ",", entry.id);
            }
            // Blood is chosen once the session starts: a new heading is red if it will be.
            const bool start = newEntry || next.edit.empty();
            if (start) next.edit.push_back({ NewEntry(vol.actorUuid, vol.volumeNumber, API().WouldBeInBlood()), {}, false });
            SKSE::log::info("[BookEditor] {} vol {}: {} entries to write in (ids {}){}", vol.actorName, vol.volumeNumber,
                            entries.size(), ids, start ? ", and a new one" : "");
            const int caret = start ? static_cast<int>(next.edit.size()) - 1 : -1;
            return BeginSession(std::move(next), caret);
        }

        // A blank journal read from the player's inventory (Ink & Quill checked where and the quill): writing in
        // a journal that doesn't exist yet, one new entry.  Its first save makes it (OnSave).
        bool OnBlankOpen(void*, std::uint32_t blankFormId) {
            if (!Database::CanWriteDiaries()) {
                Notify(Localization::GetSingleton()->GetEditNeedsSkyrimNet());
                return false;
            }
            const auto owner = PlayerJournalOwner();
            const std::string look = BlankJournals::LookOf(blankFormId);
            if (!owner || look.empty()) return false;
            SessionState next{ .actorName = owner->name, .blankLook = look };
            // Blood is chosen once the session starts: the heading is red if it will be.
            next.edit.push_back({ NewEntry(owner->uuid, 0, API().WouldBeInBlood()), {}, false });
            SKSE::log::info("[BookEditor] Blank journal 0x{:X} ({}): writing a new journal's first entry", blankFormId, look);
            return BeginSession(std::move(next), 0);
        }

        // The edit key (Ink & Quill's) on an open book: ours if it's one of the player's journals.  An NPC's
        // diary holds that NPC's memories, the player's diary SkyrimNet's (docs/EDITING.md#writing-mode).
        bool Owner(void*, std::uint32_t bookFormId) {
            auto* vol = BookManager::GetSingleton()->GetBookForFormID(bookFormId);
            if (!vol) return false;
            if (vol->kind != VolumeKind::Written) {
                SKSE::log::info("[BookEditor] {} vol {} isn't the player's journal: not editable", vol->actorName,
                                vol->volumeNumber);
                return false;
            }
            Begin(*vol, false);
            return true;
        }

        // The new-entry key in an open book: a new entry while writing, else writing in a new entry if it's one
        // of the player's journals with room (docs/EDITING.md#new-entries).
        void NewEntryInBook() {
            if (g_asking) return;
            if (API().IsWriting()) {
                if (g_session != 0) AppendNewEntry();
                return;
            }
            auto* book = RE::BookMenu::GetTargetForm();
            auto* vol = book ? BookManager::GetSingleton()->GetBookForFormID(book->GetFormID()) : nullptr;
            if (vol && vol->kind == VolumeKind::Written) Begin(*vol, true);
        }

        // ---- Menu and input ----

        // The tear-out and new-entry keys in an open book (the menu doesn't get them).  While writing, Ink &
        // Quill lets only registered keys through (RegisterKeys).
        class InputSink : public RE::BSTEventSink<RE::InputEvent*> {
        public:
            RE::BSEventNotifyControl ProcessEvent(RE::InputEvent* const* a_event,
                                                  RE::BSTEventSource<RE::InputEvent*>*) override {
                auto* config = Config::GetSingleton();
                const auto tearOut = config->GetDeleteKey();
                const auto newEntry = config->GetNewEntryKey();
                for (auto* event = a_event ? *a_event : nullptr; event; event = event->next) {
                    auto* button = event->AsButtonEvent();
                    if (!button || button->GetDevice() != RE::INPUT_DEVICE::kKeyboard || !button->IsDown()) continue;
                    const auto code = button->GetIDCode();
                    auto* ui = RE::UI::GetSingleton();
                    if (!ui || !ui->IsMenuOpen(RE::BookMenu::MENU_NAME)) continue;
                    if (code == tearOut) {
                        button->SetUserEvent("");
                        SKSE::GetTaskInterface()->AddUITask([]() { Guarded("Tear-out key", AskTearOut); });
                    } else if (newEntry != 0 && code == newEntry) {
                        button->SetUserEvent("");
                        SKSE::GetTaskInterface()->AddUITask([]() { Guarded("New-entry key", NewEntryInBook); });
                    }
                }
                return RE::BSEventNotifyControl::kContinue;
            }
        };
    }

    void Register() {
        static InputSink inputSink;
        API().SetClientName("Physical Diaries");  // in Ink & Quill's MCM
        API().AddOwner([](void* u, std::uint32_t book) {
            bool mine = false;
            Guarded("Edit key", [&] { mine = Owner(u, book); });
            return mine;
        }, nullptr);
        RegisterKeys();
        const auto onBlank = [](void* u, std::uint32_t blank) {
            bool begun = false;
            Guarded("Blank journal", [&] { begun = OnBlankOpen(u, blank); });
            return begun;
        };
        for (const auto blank : BlankJournals::Forms()) API().RegisterBlank(blank, onBlank, nullptr);
        // Ahead of MenuControls, so the menu never gets our keys.
        if (auto* input = RE::BSInputDeviceManager::GetSingleton()) input->PrependEventSink(&inputSink);
        SKSE::log::info("[BookEditor] Registered with Ink & Quill: the player's journals can be written in");
    }

    bool RegisterKeys() {
        if (!WritingMode::IsOn()) return true;
        auto* config = Config::GetSingleton();
        std::vector<std::uint32_t> keys{ config->GetDeleteKey() };
        if (const auto key = config->GetNewEntryKey(); key != 0) keys.push_back(key);
        // The whole set each time: it replaces the last.
        const auto kept = API().RegisterKeys(keys.data(), static_cast<std::int32_t>(keys.size()));
        if (kept == static_cast<std::int32_t>(keys.size())) return true;
        SKSE::log::warn("[BookEditor] Ink & Quill kept {} of our {} keys (see InkAndQuill.log): the others don't act "
                        "while writing", kept, keys.size());
        return false;
    }

    void Reset() {
        ++g_generation;
        g_session = 0;  // Ink & Quill ends its session too; that OnEnd is ignored
        g_pending.clear();
        g_edit.clear();
        g_addQueued.clear();
        g_bookFormId = 0;
        g_blankLook.clear();
        g_asking = false;
    }

} // namespace SkyrimNetDiaries::BookEditor
