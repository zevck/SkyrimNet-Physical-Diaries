#include "Database.h"
#include <sstream>

namespace SkyrimNetDiaries {

    Database::~Database() {
        Close();
    }

    bool Database::Open(const std::string& dbPath, bool readOnly) {
        if (is_open_) {
            Close();
        }

        int flags = readOnly ? SQLITE_OPEN_READONLY : (SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE);
        int rc = sqlite3_open_v2(dbPath.c_str(), &db_, flags, nullptr);
        
        if (rc != SQLITE_OK) {
            SKSE::log::error("Failed to open database {} (read-only={}): {}", dbPath, readOnly, sqlite3_errmsg(db_));
            sqlite3_close(db_);
            db_ = nullptr;
            return false;
        }

        is_open_ = true;
        SKSE::log::info("Successfully opened database: {} (read-only={})", dbPath, readOnly);
        return true;
    }

    void Database::Close() {
        if (is_open_ && db_) {
            sqlite3_close(db_);
            db_ = nullptr;
            is_open_ = false;
        }
    }
    
    bool Database::EnableWALMode() {
        if (!is_open_ || !db_) {
            SKSE::log::error("Cannot enable WAL mode - database is not open");
            return false;
        }
        
        // Enable WAL (Write-Ahead Logging) mode for concurrent access
        // This allows readers and writers to work simultaneously without blocking
        char* errMsg = nullptr;
        int rc = sqlite3_exec(db_, "PRAGMA journal_mode=WAL;", nullptr, nullptr, &errMsg);
        
        if (rc != SQLITE_OK) {
            SKSE::log::error("Failed to enable WAL mode: {}", errMsg ? errMsg : "unknown error");
            if (errMsg) sqlite3_free(errMsg);
            return false;
        }
        
        SKSE::log::info("✓ WAL mode enabled - database now supports concurrent reads/writes");
        return true;
    }

    std::vector<DiaryEntry> Database::GetDiaryEntries(const std::string& actorUUID, int limit, double startTime, double endTime) {
        std::vector<DiaryEntry> entries;

        if (!is_open_) {
            SKSE::log::error("Database is not open");
            return entries;
        }

        // Build SQL query with optional time filters
        std::string sql = R"(
            SELECT 
                d.actor_uuid,
                d.content,
                d.entry_date,
                d.creation_time,
                d.location,
                d.emotion,
                d.importance_score,
                u.actor_name
            FROM diary_entries d
            LEFT JOIN uuid_mappings u ON d.actor_uuid = u.uuid
            WHERE d.actor_uuid = ?
        )";
        
        // Add time filters if specified
        if (startTime > 0.0) {
            sql += " AND d.entry_date >= ?";
        }
        if (endTime > 0.0) {
            sql += " AND d.entry_date <= ?";
        }
        
        sql += " ORDER BY d.entry_date ASC LIMIT ?";

        SKSE::log::info("GetDiaryEntries SQL: {} (UUID: {}, startTime: {}, endTime: {}, limit: {})", 
                       sql, actorUUID, startTime, endTime, limit);

