#pragma once

#include "PCH.h"

// Forward-declare sqlite3 to avoid pulling the full header into every TU.
struct sqlite3;

namespace SkyrimNetDiaries {

    // ---------------------------------------------------------------------------
    // DiaryDB — persistent SQLite store for diary volume metadata + rendered text.
    //
    // Replaces the SKSE co-save for BookManager data so all diary state survives
    // save reverts (same character folder, independent of save file state).
    //
    // DB location:
    //   <cwd>/Data/SKSE/Plugins/SkyrimNetPhysicalDiaries/<saveFolder>/diary.db
    //
    // Schema (two tables):
    //   volumes       — one row per diary volume (metadata + rendered book_text)
    //   actor_templates — one row per actor UUID recording journal template choice
    // ---------------------------------------------------------------------------

    class DiaryDB {
    public:
        static DiaryDB* GetSingleton();

        // Open (or create) the DB for the given save folder.  Idempotent — a
        // second call with the same folder is a no-op; a different folder closes
        // the old connection first.  Returns false on failure.
        bool Open(const std::string& saveFolder);
        void Close();
        bool IsOpen() const { return db_ != nullptr; }

        // ── Volume operations ─────────────────────────────────────────────────

        struct VolumeRow {
            std::string  actorUuid;
            std::string  actorName;
            std::uint32_t actorFormId                  = 0;  // Actor's game FormID — more stable than UUID roundtrip
            std::uint32_t bookFormId                   = 0;
            int           volumeNumber                 = 1;
            double        startTime                    = 0.0;
            double        endTime                      = 0.0;
            std::string  journalTemplate;
            std::string  bioTemplateName;
            int           lastKnownEntryCount          = 0;
            double        prevVolumeLastCreationTime   = 0.0;
            int           prevVolumeCountAtBoundary    = 0;
            std::string  bookText;   // rendered font-tagged text ready for injection
            bool          persistedInSave              = false; // true once written into a .ess save file
        };

        // Insert-or-replace a full volume row.  If bookText is empty the existing
        // book_text column value is preserved (i.e. on a metadata-only upsert the
        // rendered text is not wiped).
        bool UpsertVolume(const VolumeRow& row);

        // Update only the rendered text and entry count (called after formatting).
        bool UpdateBookText(const std::string& actorUuid, int volumeNumber,
                            const std::string& text, int entryCount);

        // Update only end_time (called when a volume is sealed).
        bool UpdateEndTime(const std::string& actorUuid, int volumeNumber,
                           double endTime);

        // Update only book_form_id (called after DPF recreates a lost form).
        bool UpdateFormID(const std::string& actorUuid, int volumeNumber,
                          std::uint32_t formId);

        // Delete a single volume row.
        bool DeleteVolume(const std::string& actorUuid, int volumeNumber);

        // Delete all volumes (and template) for an actor.
        bool DeleteActor(const std::string& actorUuid);

        // Return all volume rows ordered by (actor_uuid, volume_number).
        std::vector<VolumeRow> LoadAllVolumes();

        // Mark every volume as persisted (call from kPostSaveGame so that the
        // inventory-check on next load doesn't re-add legitimately taken books).
        bool MarkAllVolumesPersisted();

        // ── Actor-template operations ─────────────────────────────────────────

        std::unordered_map<std::string, std::string> LoadActorTemplates();

        // ── Theft tracking ────────────────────────────────────────────────────

        // Track which specific volumes are stolen (not just a timestamp)
        bool AddStolenVolume(const std::string& actorUuid, int volumeNumber, double gameTime);
        bool RemoveStolenVolume(const std::string& actorUuid, int volumeNumber);
        bool HasAnyStolenVolumes(const std::string& actorUuid);
        bool ClearAllStolenVolumes(const std::string& actorUuid);
        double GetLastKnownGameTime(const std::string& actorUuid);
        bool UpdateLastKnownGameTime(const std::string& actorUuid, double gameTime);
        bool UpsertActorTemplate(const std::string& uuid,
                                 const std::string& templateName);

    private:
        DiaryDB() = default;
        DiaryDB(const DiaryDB&) = delete;
        DiaryDB& operator=(const DiaryDB&) = delete;

        bool EnsureSchema();
        bool Exec(const char* sql);

        sqlite3*    db_          = nullptr;
        std::string openFolder_;  // folder name the DB was opened for
    };

} // namespace SkyrimNetDiaries
