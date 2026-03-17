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

    static bool StepAndFinalize(sqlite3* db, sqlite3_stmt* stmt, const char* tag) {
        bool ok = (sqlite3_step(stmt) == SQLITE_DONE);
        if (!ok) SKSE::log::error("[DiaryDB] {} step: {}", tag, sqlite3_errmsg(db));
        sqlite3_finalize(stmt);
        return ok;
    }

    // ── Volume operations ────────────────────────────────────────────────────────

    bool DiaryDB::UpsertVolume(const VolumeRow& r) {
        if (!db_) return false;

        const char* sql =
            "INSERT INTO volumes "
            "(actor_uuid, actor_name, actor_form_id, book_form_id, volume_number, start_time, end_time, "
            " journal_template, bio_template_name, last_known_entry_count, "
            " prev_volume_last_creation_time, prev_volume_count_at_boundary, book_text) "
            "VALUES (?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11,?12,?13) "
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
            // Preserve existing text when the caller passes an empty string.
            " book_text=CASE WHEN excluded.book_text='' THEN volumes.book_text "
            "                ELSE excluded.book_text END;";

        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
            SKSE::log::error("[DiaryDB] UpsertVolume prepare: {}", sqlite3_errmsg(db_));
            return false;
        }

        sqlite3_bind_text(stmt,  1, r.actorUuid.c_str(),          -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt,  2, r.actorName.c_str(),           -1, SQLITE_TRANSIENT);
        sqlite3_bind_int (stmt,  3, static_cast<int>(r.actorFormId));
        sqlite3_bind_int (stmt,  4, static_cast<int>(r.bookFormId));
        sqlite3_bind_int (stmt,  5, r.volumeNumber);
        sqlite3_bind_double(stmt,6, r.startTime);
        sqlite3_bind_double(stmt,7, r.endTime);
        sqlite3_bind_text(stmt,  8, r.journalTemplate.c_str(),     -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt,  9, r.bioTemplateName.c_str(),     -1, SQLITE_TRANSIENT);
        sqlite3_bind_int (stmt, 10, r.lastKnownEntryCount);
        sqlite3_bind_double(stmt,11, r.prevVolumeLastCreationTime);
        sqlite3_bind_int (stmt, 12, r.prevVolumeCountAtBoundary);
        sqlite3_bind_text(stmt, 13, r.bookText.c_str(),            -1, SQLITE_TRANSIENT);

        return StepAndFinalize(db_, stmt, "UpsertVolume");
    }

    bool DiaryDB::UpdateBookText(const std::string& actorUuid, int volumeNumber,
                                  const std::string& text, int entryCount) {
        if (!db_) return false;
        const char* sql =
            "UPDATE volumes SET book_text=?1, last_known_entry_count=?2 "
            "WHERE actor_uuid=?3 AND volume_number=?4;";
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
            SKSE::log::error("[DiaryDB] UpdateBookText prepare: {}", sqlite3_errmsg(db_));
            return false;
        }
        sqlite3_bind_text  (stmt, 1, text.c_str(),       -1, SQLITE_TRANSIENT);
        sqlite3_bind_int   (stmt, 2, entryCount);
        sqlite3_bind_text  (stmt, 3, actorUuid.c_str(),  -1, SQLITE_TRANSIENT);
        sqlite3_bind_int   (stmt, 4, volumeNumber);
        return StepAndFinalize(db_, stmt, "UpdateBookText");
    }

    bool DiaryDB::UpdateEndTime(const std::string& actorUuid, int volumeNumber,
                                 double endTime) {
        if (!db_) return false;
        const char* sql =
            "UPDATE volumes SET end_time=?1 WHERE actor_uuid=?2 AND volume_number=?3;";
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
            SKSE::log::error("[DiaryDB] UpdateEndTime prepare: {}", sqlite3_errmsg(db_));
            return false;
        }
        sqlite3_bind_double(stmt, 1, endTime);
        sqlite3_bind_text  (stmt, 2, actorUuid.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int   (stmt, 3, volumeNumber);
        return StepAndFinalize(db_, stmt, "UpdateEndTime");
    }

    bool DiaryDB::UpdateFormID(const std::string& actorUuid, int volumeNumber,
                                std::uint32_t formId) {
        if (!db_) return false;
        const char* sql =
            "UPDATE volumes SET book_form_id=?1 WHERE actor_uuid=?2 AND volume_number=?3;";
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
            SKSE::log::error("[DiaryDB] UpdateFormID prepare: {}", sqlite3_errmsg(db_));
            return false;
        }
        sqlite3_bind_int (stmt, 1, static_cast<int>(formId));
        sqlite3_bind_text(stmt, 2, actorUuid.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int (stmt, 3, volumeNumber);
        return StepAndFinalize(db_, stmt, "UpdateFormID");
    }

    bool DiaryDB::DeleteVolume(const std::string& actorUuid, int volumeNumber) {
        if (!db_) return false;
        const char* sql =
            "DELETE FROM volumes WHERE actor_uuid=?1 AND volume_number=?2;";
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
            SKSE::log::error("[DiaryDB] DeleteVolume prepare: {}", sqlite3_errmsg(db_));
            return false;
        }
        sqlite3_bind_text(stmt, 1, actorUuid.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int (stmt, 2, volumeNumber);
        return StepAndFinalize(db_, stmt, "DeleteVolume");
    }

    bool DiaryDB::DeleteActor(const std::string& actorUuid) {
        if (!db_) return false;
        // Delete volumes
        {
            const char* sql = "DELETE FROM volumes WHERE actor_uuid=?1;";
            sqlite3_stmt* stmt = nullptr;
            if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
                SKSE::log::error("[DiaryDB] DeleteActor volumes prepare: {}", sqlite3_errmsg(db_));
                return false;
            }
            sqlite3_bind_text(stmt, 1, actorUuid.c_str(), -1, SQLITE_TRANSIENT);
            StepAndFinalize(db_, stmt, "DeleteActor volumes");
        }
        // Delete template
        {
            const char* sql = "DELETE FROM actor_templates WHERE actor_uuid=?1;";
            sqlite3_stmt* stmt = nullptr;
            if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return false;
            sqlite3_bind_text(stmt, 1, actorUuid.c_str(), -1, SQLITE_TRANSIENT);
            StepAndFinalize(db_, stmt, "DeleteActor template");
        }
        return true;
    }

    std::vector<DiaryDB::VolumeRow> DiaryDB::LoadAllVolumes() {
        std::vector<VolumeRow> rows;
        if (!db_) return rows;

        const char* sql =
            "SELECT actor_uuid, actor_name, actor_form_id, book_form_id, volume_number, start_time, end_time, "
            "       journal_template, bio_template_name, last_known_entry_count, "
            "       prev_volume_last_creation_time, prev_volume_count_at_boundary, book_text, "
            "       persisted_in_save "
            "FROM volumes ORDER BY actor_uuid, volume_number;";

        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
            SKSE::log::error("[DiaryDB] LoadAllVolumes prepare: {}", sqlite3_errmsg(db_));
            return rows;
        }

        while (sqlite3_step(stmt) == SQLITE_ROW) {
            VolumeRow r;
            auto col = [&](int i) -> std::string {
                const char* t = reinterpret_cast<const char*>(sqlite3_column_text(stmt, i));
                return t ? t : "";
            };
            r.actorUuid                   = col(0);
            r.actorName                   = col(1);
            r.actorFormId                 = static_cast<std::uint32_t>(sqlite3_column_int(stmt, 2));
            r.bookFormId                  = static_cast<std::uint32_t>(sqlite3_column_int(stmt, 3));
            r.volumeNumber                = sqlite3_column_int(stmt, 4);
            r.startTime                   = sqlite3_column_double(stmt, 5);
            r.endTime                     = sqlite3_column_double(stmt, 6);
            r.journalTemplate             = col(7);
            r.bioTemplateName             = col(8);
            r.lastKnownEntryCount         = sqlite3_column_int(stmt, 9);
            r.prevVolumeLastCreationTime  = sqlite3_column_double(stmt, 10);
            r.prevVolumeCountAtBoundary   = sqlite3_column_int(stmt, 11);
            r.bookText                    = col(12);
            r.persistedInSave             = sqlite3_column_int(stmt, 13) != 0;
            rows.push_back(std::move(r));
        }
        sqlite3_finalize(stmt);
        return rows;
    }

    // Open an arbitrary DiaryDB file read-only and return all its volume rows.
    // Does NOT affect the currently-open database instance.
    std::vector<DiaryDB::VolumeRow> DiaryDB::LoadAllVolumesFromPath(const std::string& dbPath) {
        std::vector<VolumeRow> rows;

        SKSE::log::info("[DiaryDB::LoadAllVolumesFromPath] Opening: {}", dbPath);

        sqlite3* db = nullptr;
        if (sqlite3_open_v2(dbPath.c_str(), &db, SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK) {
            SKSE::log::error("[DiaryDB::LoadAllVolumesFromPath] Failed to open: {}",
                             db ? sqlite3_errmsg(db) : "(null handle)");
            if (db) sqlite3_close(db);
            return rows;
        }

        const char* sql =
            "SELECT actor_uuid, actor_name, actor_form_id, book_form_id, volume_number, "
            "       start_time, end_time, journal_template, bio_template_name, "
            "       last_known_entry_count, prev_volume_last_creation_time, "
            "       prev_volume_count_at_boundary, book_text, persisted_in_save "
            "FROM volumes ORDER BY actor_name, volume_number;";

        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
            SKSE::log::error("[DiaryDB::LoadAllVolumesFromPath] prepare failed: {}", sqlite3_errmsg(db));
            sqlite3_close(db);
            return rows;
        }

        while (sqlite3_step(stmt) == SQLITE_ROW) {
            VolumeRow r;
            auto col = [&](int i) -> std::string {
                const char* t = reinterpret_cast<const char*>(sqlite3_column_text(stmt, i));
                return t ? t : "";
            };
            r.actorUuid                  = col(0);
            r.actorName                  = col(1);
            r.actorFormId                = static_cast<std::uint32_t>(sqlite3_column_int(stmt, 2));
            r.bookFormId                 = static_cast<std::uint32_t>(sqlite3_column_int(stmt, 3));
            r.volumeNumber               = sqlite3_column_int(stmt, 4);
            r.startTime                  = sqlite3_column_double(stmt, 5);
            r.endTime                    = sqlite3_column_double(stmt, 6);
            r.journalTemplate            = col(7);
            r.bioTemplateName            = col(8);
            r.lastKnownEntryCount        = sqlite3_column_int(stmt, 9);
            r.prevVolumeLastCreationTime = sqlite3_column_double(stmt, 10);
            r.prevVolumeCountAtBoundary  = sqlite3_column_int(stmt, 11);
            r.bookText                   = col(12);
            r.persistedInSave            = sqlite3_column_int(stmt, 13) != 0;
            rows.push_back(std::move(r));
        }
        sqlite3_finalize(stmt);
        sqlite3_close(db);

        SKSE::log::info("[DiaryDB::LoadAllVolumesFromPath] Loaded {} volume(s)", rows.size());
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

        const char* sql = "SELECT actor_uuid, template_name FROM actor_templates;";
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return result;

        while (sqlite3_step(stmt) == SQLITE_ROW) {
            const char* uuid = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
            const char* name = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
            if (uuid && name) result[uuid] = name;
        }
        sqlite3_finalize(stmt);
        return result;
    }

    // ── Theft tracking ───────────────────────────────────────────────────────────

    bool DiaryDB::AddStolenVolume(const std::string& actorUuid, int volumeNumber, double gameTime) {
        if (!db_) return false;
        const char* sql =
            "INSERT OR REPLACE INTO stolen_volumes (actor_uuid, volume_number, stolen_at) "
            "VALUES (?1, ?2, ?3);";
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
            SKSE::log::error("[DiaryDB] AddStolenVolume prepare failed: {}", sqlite3_errmsg(db_));
            return false;
        }
        sqlite3_bind_text(stmt, 1, actorUuid.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(stmt, 2, volumeNumber);
        sqlite3_bind_double(stmt, 3, gameTime);
        bool ok = (sqlite3_step(stmt) == SQLITE_DONE);
        sqlite3_finalize(stmt);
        
        if (ok) {
            SKSE::log::debug("[DiaryDB] Marked volume {} stolen for UUID: {}", volumeNumber, actorUuid);
        }
        return ok;
    }

    bool DiaryDB::RemoveStolenVolume(const std::string& actorUuid, int volumeNumber) {
        if (!db_) return false;
        const char* sql = "DELETE FROM stolen_volumes WHERE actor_uuid=?1 AND volume_number=?2;";
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return false;
        sqlite3_bind_text(stmt, 1, actorUuid.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(stmt, 2, volumeNumber);
        bool ok = (sqlite3_step(stmt) == SQLITE_DONE);
        sqlite3_finalize(stmt);
        
        if (ok) {
            SKSE::log::debug("[DiaryDB] Removed stolen volume {} for UUID: {}", volumeNumber, actorUuid);
        }
        return ok;
    }

    bool DiaryDB::HasAnyStolenVolumes(const std::string& actorUuid) {
        if (!db_) return false;
        const char* sql = "SELECT COUNT(*) FROM stolen_volumes WHERE actor_uuid=?1;";
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return false;
        sqlite3_bind_text(stmt, 1, actorUuid.c_str(), -1, SQLITE_TRANSIENT);
        
        bool hasStolen = false;
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            int count = sqlite3_column_int(stmt, 0);
            hasStolen = (count > 0);
            
            if (count > 0) {
                // List which volumes are stolen for debugging
                sqlite3_finalize(stmt);
                const char* detailSql = "SELECT volume_number FROM stolen_volumes WHERE actor_uuid=?1 ORDER BY volume_number;";
                if (sqlite3_prepare_v2(db_, detailSql, -1, &stmt, nullptr) == SQLITE_OK) {
                    sqlite3_bind_text(stmt, 1, actorUuid.c_str(), -1, SQLITE_TRANSIENT);
                    std::string volumes;
                    while (sqlite3_step(stmt) == SQLITE_ROW) {
                        int vol = sqlite3_column_int(stmt, 0);
                        if (!volumes.empty()) volumes += ", ";
                        volumes += std::to_string(vol);
                    }
                    SKSE::log::debug("[DiaryDB] UUID {} has {} stolen volumes: [{}]", actorUuid, count, volumes);
                }
            } else {
                SKSE::log::debug("[DiaryDB] UUID {} has {} stolen volumes", actorUuid, count);
            }
        }
        sqlite3_finalize(stmt);
        return hasStolen;
    }

    bool DiaryDB::ClearAllStolenVolumes(const std::string& actorUuid) {
        if (!db_) return false;
        const char* sql = "DELETE FROM stolen_volumes WHERE actor_uuid=?1;";
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
            SKSE::log::error("[DiaryDB] ClearAllStolenVolumes prepare failed: {}", sqlite3_errmsg(db_));
            return false;
        }
        sqlite3_bind_text(stmt, 1, actorUuid.c_str(), -1, SQLITE_TRANSIENT);
        bool ok = (sqlite3_step(stmt) == SQLITE_DONE);
        sqlite3_finalize(stmt);
        
        if (ok) {
            int rowsAffected = sqlite3_changes(db_);
            SKSE::log::debug("[DiaryDB] Cleared all stolen volumes for UUID: {} ({} rows deleted)", actorUuid, rowsAffected);
        } else {
            SKSE::log::error("[DiaryDB] ClearAllStolenVolumes DELETE failed: {}", sqlite3_errmsg(db_));
        }
        return ok;
    }

    double DiaryDB::GetLastKnownGameTime(const std::string& actorUuid) {
        if (!db_) return 0.0;
        const char* sql = "SELECT last_known_game_time FROM actor_templates WHERE actor_uuid=?1;";
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return 0.0;
        sqlite3_bind_text(stmt, 1, actorUuid.c_str(), -1, SQLITE_TRANSIENT);
        
        double result = 0.0;
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            result = sqlite3_column_double(stmt, 0);
        }
        sqlite3_finalize(stmt);
        return result;
    }

    bool DiaryDB::UpdateLastKnownGameTime(const std::string& actorUuid, double gameTime) {
        if (!db_) return false;
        const char* sql =
            "INSERT INTO actor_templates (actor_uuid, last_known_game_time) VALUES (?1, ?2) "
            "ON CONFLICT(actor_uuid) DO UPDATE SET last_known_game_time=?2;";
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return false;
        sqlite3_bind_text(stmt, 1, actorUuid.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_double(stmt, 2, gameTime);
        bool ok = (sqlite3_step(stmt) == SQLITE_DONE);
        sqlite3_finalize(stmt);
        return ok;
    }

    bool DiaryDB::UpsertActorTemplate(const std::string& uuid,
                                       const std::string& templateName) {
        if (!db_) return false;
        const char* sql =
            "INSERT INTO actor_templates (actor_uuid, template_name) VALUES (?1,?2) "
            "ON CONFLICT(actor_uuid) DO UPDATE SET template_name=excluded.template_name;";
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return false;
        sqlite3_bind_text(stmt, 1, uuid.c_str(),          -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 2, templateName.c_str(),  -1, SQLITE_TRANSIENT);
        return StepAndFinalize(db_, stmt, "UpsertActorTemplate");
    }

} // namespace SkyrimNetDiaries