        sqlite3_stmt* stmt = nullptr;
        int rc = sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr);
        
        if (rc != SQLITE_OK) {
            SKSE::log::error("Failed to prepare statement: {}", sqlite3_errmsg(db_));
            return entries;
        }

        int bindIndex = 1;
        sqlite3_bind_text(stmt, bindIndex++, actorUUID.c_str(), -1, SQLITE_TRANSIENT);
        
        if (startTime > 0.0) {
            sqlite3_bind_double(stmt, bindIndex++, startTime);
        }
        if (endTime > 0.0) {
            sqlite3_bind_double(stmt, bindIndex++, endTime);
        }
        
        sqlite3_bind_int(stmt, bindIndex, limit);

        while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
            DiaryEntry entry;
            entry.actor_uuid = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
            entry.content = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
            entry.entry_date = sqlite3_column_double(stmt, 2);
            entry.creation_time = sqlite3_column_double(stmt, 3);
            
            if (sqlite3_column_text(stmt, 4)) {
                entry.location = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 4));
            }
            if (sqlite3_column_text(stmt, 5)) {
                entry.emotion = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 5));
            }
            
            entry.importance_score = sqlite3_column_double(stmt, 6);
            
            if (sqlite3_column_text(stmt, 7)) {
                entry.actor_name = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 7));
            }

            entries.push_back(entry);
        }

        sqlite3_finalize(stmt);

        if (rc != SQLITE_DONE) {
            SKSE::log::error("Error reading diary entries: {}", sqlite3_errmsg(db_));
        }

        SKSE::log::info("GetDiaryEntries returned {} entries", entries.size());

        return entries;
    }

    std::string Database::GetActorName(const std::string& actorUUID) {
        if (!is_open_) {
            return "";
        }

        const char* sql = "SELECT actor_name FROM uuid_mappings WHERE uuid = ? LIMIT 1";
        
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
            SKSE::log::error("Failed to prepare statement: {}", sqlite3_errmsg(db_));
            return "";
        }

        sqlite3_bind_text(stmt, 1, actorUUID.c_str(), -1, SQLITE_TRANSIENT);

        std::string name;
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            if (sqlite3_column_text(stmt, 0)) {
                name = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
            }
        }

        sqlite3_finalize(stmt);
        return name;
    }

    std::vector<DiaryEntry> Database::GetAllDiaryEntries(int limit) {
        std::vector<DiaryEntry> entries;

        if (!is_open_) {
            SKSE::log::error("Database is not open");
            return entries;
        }

        const char* sql = R"(
            SELECT 
                d.actor_uuid,
                d.content,
                d.entry_date,
                d.creation_time,
                d.location,
                d.emotion,
                d.importance_score,
                u.actor_name
            FROM diary_entries d
            LEFT JOIN uuid_mappings u ON d.actor_uuid = u.uuid
            ORDER BY d.creation_time DESC
            LIMIT ?
        )";

        sqlite3_stmt* stmt = nullptr;
        int rc = sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
        
        if (rc != SQLITE_OK) {
            SKSE::log::error("Failed to prepare statement: {}", sqlite3_errmsg(db_));
            return entries;
        }

        sqlite3_bind_int(stmt, 1, limit);

        while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
            DiaryEntry entry;
            entry.actor_uuid = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
            entry.content = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
            entry.entry_date = sqlite3_column_double(stmt, 2);
            entry.creation_time = sqlite3_column_double(stmt, 3);
            
            if (sqlite3_column_text(stmt, 4)) {
                entry.location = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 4));
            }
            if (sqlite3_column_text(stmt, 5)) {
                entry.emotion = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 5));
            }
            
            entry.importance_score = sqlite3_column_double(stmt, 6);
            
            if (sqlite3_column_text(stmt, 7)) {
                entry.actor_name = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 7));
            }

            entries.push_back(entry);
        }

        sqlite3_finalize(stmt);

        if (rc != SQLITE_DONE) {
            SKSE::log::error("Error reading diary entries: {}", sqlite3_errmsg(db_));
        }

        SKSE::log::info("Retrieved {} diary entries", entries.size());
        return entries;
    }

    void Database::LogCountersTable() {
        if (!is_open_) {
            SKSE::log::error("Database is not open");
            return;
        }

        const char* sql = "SELECT id, label, value, session_id FROM counters";
        sqlite3_stmt* stmt = nullptr;
        int rc = sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
        
        if (rc != SQLITE_OK) {
            SKSE::log::error("Failed to prepare counters query: {}", sqlite3_errmsg(db_));
            return;
        }

        SKSE::log::info("=== Counters Table ===");
        while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
            int id = sqlite3_column_int(stmt, 0);
            const char* label = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
            int value = sqlite3_column_int(stmt, 2);
            const char* sessionId = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3));

            SKSE::log::info("  id={}, label='{}', value={}, session_id='{}'", 
                id, 
                label ? label : "NULL",
                value,
                sessionId ? sessionId : "NULL");
        }

        sqlite3_finalize(stmt);

        if (rc != SQLITE_DONE) {
            SKSE::log::error("Error reading counters: {}", sqlite3_errmsg(db_));
        }
    }

    std::string Database::GetPlayerUUID() {
        if (!is_open_) {
            return "";
        }

        const char* sql = "SELECT uuid FROM actors WHERE is_player = 1 LIMIT 1";
        sqlite3_stmt* stmt = nullptr;
        int rc = sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
        
        if (rc != SQLITE_OK) {
            SKSE::log::error("Failed to prepare player UUID query: {}", sqlite3_errmsg(db_));
            return "";
        }

        std::string playerUuid;
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            const char* uuid = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
            if (uuid) {
                playerUuid = uuid;
            }
        }

        sqlite3_finalize(stmt);
        return playerUuid;
    }

    std::string Database::GetPlayerName() {
        if (!is_open_) {
            return "";
        }

        const char* sql = "SELECT name FROM actors WHERE is_player = 1 LIMIT 1";
        sqlite3_stmt* stmt = nullptr;
        int rc = sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
        
        if (rc != SQLITE_OK) {
            SKSE::log::error("Failed to prepare player name query: {}", sqlite3_errmsg(db_));
            return "";
        }

        std::string playerName;
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            const char* name = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
            if (name) {
                playerName = name;
            }
        }

        sqlite3_finalize(stmt);
        return playerName;
    }

    bool Database::HasPlayerUUID(const std::string& playerUuid) {
        if (!is_open_) {
            return false;
        }

        const char* sql = "SELECT COUNT(*) FROM actors WHERE uuid = ? AND is_player = 1";
        sqlite3_stmt* stmt = nullptr;
        int rc = sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
        
        if (rc != SQLITE_OK) {
            return false;
        }

        sqlite3_bind_text(stmt, 1, playerUuid.c_str(), -1, SQLITE_TRANSIENT);

        bool hasPlayer = false;
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            int count = sqlite3_column_int(stmt, 0);
            hasPlayer = (count > 0);
        }

        sqlite3_finalize(stmt);
        return hasPlayer;
    }

    std::vector<std::string> Database::GetAllActorUUIDs() {
        std::vector<std::string> uuids;
        
        if (!is_open_) {
            return uuids;
        }

        // Get all unique actor UUIDs that have diary entries
        const char* sql = "SELECT DISTINCT actor_uuid FROM diary_entries";
        sqlite3_stmt* stmt = nullptr;
        int rc = sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
        
        if (rc != SQLITE_OK) {
            SKSE::log::error("Failed to prepare actor UUID query: {}", sqlite3_errmsg(db_));
            return uuids;
        }

        while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
            const char* uuid = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
            if (uuid) {
                uuids.push_back(uuid);
            }
        }

        sqlite3_finalize(stmt);

        if (rc != SQLITE_DONE && rc != SQLITE_ROW) {
            SKSE::log::error("Error reading actor UUIDs: {}", sqlite3_errmsg(db_));
        }

        return uuids;
    }
    
    std::string Database::GetUUIDByTemplateName(const std::string& templateName) {
        if (!is_open_ || templateName.empty()) {
            return "";
        }

        // Query uuid_mappings table for matching bio_template_name
        const char* sql = "SELECT uuid FROM uuid_mappings WHERE bio_template_name = ?";
        sqlite3_stmt* stmt = nullptr;
        int rc = sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
        
        if (rc != SQLITE_OK) {
            SKSE::log::error("Failed to prepare UUID template query: {}", sqlite3_errmsg(db_));
            return "";
        }

        sqlite3_bind_text(stmt, 1, templateName.c_str(), -1, SQLITE_TRANSIENT);

        std::string uuid;
        if ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
            const char* result = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
            if (result) {
                uuid = result;
            }
        }

        sqlite3_finalize(stmt);

        if (rc != SQLITE_DONE && rc != SQLITE_ROW) {
            SKSE::log::error("Error reading UUID by template name {}: {}", templateName, sqlite3_errmsg(db_));
        }

        return uuid;
    }
    
    std::string Database::GetTemplateNameByUUID(const std::string& actorUUID) {
        if (!is_open_) {
            return "";
        }

        const char* sql = "SELECT bio_template_name FROM uuid_mappings WHERE uuid = ?";
        
        sqlite3_stmt* stmt = nullptr;
        int rc = sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
        
        if (rc != SQLITE_OK) {
            SKSE::log::error("Failed to prepare template name query: {}", sqlite3_errmsg(db_));
            return "";
        }

        sqlite3_bind_text(stmt, 1, actorUUID.c_str(), -1, SQLITE_TRANSIENT);

        std::string templateName;
        if ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
            const char* result = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
            if (result) {
                templateName = result;
                SKSE::log::info("Found bio_template_name='{}' for UUID {}", templateName, actorUUID);
            }
        } else {
            SKSE::log::warn("No bio_template_name found for UUID {} in uuid_mappings table", actorUUID);
        }

        sqlite3_finalize(stmt);

        if (rc != SQLITE_DONE && rc != SQLITE_ROW) {
            SKSE::log::error("Error reading template name for UUID {}: {}", actorUUID, sqlite3_errmsg(db_));
        }

        return templateName;
    }

    std::vector<std::pair<std::string, std::string>> Database::GetActorUUIDsWithNewEntries(double sinceTime) {
        std::vector<std::pair<std::string, std::string>> results;
        
        if (!is_open_) {
            return results;
        }

        // Get all actors with diary entries created after sinceTime
        const char* sql = R"(
            SELECT DISTINCT d.actor_uuid, u.actor_name 
            FROM diary_entries d
            INNER JOIN uuid_mappings u ON d.actor_uuid = u.uuid
            WHERE d.creation_time > ?
            ORDER BY u.actor_name
        )";
        
        sqlite3_stmt* stmt = nullptr;
        int rc = sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
        
        if (rc != SQLITE_OK) {
            SKSE::log::error("Failed to prepare new entries query: {}", sqlite3_errmsg(db_));
            return results;
        }

        sqlite3_bind_double(stmt, 1, sinceTime);

        while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
            const char* uuid = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
            const char* name = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
            
            if (uuid && name) {
                results.emplace_back(uuid, name);
            }
        }

        sqlite3_finalize(stmt);

        if (rc != SQLITE_DONE && rc != SQLITE_ROW) {
            SKSE::log::error("Error reading actors with new entries: {}", sqlite3_errmsg(db_));
        }

        return results;
    }

    RE::FormID Database::GetFormIDForUUID(const std::string& actorUUID) {
        if (!is_open_) {
            return 0;
        }

        const char* sql = "SELECT form_id FROM uuid_mappings WHERE uuid = ? LIMIT 1";
        sqlite3_stmt* stmt = nullptr;
        int rc = sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
        
        if (rc != SQLITE_OK) {
            SKSE::log::error("Failed to prepare FormID query: {}", sqlite3_errmsg(db_));
            return 0;
        }

        sqlite3_bind_text(stmt, 1, actorUUID.c_str(), -1, SQLITE_TRANSIENT);

        RE::FormID formID = 0;
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            formID = static_cast<RE::FormID>(sqlite3_column_int64(stmt, 0));
        }

        sqlite3_finalize(stmt);
        return formID;
    }

} // namespace SkyrimNetDiaries
