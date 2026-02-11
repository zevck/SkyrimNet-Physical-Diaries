#pragma once

#include "PCH.h"
#include "Database.h"  // For DiaryEntry struct
#include "Config.h"    // For configuration settings

namespace SkyrimNetDiaries {

    struct DiaryBookData {
        std::string actorUuid;
        std::string actorName;
        RE::FormID bookFormId;     // The actual book FormID (unique identifier)
        double startTime;          // Unix timestamp - show entries after this (0 = beginning)
        double endTime;            // Unix timestamp - show entries before this (0 = present/no limit)
        int volumeNumber = 1;
        std::string journalTemplate;  // Which template book was used (for consistent appearance)
    };

    class BookManager {
    public:
        static BookManager* GetSingleton();

        // Initialize with template book Editor IDs from ESP
        // baseTemplate is the default, others are for variety (pass empty strings to disable variety)
        void Initialize(const std::string& baseTemplate, 
                       const std::string& journal01 = "",
                       const std::string& journal02 = "",
                       const std::string& journal03 = "",
                       const std::string& journal04 = "",
                       const std::string& nightingaleJournal = "");

        // Create a new diary book for an actor
        RE::TESObjectBOOK* CreateDiaryBook(const std::string& actorUuid, const std::string& actorName,
                                           double startTime = 0.0, double endTime = 0.0, int volumeNumber = 1,
                                           RE::FormID targetActorFormID = 0,
                                           const std::vector<DiaryEntry>& entries = {},
                                           const std::string& bioTemplateName = "");

        // Update an existing book's text from database
        bool UpdateBookText(RE::TESObjectBOOK* book, const std::string& actorUuid,
                           double startTime, double endTime);

        // Set book text directly (for runtime injection)
        void SetBookText(RE::TESObjectBOOK* book, const std::string& text);

        // Write book text to Dynamic Book Framework files
        bool WriteDynamicBookFile(const std::string& bookTitle, const std::string& text);
        bool RegisterDiaryInDBF(const std::string& bookTitle);

        // Find book by actor UUID
        // Get latest volume for an actor by UUID
        DiaryBookData* GetBookForActor(const std::string& actorUuid);

        // Get all volumes for an actor by UUID
        std::vector<DiaryBookData>* GetAllVolumesForActor(const std::string& actorUuid);

        // Get book data by FormID (searches all actors)
        DiaryBookData* GetBookForFormID(RE::FormID formId);
        
        // Get book  data by FormID (returns copy for safe access)
        std::optional<DiaryBookData> GetBookByFormID(RE::FormID formId);
        
        // Get all actor UUIDs that have tracked books
        std::vector<std::string> GetAllTrackedActorUUIDs() const;

        // Check if BookManager has any books
        bool HasAnyBooks() const { return !books_.empty(); }

        // Get all books (for checking volume numbers)
        const std::unordered_map<std::string, std::vector<DiaryBookData>>& GetAllBooks() const { return books_; }

        // Track a book creation
        void RegisterBook(const std::string& actorUuid, const std::string& actorName,
                         RE::FormID bookFormId, double startTime, double endTime, int volumeNumber,
                         const std::string& journalTemplate = "");

        // Update a book's endTime (when diary is stolen/removed)
        void UpdateBookEndTime(const std::string& actorUuid, int volumeNumber, double endTime);

        // Unregister a book (when it becomes obsolete due to deletions)
        void UnregisterBook(const std::string& actorUuid);

        // Remove a specific book by FormID (when player returns it to NPC)
        // Returns: 0 = not found, 1 = removed but newer volumes exist, 2 = removed and was latest volume
        int RemoveBookByFormID(RE::FormID formId);

        // Regenerate all diary texts from database (called on game load)
        void RegenerateAllDiaryTexts();

        // Serialization
        void Save(SKSE::SerializationInterface* a_intfc);
        void Load(SKSE::SerializationInterface* a_intfc);
        void Revert();

    private:
        BookManager() = default;
        BookManager(const BookManager&) = delete;
        BookManager& operator=(const BookManager&) = delete;

        // Select appropriate journal template for an actor
        std::string SelectJournalTemplate(const std::string& actorUuid, const std::string& actorName);

        std::string templateBookEditorId_;  // Default/base template
        std::vector<std::string> journalTemplates_;  // Additional template variants
        std::string nightingaleTemplate_;  // Special template for Nightingale NPCs
        std::unordered_map<std::string, std::string> actorTemplates_;  // UUID → template choice (persists across volumes)
        std::unordered_map<std::string, std::vector<DiaryBookData>> books_; // UUID → all volumes
    };

} // namespace SkyrimNetDiaries
