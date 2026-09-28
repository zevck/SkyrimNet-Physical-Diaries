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

#include "DiaryDB.h"

#include <sqlite3.h>
#include <filesystem>

namespace SkyrimNetDiaries {

    DiaryDB* DiaryDB::GetSingleton() {
        static DiaryDB instance;
        return &instance;
    }

    bool DiaryDB::Open(const std::string& saveFolder) {
        if (db_ && openFolder_ == saveFolder) return true;  // already open for same folder
        if (db_) Close();

        // The name becomes a path component: accept only "SkyrimNet-<digits and dashes>".
        const bool validName = saveFolder.starts_with("SkyrimNet-") && saveFolder.size() > 10 &&
            std::all_of(saveFolder.begin() + 10, saveFolder.end(),
                        [](char c) { return (c >= '0' && c <= '9') || c == '-'; });
        if (!validName) {
            SKSE::log::error("[DiaryDB] Refusing to open invalid save folder name '{}'", saveFolder);
            return false;
        }

        auto dbPath = std::filesystem::current_path()
            / "Data" / "SKSE" / "Plugins" / "SkyrimNetPhysicalDiaries"
            / saveFolder / "diary.db";
        std::filesystem::create_directories(dbPath.parent_path());

        if (sqlite3_open(dbPath.string().c_str(), &db_) != SQLITE_OK) {
            SKSE::log::error("[DiaryDB] Failed to open '{}': {}", dbPath.string(), sqlite3_errmsg(db_));
            sqlite3_close(db_);
            db_ = nullptr;
            return false;
        }

        Exec("PRAGMA journal_mode=WAL;");
        Exec("PRAGMA synchronous=NORMAL;");

        if (!EnsureSchema()) {
            Close();
            return false;
        }

        openFolder_ = saveFolder;
        SKSE::log::info("[DiaryDB] Opened '{}'", dbPath.string());
        return true;
    }

    void DiaryDB::Close() {
        if (db_) {
            sqlite3_close(db_);
            db_ = nullptr;
            openFolder_.clear();
        }
    }

    bool DiaryDB::Exec(const char* sql) {
        char* err = nullptr;
        if (sqlite3_exec(db_, sql, nullptr, nullptr, &err) != SQLITE_OK) {
            SKSE::log::error("[DiaryDB] SQL error: {}", err ? err : "unknown");
            sqlite3_free(err);
            return false;
        }
        return true;
    }

    bool DiaryDB::EnsureSchema() {
        // Create tables (new installs).
        bool ok = Exec(R"(
            CREATE TABLE IF NOT EXISTS volumes (
                actor_uuid                      TEXT    NOT NULL,
                actor_name                      TEXT    NOT NULL DEFAULT '',
                actor_form_id                   INTEGER NOT NULL DEFAULT 0,
                book_form_id                    INTEGER NOT NULL DEFAULT 0,
                volume_number                   INTEGER NOT NULL DEFAULT 1,
                start_time                      REAL    NOT NULL DEFAULT 0,
                end_time                        REAL    NOT NULL DEFAULT 0,
                journal_template                TEXT    NOT NULL DEFAULT '',
                bio_template_name               TEXT    NOT NULL DEFAULT '',
                last_known_entry_count          INTEGER NOT NULL DEFAULT 0,
                prev_volume_last_creation_time  REAL    NOT NULL DEFAULT 0,
                prev_volume_count_at_boundary   INTEGER NOT NULL DEFAULT 0,
                book_text                       TEXT    NOT NULL DEFAULT '',
                persisted_in_save               INTEGER NOT NULL DEFAULT 0,
                PRIMARY KEY (actor_uuid, volume_number)
            );
            CREATE TABLE IF NOT EXISTS actor_templates (
                actor_uuid                TEXT PRIMARY KEY,
                template_name             TEXT NOT NULL DEFAULT '',
                last_known_game_time      REAL DEFAULT 0.0
            );
            CREATE TABLE IF NOT EXISTS stolen_volumes (
                actor_uuid     TEXT NOT NULL,
                volume_number  INTEGER NOT NULL,
                stolen_at      REAL NOT NULL,
                PRIMARY KEY (actor_uuid, volume_number)
            );
        )");
        if (!ok) return false;

        // Migration: add persisted_in_save to existing DBs that pre-date this column.
        // SQLite returns an error if the column already exists — we ignore it.
        sqlite3_exec(db_,
            "ALTER TABLE volumes ADD COLUMN persisted_in_save INTEGER NOT NULL DEFAULT 0;",
            nullptr, nullptr, nullptr);

        // Migration: add actor_form_id to existing DBs.
        sqlite3_exec(db_,
            "ALTER TABLE volumes ADD COLUMN actor_form_id INTEGER NOT NULL DEFAULT 0;",
            nullptr, nullptr, nullptr);

        return true;
    }

