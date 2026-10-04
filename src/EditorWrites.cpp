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

#include "EditorInternal.h"
#include "BookEditor.h"
#include "DiaryDB.h"
#include "Localization.h"

#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>

// The book editor's writes to SkyrimNet: one worker, in order (docs/EDITING.md#saving).
namespace SkyrimNetDiaries::BookEditor {

    namespace {
        // Called on the game thread when a job is done.  An add's job carries its new id.
        void CompleteWrite(const WriteJob& job, bool ok) {
            if (job.generation != g_generation) {
                SKSE::log::info("[BookEditor] Write for entry {} finished after a load: ignored", job.entryId);
                return;
            }
            const bool isDelete = job.kind == WriteJob::Kind::Delete;
            const int key = job.entryId != 0 ? job.entryId : -job.localKey;
            if (ok && job.entryId != 0) {
                auto* db = DiaryDB::GetSingleton();
                if (isDelete) {
                    db->DeleteBlood(job.entryId);
                } else {
                    db->SetBlood(job.entryId, job.blood, job.text, job.bloodHeading);
                }
            }
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

        // One worker for the process, so writes land in order; completions run as game-thread tasks.
        // A new entry's later jobs name it by local key: its add ran first, and ids_ has its id.
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
    }

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

    bool HasPendingWrites(RE::FormID a_bookFormId) {
        const auto it = g_pending.find(a_bookFormId);
        return it != g_pending.end() && it->second.count > 0;
    }

} // namespace SkyrimNetDiaries::BookEditor
