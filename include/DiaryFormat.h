#pragma once

#include "PCH.h"
#include "Database.h"

namespace SkyrimNetDiaries {

    // Helper function to format diary entries into book text
    std::string FormatDiaryEntries(const std::vector<DiaryEntry>& entries,
                                   const std::string& actorName,
                                   double startTime, double endTime);

    // Get the current database path (from main.cpp)
    std::filesystem::path GetCurrentDatabasePath();

} // namespace SkyrimNetDiaries