    // ── Helpers ─────────────────────────────────────────────────────────────────

    namespace {

        // A prepared statement that finalizes itself.  Failed prepares and steps are
        // logged with the statement's tag; a failed prepare makes every call a no-op.
        class Statement {
        public:
            Statement(sqlite3* db, const char* sql, const char* tag) : db_(db), tag_(tag) {
                if (sqlite3_prepare_v2(db, sql, -1, &stmt_, nullptr) != SQLITE_OK) {
                    SKSE::log::error("[DiaryDB] {} prepare: {}", tag, sqlite3_errmsg(db));
                    sqlite3_finalize(stmt_);
                    stmt_ = nullptr;
                }
            }
            ~Statement() { sqlite3_finalize(stmt_); }
            Statement(const Statement&) = delete;
            Statement& operator=(const Statement&) = delete;

            explicit operator bool() const { return stmt_ != nullptr; }

            Statement& Bind(int i, const std::string& v) { sqlite3_bind_text(stmt_, i, v.c_str(), -1, SQLITE_TRANSIENT); return *this; }
            Statement& Bind(int i, int v)                { sqlite3_bind_int(stmt_, i, v); return *this; }
            Statement& Bind(int i, double v)             { sqlite3_bind_double(stmt_, i, v); return *this; }

            // For statements without result rows.
            bool Run() {
                if (!stmt_) return false;
                const bool ok = sqlite3_step(stmt_) == SQLITE_DONE;
                if (!ok) SKSE::log::error("[DiaryDB] {} step: {}", tag_, sqlite3_errmsg(db_));
                return ok;
            }
            // Advances to the next result row.
            bool Next() { return stmt_ && sqlite3_step(stmt_) == SQLITE_ROW; }

            int Int(int col) const { return sqlite3_column_int(stmt_, col); }
            double Double(int col) const { return sqlite3_column_double(stmt_, col); }
            std::string Text(int col) const {
                const auto* t = reinterpret_cast<const char*>(sqlite3_column_text(stmt_, col));
                return t ? t : "";
            }

        private:
            sqlite3* db_;
            const char* tag_;
            sqlite3_stmt* stmt_ = nullptr;
        };

    } // namespace

    // ── Volume operations ────────────────────────────────────────────────────────

    bool DiaryDB::UpsertVolume(const VolumeRow& r) {
        if (!db_) return false;
        Statement st(db_,
            "INSERT INTO volumes "
            "(actor_uuid, actor_name, actor_form_id, book_form_id, volume_number, start_time, end_time, "
            " journal_template, bio_template_name, last_known_entry_count, "
            " prev_volume_last_creation_time, prev_volume_count_at_boundary, book_text, persisted_in_save) "
            "VALUES (?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11,?12,?13,?14) "
            "ON CONFLICT(actor_uuid, volume_number) DO UPDATE SET "
            " actor_name=excluded.actor_name, "
            " actor_form_id=CASE WHEN excluded.actor_form_id!=0 THEN excluded.actor_form_id ELSE volumes.actor_form_id END, "
            " book_form_id=excluded.book_form_id, "
            " start_time=excluded.start_time, "
            " end_time=excluded.end_time, "
            " journal_template=excluded.journal_template, "
            " bio_template_name=excluded.bio_template_name, "
            " last_known_entry_count=excluded.last_known_entry_count, "
            " prev_volume_last_creation_time=excluded.prev_volume_last_creation_time, "
            " prev_volume_count_at_boundary=excluded.prev_volume_count_at_boundary, "
            // Never un-persist: once a volume was in a save, it stays marked.
            " persisted_in_save=MAX(volumes.persisted_in_save, excluded.persisted_in_save), "
            // Preserve existing text when the caller passes an empty string.
            " book_text=CASE WHEN excluded.book_text='' THEN volumes.book_text "
            "                ELSE excluded.book_text END;",
            "UpsertVolume");
        return st.Bind(1, r.actorUuid).Bind(2, r.actorName)
                 .Bind(3, static_cast<int>(r.actorFormId)).Bind(4, static_cast<int>(r.bookFormId))
                 .Bind(5, r.volumeNumber).Bind(6, r.startTime).Bind(7, r.endTime)
                 .Bind(8, r.journalTemplate).Bind(9, r.bioTemplateName).Bind(10, r.lastKnownEntryCount)
                 .Bind(11, r.prevVolumeLastCreationTime).Bind(12, r.prevVolumeCountAtBoundary)
                 .Bind(13, r.bookText).Bind(14, r.persistedInSave ? 1 : 0)
                 .Run();
    }

    bool DiaryDB::UpdateBookText(const std::string& actorUuid, int volumeNumber,
                                  const std::string& text, int entryCount) {
        if (!db_) return false;
        Statement st(db_,
            "UPDATE volumes SET book_text=?1, last_known_entry_count=?2 "
            "WHERE actor_uuid=?3 AND volume_number=?4;",
            "UpdateBookText");
        return st.Bind(1, text).Bind(2, entryCount).Bind(3, actorUuid).Bind(4, volumeNumber).Run();
    }

    bool DiaryDB::UpdateEndTime(const std::string& actorUuid, int volumeNumber,
                                 double endTime) {
        if (!db_) return false;
        Statement st(db_, "UPDATE volumes SET end_time=?1 WHERE actor_uuid=?2 AND volume_number=?3;", "UpdateEndTime");
        return st.Bind(1, endTime).Bind(2, actorUuid).Bind(3, volumeNumber).Run();
    }

    bool DiaryDB::DeleteVolume(const std::string& actorUuid, int volumeNumber) {
        if (!db_) return false;
        Statement st(db_, "DELETE FROM volumes WHERE actor_uuid=?1 AND volume_number=?2;", "DeleteVolume");
        return st.Bind(1, actorUuid).Bind(2, volumeNumber).Run();
    }

    bool DiaryDB::DeleteActor(const std::string& actorUuid) {
        if (!db_) return false;
        Statement volumes(db_, "DELETE FROM volumes WHERE actor_uuid=?1;", "DeleteActor volumes");
        if (!volumes.Bind(1, actorUuid).Run()) return false;
        Statement templates(db_, "DELETE FROM actor_templates WHERE actor_uuid=?1;", "DeleteActor template");
        return templates.Bind(1, actorUuid).Run();
    }

    std::vector<DiaryDB::VolumeRow> DiaryDB::LoadAllVolumes() {
        std::vector<VolumeRow> rows;
        if (!db_) return rows;

        Statement st(db_,
            "SELECT actor_uuid, actor_name, actor_form_id, book_form_id, volume_number, start_time, end_time, "
            "       journal_template, bio_template_name, last_known_entry_count, "
            "       prev_volume_last_creation_time, prev_volume_count_at_boundary, book_text, "
            "       persisted_in_save "
            "FROM volumes ORDER BY actor_uuid, volume_number;",
            "LoadAllVolumes");
        while (st.Next()) {
            VolumeRow r;
            r.actorUuid                   = st.Text(0);
            r.actorName                   = st.Text(1);
            r.actorFormId                 = static_cast<std::uint32_t>(st.Int(2));
            r.bookFormId                  = static_cast<std::uint32_t>(st.Int(3));
            r.volumeNumber                = st.Int(4);
            r.startTime                   = st.Double(5);
            r.endTime                     = st.Double(6);
            r.journalTemplate             = st.Text(7);
            r.bioTemplateName             = st.Text(8);
            r.lastKnownEntryCount         = st.Int(9);
            r.prevVolumeLastCreationTime  = st.Double(10);
            r.prevVolumeCountAtBoundary   = st.Int(11);
            r.bookText                    = st.Text(12);
            r.persistedInSave             = st.Int(13) != 0;
            rows.push_back(std::move(r));
        }
        return rows;
    }

    bool DiaryDB::MarkAllVolumesPersisted() {
        if (!db_) return false;
        SKSE::log::debug("[DiaryDB] Marking all volumes as persisted_in_save=1");
        return Exec("UPDATE volumes SET persisted_in_save=1;");
    }

    // ── Actor-template operations ────────────────────────────────────────────────

    std::unordered_map<std::string, std::string> DiaryDB::LoadActorTemplates() {
        std::unordered_map<std::string, std::string> result;
        if (!db_) return result;
        Statement st(db_, "SELECT actor_uuid, template_name FROM actor_templates;", "LoadActorTemplates");
        while (st.Next()) {
            auto name = st.Text(1);
            if (!name.empty()) result[st.Text(0)] = std::move(name);  // skip rows with no template yet
        }
        return result;
    }

    // ── Theft tracking ───────────────────────────────────────────────────────────

    bool DiaryDB::AddStolenVolume(const std::string& actorUuid, int volumeNumber, double gameTime) {
        if (!db_) return false;
        Statement st(db_,
            "INSERT OR REPLACE INTO stolen_volumes (actor_uuid, volume_number, stolen_at) VALUES (?1, ?2, ?3);",
            "AddStolenVolume");
        const bool ok = st.Bind(1, actorUuid).Bind(2, volumeNumber).Bind(3, gameTime).Run();
        if (ok) SKSE::log::debug("[DiaryDB] Marked volume {} stolen for UUID: {}", volumeNumber, actorUuid);
        return ok;
    }

    bool DiaryDB::RemoveStolenVolume(const std::string& actorUuid, int volumeNumber) {
        if (!db_) return false;
        Statement st(db_, "DELETE FROM stolen_volumes WHERE actor_uuid=?1 AND volume_number=?2;", "RemoveStolenVolume");
        const bool ok = st.Bind(1, actorUuid).Bind(2, volumeNumber).Run();
        if (ok) SKSE::log::debug("[DiaryDB] Removed stolen volume {} for UUID: {}", volumeNumber, actorUuid);
        return ok;
    }

    std::vector<std::string> DiaryDB::LoadStolenActorUuids() {
        std::vector<std::string> uuids;
        if (!db_) return uuids;
        Statement st(db_, "SELECT DISTINCT actor_uuid FROM stolen_volumes;", "LoadStolenActorUuids");
        while (st.Next()) uuids.push_back(st.Text(0));
        return uuids;
    }

    bool DiaryDB::ClearAllStolenVolumes(const std::string& actorUuid) {
        if (!db_) return false;
        Statement st(db_, "DELETE FROM stolen_volumes WHERE actor_uuid=?1;", "ClearAllStolenVolumes");
        const bool ok = st.Bind(1, actorUuid).Run();
        if (ok) {
            SKSE::log::debug("[DiaryDB] Cleared all stolen volumes for UUID: {} ({} rows deleted)",
                             actorUuid, sqlite3_changes(db_));
        }
        return ok;
    }

    int DiaryDB::RemoveStolenVolumesAfter(double gameTime) {
        if (!db_) return 0;
        Statement st(db_, "DELETE FROM stolen_volumes WHERE stolen_at > ?1;", "RemoveStolenVolumesAfter");
        return st.Bind(1, gameTime).Run() ? sqlite3_changes(db_) : 0;
    }

    bool DiaryDB::ClearPersisted(const std::string& actorUuid, int volumeNumber) {
        if (!db_) return false;
        Statement st(db_, "UPDATE volumes SET persisted_in_save=0 WHERE actor_uuid=?1 AND volume_number=?2;",
                     "ClearPersisted");
        return st.Bind(1, actorUuid).Bind(2, volumeNumber).Run();
    }

    bool DiaryDB::UpsertActorTemplate(const std::string& uuid,
                                       const std::string& templateName) {
        if (!db_) return false;
        Statement st(db_,
            "INSERT INTO actor_templates (actor_uuid, template_name) VALUES (?1,?2) "
            "ON CONFLICT(actor_uuid) DO UPDATE SET template_name=excluded.template_name;",
            "UpsertActorTemplate");
        return st.Bind(1, uuid).Bind(2, templateName).Run();
    }

} // namespace SkyrimNetDiaries
