#include "Database.h"
#include "BookManager.h"
#include "BookTextHook.h"
#include "DiaryTheftHandler.h"
#include "SkyrimNetPhysicalDiariesAPI.h"
#include "Config.h"
#include "DiaryDB.h"
#include "PapyrusAPI.h"
#include "Localization.h"
#include <spdlog/sinks/basic_file_sink.h>
#include <fstream>
#include <unordered_set>
#include <unordered_map>
#include <cstring>



// Helper function to sanitize text for Skyrim's book renderer
std::string SanitizeBookText(const std::string& text) {
    std::string result = text;
        
        // Strip leading date headers/prefixes that LLM sometimes includes.
        // e.g. "# Sundas, 17th Last Seed - Late Morning"   (whole-line header, any language)
        //      "Sundas, 17th of Last Seed, 4E 201"         (whole-line English Tamrielic)
        //      "Sundas, 17th Last Seed. Today I saw..."    (inline date prefix in prose)
        // Only strip when ShowDateHeaders is enabled: we're supplying our own formatted headers
        // so LLM-generated ones would duplicate. When ShowDateHeaders is disabled the user
        // wants the LLM's dates to show through — don't strip them.
        if (SkyrimNetDiaries::Config::GetSingleton()->GetShowDateHeaders())
        {
            // Step 1: Strip any leading markdown heading line unconditionally.
            // A line starting with '#' is always AI formatting noise regardless of language.
            if (!result.empty() && result[0] == '#') {
                size_t firstNewline = result.find('\n');
                if (firstNewline != std::string::npos) {
                    result = result.substr(firstNewline + 1);
                    while (!result.empty() && (result[0] == '\n' || result[0] == '\r'))
                        result = result.substr(1);
                } else {
                    result.clear();
                }
            }

            // Step 1.5: Strip a leading line that is entirely bold markdown (**...**).
            // LLMs sometimes write "**17th of Last Seed, 4E 201 - Morning**" as a header.
            if (result.size() >= 5 && result[0] == '*' && result[1] == '*') {
                size_t nl = result.find('\n');
                size_t lineEnd = (nl != std::string::npos) ? nl : result.size();
                if (lineEnd >= 5 &&
                    result[lineEnd - 1] == '*' && result[lineEnd - 2] == '*') {
                    if (nl != std::string::npos) {
                        result = result.substr(nl + 1);
                        while (!result.empty() && (result[0] == '\n' || result[0] == '\r'))
                            result = result.substr(1);
                    } else {
                        result.clear();
                    }
                }
            }

            // Step 2: Strip plain-text date headers/prefixes the LLM sometimes writes.
            // Covers Tamrielic day/month names in all 9 supported languages, era markers,
            // and real-world time patterns. CJK day names (single kanji/short) are skipped
            // to avoid false positives — those are caught by markdown stripping in steps 1/1.5.

            // Pattern: first line is a short time string ("9:28 AM" or "9:28 AM, ...").
            // Language-independent — digits and AM/PM are ASCII in all locales.
            if (!result.empty()) {
                size_t nl = result.find('\n');
                size_t lineEnd = (nl != std::string::npos && nl < 80) ? nl : std::string::npos;
                if (lineEnd != std::string::npos) {
                    std::string_view firstLine(result.c_str(), lineEnd);
                    // Match: optional whitespace, 1-2 digits, colon, 2 digits, space, AM/PM
                    size_t i = 0;
                    while (i < firstLine.size() && firstLine[i] == ' ') ++i;
                    size_t numStart = i;
                    while (i < firstLine.size() && std::isdigit((unsigned char)firstLine[i])) ++i;
                    bool isTime = (i > numStart && i < firstLine.size() && firstLine[i] == ':');
                    if (isTime) {
                        ++i; // skip ':'
                        size_t minStart = i;
                        while (i < firstLine.size() && std::isdigit((unsigned char)firstLine[i])) ++i;
                        if (i == minStart + 2 && i < firstLine.size() && firstLine[i] == ' ') {
                            ++i;
                            if (firstLine.substr(i, 2) == "AM" || firstLine.substr(i, 2) == "PM") {
                                result = result.substr(lineEnd + 1);
                                while (!result.empty() && (result[0] == '\n' || result[0] == '\r'))
                                    result = result.substr(1);
                            }
                        }
                    }
                }
            }

            // Build date word list dynamically from loaded locale data.
            // Includes all month/day names from the active locale + English fallbacks.
            auto* locInst = SkyrimNetDiaries::Localization::GetSingleton();
            std::vector<std::string> skyrimDateWords;
            skyrimDateWords.reserve(40);

            // Always include English day/month names (LLMs often default to English)
            for (auto& d : {"Sundas", "Morndas", "Tirdas", "Middas", "Turdas", "Fredas", "Loredas"})
                skyrimDateWords.emplace_back(d);
            for (auto& m : {"Morning Star", "Sun's Dawn", "First Seed", "Rain's Hand",
                            "Second Seed", "Midyear", "Sun's Height", "Last Seed",
                            "Hearthfire", "Frostfall", "Sun's Dusk", "Evening Star"})
                skyrimDateWords.emplace_back(m);

            // Add loaded locale's day/month names (may be same as English, GMST, or custom)
            for (int i = 0; i < 7; ++i) {
                const auto& dn = locInst->GetDayName(i);
                // Skip single-character day names (CJK) — too common for safe matching
                if (dn.size() > 3) skyrimDateWords.push_back(dn);
            }
            for (int i = 0; i < 12; ++i) {
                skyrimDateWords.push_back(locInst->GetMonthName(i));
            }

            // Era markers
            skyrimDateWords.emplace_back("4E ");
            skyrimDateWords.emplace_back("4\xd0\xad ");  // 4Э (Russian)
            skyrimDateWords.emplace_back("\xe7\xac\xac\xe5\x9b\x9b\xe7\xb4\x80");  // 第四紀 (Japanese/Chinese)

            std::string_view view(result.c_str(), result.size());
            const std::string* matchedWord = nullptr;
            for (const auto& word : skyrimDateWords) {
                if (view.substr(0, word.size()) == word) {
                    matchedWord = &word;
                    break;
                }
            }

            // Ordinal-first fallback: "17th of Last Seed", "3rd Hearthfire", etc.
            // Pattern: 1-2 digits + (st|nd|rd|th) + space + optional "of " + <month>
            if (!matchedWord && !result.empty() && std::isdigit((unsigned char)result[0])) {
                size_t i = 0;
                while (i < result.size() && std::isdigit((unsigned char)result[i])) ++i;
                if (i >= 1 && i <= 2 && i + 3 <= result.size()) {
                    std::string_view suf = view.substr(i, 3);
                    if (suf == "st " || suf == "nd " || suf == "rd " || suf == "th ") {
                        i += 3;
                        if (i + 3 <= result.size() && view.substr(i, 3) == "of ") i += 3;
                        for (const auto& word : skyrimDateWords) {
                            if (view.size() >= i + word.size() &&
                                view.substr(i, word.size()) == word) {
                                matchedWord = &word;
                                break;
                            }
                        }
                    }
                }
            }

            if (matchedWord) {
                size_t firstNewline = result.find('\n');
                bool isWholeLine = (firstNewline != std::string::npos && firstNewline < 120);

                if (isWholeLine) {
                    // The whole first line is a date header — strip the line and
                    // any following blank lines.
                    result = result.substr(firstNewline + 1);
                    while (!result.empty() && (result[0] == '\n' || result[0] == '\r')) {
                        result = result.substr(1);
                    }
                } else {
                    // The date is a prefix embedded in prose ("Sundas, 17th Last Seed. Today...").
                    // Strip up to and including the first sentence-ending punctuation followed
                    // by whitespace so the prose content is preserved.
                    size_t searchFrom = matchedWord->size();
                    size_t breakPos = std::string::npos;
                    for (size_t i = searchFrom; i + 1 < result.size(); ++i) {
                        char c = result[i];
                        char next = result[i + 1];
                        if ((c == '.' || c == '!' || c == '?') && (next == ' ' || next == '\n')) {
                            breakPos = i + 2; // skip the punctuation and the space/newline
                            break;
                        }
                    }
                    if (breakPos != std::string::npos && breakPos < 200) {
                        result = result.substr(breakPos);
                        // Capitalise the first character of the remaining prose if needed
                        if (!result.empty() && std::islower((unsigned char)result[0])) {
                            result[0] = (char)std::toupper((unsigned char)result[0]);
                        }
                    }
                    // If no sentence break found within 200 chars, leave content untouched
                    // (safer than stripping potentially good content)
                }
            }
        }
        
        // Replace em dashes (U+2014) with single hyphen
        size_t pos = 0;
        while ((pos = result.find("\xE2\x80\x94", pos)) != std::string::npos) {
            result.replace(pos, 3, "-");
            pos += 1;
        }
    
    // Replace en dashes (U+2013) with single hyphen
    pos = 0;
    while ((pos = result.find("\xE2\x80\x93", pos)) != std::string::npos) {
        result.replace(pos, 3, "-");
        pos += 1;
    }
    
    // Replace curly quotes with straight quotes
    // Left double quote (U+201C)
    pos = 0;
    while ((pos = result.find("\xE2\x80\x9C", pos)) != std::string::npos) {
        result.replace(pos, 3, "\"");
        pos += 1;
    }
    // Right double quote (U+201D)
    pos = 0;
    while ((pos = result.find("\xE2\x80\x9D", pos)) != std::string::npos) {
        result.replace(pos, 3, "\"");
        pos += 1;
    }
    // Left single quote (U+2018)
    pos = 0;
    while ((pos = result.find("\xE2\x80\x98", pos)) != std::string::npos) {
        result.replace(pos, 3, "'");
        pos += 1;
    }
    // Right single quote (U+2019)
    pos = 0;
    while ((pos = result.find("\xE2\x80\x99", pos)) != std::string::npos) {
        result.replace(pos, 3, "'");
        pos += 1;
    }
    
    // Replace ellipsis (U+2026) with three periods
    pos = 0;
    while ((pos = result.find("\xE2\x80\xA6", pos)) != std::string::npos) {
        result.replace(pos, 3, "...");
        pos += 3;
    }

    // Strip Markdown formatting characters that pass through raw as asterisks/underscores.
    // **bold** and *italic* → just the inner text (Skyrim book HTML uses <b>/<i> if needed,
    // but the handwriting font rarely has bold/italic variants so stripping is cleanest).
    // Process ** before * to avoid partially matching bold markers as italic.
    {
        std::string out;
        out.reserve(result.size());
        const char* s = result.c_str();
        while (*s) {
            if (s[0] == '*' && s[1] == '*') {
                // Skip the opening **; scan forward for closing **
                s += 2;
            } else if (s[0] == '*') {
                // Skip a lone *
                s += 1;
            } else if (s[0] == '_' && s[1] == '_') {
                s += 2;
            } else if (s[0] == '_'
                       && (s > result.c_str() && (*(s-1) == ' ' || *(s-1) == '\n'))
                       && s[1] != ' ') {
                // Only strip leading _ used as italic marker (surrounded by spaces/newlines),
                // not underscores mid-word (e.g. snake_case variable names in narration).
                s += 1;
            } else {
                out += *s++;
            }
        }
        result = std::move(out);
    }

    return result;
}

// Helper function to convert game time to readable date
std::string FormatGameDate(double gameTime) {
    // Game start: 17 Last Seed, 4E 201 (Sundas)
    // gameTime appears to be in seconds since game start
    // Convert to days: 60 seconds/min * 60 min/hour * 24 hours/day = 86400 seconds/day
    int totalDays = static_cast<int>(gameTime / 86400.0);
    
    const int startDay = 17;
    const int startMonth = 7; // Last Seed (0-indexed)
    const int startYear = 201;
    const int startDayOfWeek = 0; // Sundas
    
    auto* loc = SkyrimNetDiaries::Localization::GetSingleton();

    // Calculate absolute day number from game start (17 Last Seed)
    int absoluteDay = startDay + totalDays;
    int currentMonth = startMonth;
    int currentYear = startYear;

    // Handle month/year underflow (negative totalDays — e.g. test entries before game start)
    while (absoluteDay <= 0) {
        absoluteDay += 30;
        currentMonth--;
        if (currentMonth < 0) {
            currentMonth = 11;
            currentYear--;
        }
    }
    // Handle month/year overflow
    while (absoluteDay > 30) {
        absoluteDay -= 30;
        currentMonth++;
        if (currentMonth >= 12) {
            currentMonth = 0;
            currentYear++;
        }
    }

    // Calculate day of week from start day — use +7 to keep result non-negative
    int dayOfWeek = ((startDayOfWeek + totalDays) % 7 + 7) % 7;

    return loc->FormatDateLong(loc->GetDayName(dayOfWeek).c_str(), absoluteDay,
                               loc->GetMonthName(currentMonth).c_str(), currentYear);
}

std::string FormatGameDateShort(double gameTime) {
    // Same as FormatGameDate but without day of week - for title page
    int totalDays = static_cast<int>(gameTime / 86400.0);

    const int startDay = 17;
    const int startMonth = 7; // Last Seed (0-indexed)
    const int startYear = 201;

    auto* loc = SkyrimNetDiaries::Localization::GetSingleton();

    // Calculate absolute day number from game start (17 Last Seed)
    int absoluteDay = startDay + totalDays;
    int currentMonth = startMonth;
    int currentYear = startYear;

    // Handle month/year underflow (negative totalDays)
    while (absoluteDay <= 0) {
        absoluteDay += 30;
        currentMonth--;
        if (currentMonth < 0) {
            currentMonth = 11;
            currentYear--;
        }
    }
    // Handle month/year overflow
    while (absoluteDay > 30) {
        absoluteDay -= 30;
        currentMonth++;
        if (currentMonth >= 12) {
            currentMonth = 0;
            currentYear++;
        }
    }

    return loc->FormatDateShort(absoluteDay, loc->GetMonthName(currentMonth).c_str(), currentYear);
}

namespace
{
    // Cache the current save's folder name (e.g., "SkyrimNet-1772379483115-796523")
    std::string g_currentSaveFolder;

    constexpr std::uint32_t kSerializationVersion = 3;
    constexpr std::uint32_t kSerializationTypeBooks = 'SNDB'; // SkyrimNet Diary Books
    // Retired 'SNDC' records may still exist in older saves; LoadCallback skips them.
    constexpr std::uint32_t kSerializationTypeFolder = 'SNDF'; // SkyrimNet Diary Folder (save-specific database name)

} // end anonymous namespace

// Parse SkyrimNet.log to detect the current save folder (more reliable than filesystem scan).
// Finds the most recent "Using save ID: " entry, verifies the .db file exists, and caches
// the result in g_currentSaveFolder. Returns "" on failure.
std::string DetectSaveFolderFromLog() {
    if (!g_currentSaveFolder.empty()) {
        return g_currentSaveFolder;
    }

    try {
        auto logDir = SKSE::log::log_directory();
        if (!logDir) {
            SKSE::log::warn("DetectSaveFolderFromLog: could not get SKSE log directory");
            return "";
        }

        auto logPath = *logDir / "SkyrimNet.log";
        if (!std::filesystem::exists(logPath)) {
            SKSE::log::warn("DetectSaveFolderFromLog: SkyrimNet.log not found at {}", logPath.string());
            return "";
        }

        std::ifstream file(logPath);
        if (!file.is_open()) {
            SKSE::log::warn("DetectSaveFolderFromLog: could not open {}", logPath.string());
            return "";
        }

        // Scan all lines, keeping the LAST "Using save ID: " occurrence
        static const std::string marker = "Using save ID: ";
        std::string lastSaveId;
        std::string line;
        while (std::getline(file, line)) {
            auto pos = line.find(marker);
            if (pos != std::string::npos) {
                lastSaveId = line.substr(pos + marker.length());
                // Trim trailing whitespace / CR
                while (!lastSaveId.empty() &&
                       (lastSaveId.back() == '\r' || lastSaveId.back() == '\n' || lastSaveId.back() == ' ')) {
                    lastSaveId.pop_back();
                }
            }
        }

        if (lastSaveId.empty()) {
            SKSE::log::warn("DetectSaveFolderFromLog: no 'Using save ID' line found in log");
            return "";
        }

        // Verify the corresponding .db file actually exists
        std::string folderName = "SkyrimNet-" + lastSaveId;
        auto dbPath = std::filesystem::current_path() / "Data" / "SKSE" / "Plugins" / "SkyrimNet" / "data" / (folderName + ".db");
        if (!std::filesystem::exists(dbPath)) {
            SKSE::log::warn("DetectSaveFolderFromLog: save ID '{}' found in log but DB not found at {}",
                            lastSaveId, dbPath.string());
            return "";
        }

        g_currentSaveFolder = folderName;
        SKSE::log::info("DetectSaveFolderFromLog: detected save folder '{}'", g_currentSaveFolder);
        return g_currentSaveFolder;

    } catch (const std::exception& e) {
        SKSE::log::error("DetectSaveFolderFromLog exception: {}", e.what());
        return "";
    }
}

// Format diary entries - accessible from BookManager
// maxEntries: Maximum number of entries to include in this book (default 10)
std::string FormatDiaryEntries(const std::vector<SkyrimNetDiaries::DiaryEntry>& entries,
                               const std::string& actorName,
                               double startTime, double endTime,
                               int maxEntries = 10) {
    std::string bookText;
    auto config = SkyrimNetDiaries::Config::GetSingleton();
    int fontTitle = config->GetFontSizeTitle();
    int fontDate = config->GetFontSizeDate();
    int fontContent = config->GetFontSizeContent();
    int fontSmall = config->GetFontSizeSmall();
    std::string fontFace = config->GetFontFace();

    // Blank first page
    bookText = "[pagebreak]\n\n";

    // Title page — handwriting font, centred; leading newlines push it down visually
    bookText += "\n\n\n\n\n\n";
    bookText += "<font face='" + fontFace + "' size='" + std::to_string(fontTitle) + "'><p align='center'>";
    auto* loc = SkyrimNetDiaries::Localization::GetSingleton();
    bookText += loc->FormatDiaryTitle(actorName);
    bookText += "</p></font>\n\n";

    if (entries.empty()) {
        bookText += "[pagebreak]\n\n";
        bookText += "<font face='" + fontFace + "' size='" + std::to_string(fontContent) + "'><p align='center'>"
                  + std::string(SkyrimNetDiaries::Localization::kEmptySentinel)
                  + loc->GetEmptyVolumeText() + "</p></font>";
    } else {
        // Date range below title
        std::string firstDate = FormatGameDateShort(entries.front().entry_date);
        std::string lastDate = FormatGameDateShort(entries.back().entry_date);

        bookText += "<font face='" + fontFace + "' size='" + std::to_string(fontSmall) + "'><p align='center'>";
        if (firstDate == lastDate) {
            bookText += firstDate;
        } else {
            bookText += firstDate + " - " + lastDate;
        }
        bookText += "</p></font>\n\n";

        // Page break — entries start on the next page
        bookText += "[pagebreak]\n\n";
        
        int entriesIncluded = 0;
        
        for (size_t i = 0; i < entries.size() && entriesIncluded < maxEntries; ++i) {
            const auto& entry = entries[i];
            
            // Filter by time range if specified
            if (startTime > 0.0 && entry.entry_date < startTime) continue;
            if (endTime > 0.0 && entry.entry_date > endTime) continue;
            
            entriesIncluded++;
            
            // Format the date string
            std::string dateStr = FormatGameDate(entry.entry_date);
            
            // Date header (optional — controlled by ShowDateHeaders config)
            if (SkyrimNetDiaries::Config::GetSingleton()->GetShowDateHeaders()) {
                // Reset font size explicitly (title page font might bleed through pagebreak)
                bookText += "<font face='" + fontFace + "' size='" + std::to_string(fontDate) + "'></font>";
                bookText += "<font face='" + fontFace + "' size='" + std::to_string(fontDate) + "'>" + dateStr + "</font>";
                bookText += "<font face='" + fontFace + "' size='" + std::to_string(fontContent) + "'></font>\n\n";  // Reset to content font size
            }
            
            // Entry content - wrap EACH paragraph in font tag since Skyrim resets after \n\n
            std::string content = SanitizeBookText(entry.content);

            // Split by double newlines (paragraph breaks) and wrap each
            size_t pos = 0;
            size_t found;
            while ((found = content.find("\n\n", pos)) != std::string::npos) {
                std::string paragraph = content.substr(pos, found - pos);
                if (!paragraph.empty()) {
                    bookText += "<font face='" + fontFace + "' size='" + std::to_string(fontContent) + "'>" + paragraph + "</font>\n\n";
                }
                pos = found + 2;
            }
            // Last paragraph
            std::string lastParagraph = content.substr(pos);
            if (!lastParagraph.empty()) {
                bookText += "<font face='" + fontFace + "' size='" + std::to_string(fontContent) + "'>" + lastParagraph + "</font>\n\n";
            }
            
            bookText += "\n\n";
            
            // Pagebreak between entries
            if (i < entries.size() - 1) {
                bookText += "[pagebreak]\n\n";
            }
        }
    }
    
    return bookText;
}


// =============================================================================
// Create all diary volumes for an actor from a flat list of entries.
// Entries are sorted oldest-first and chunked into EntriesPerVolume batches.
// startingVolumeNumber is 1 for a fresh actor, or latestVolume+1 for additions.
// =============================================================================

void CreateAllVolumesForActor(
    const std::string& uuid,
    const std::string& actorName,
    RE::FormID formId,
    const std::string& bioTemplateName,
    std::vector<SkyrimNetDiaries::DiaryEntry> allEntries,
    int startingVolumeNumber)
{
    if (allEntries.empty()) return;

    // Sort oldest-first by (entry_date, creation_time).
    std::sort(allEntries.begin(), allEntries.end(),
        [](const SkyrimNetDiaries::DiaryEntry& a, const SkyrimNetDiaries::DiaryEntry& b) {
            if (a.entry_date != b.entry_date) return a.entry_date < b.entry_date;
            return a.creation_time < b.creation_time;
        });

    const int chunkSize = SkyrimNetDiaries::Config::GetSingleton()->GetEntriesPerVolume();
    auto bookManager = SkyrimNetDiaries::BookManager::GetSingleton();
    int volumeNumber = startingVolumeNumber;

    for (size_t offset = 0; offset < allEntries.size(); offset += chunkSize, ++volumeNumber) {
        size_t end = std::min(offset + static_cast<size_t>(chunkSize), allEntries.size());
        std::vector<SkyrimNetDiaries::DiaryEntry> chunk(allEntries.begin() + offset,
                                                        allEntries.begin() + end);

        // The creation_time of the last entry in the previous chunk is used by GetDiaryEntries
        // to exclude it from this volume when both volumes share the same entry_date boundary.
        double prevChunkLastCreationTime = (offset == 0) ? 0.0 : allEntries[offset - 1].creation_time;

        // Count how many entries in the previous chunk share the boundary date with this chunk's
        // first entry.  Those entries would be returned by the API query for this volume (because
        // their entry_date >= this volume's startTime) but must be excluded.  Storing the exact
        // count prevents over-removal when two entries are truly identical (same entry_date AND
        // creation_time), which is the root cause of the "entry #10 missing" bug.
        int prevChunkCountAtBoundary = 0;
        if (offset > 0) {
            double boundaryDate = chunk.front().entry_date;
            for (int i = static_cast<int>(offset) - 1; i >= 0 && allEntries[i].entry_date == boundaryDate; --i) {
                ++prevChunkCountAtBoundary;
            }
        }

        // volume 1 uses 0.0 (no lower bound); later volumes start at their first entry's date,
        // which under normal circumstances is strictly greater than the previous chunk's last date.
        double volStart = (volumeNumber == 1) ? 0.0 : chunk.front().entry_date;
        double volEnd   = chunk.back().entry_date;

        SKSE::log::debug("Creating volume {} for {} ({} entries, {:.2f}\u2013{:.2f})",
                       volumeNumber, actorName, chunk.size(), volStart, volEnd);

        bookManager->CreateDiaryBook(uuid, actorName, volStart, volEnd,
                                     volumeNumber, formId, chunk, bioTemplateName,
                                     prevChunkLastCreationTime, prevChunkCountAtBoundary);
    }
}

// =============================================================================
// ModEvent-triggered diary update for single actor
// =============================================================================

void UpdateDiaryForActorInternal(RE::FormID formId) {
    SKSE::log::debug("=== UpdateDiaryForActorInternal called for FormID 0x{:X} ===", formId);
    
    // Initialize SkyrimNet API if not already done
    if (!SkyrimNetDiaries::Database::InitializeAPI()) {
        SKSE::log::error("Failed to initialize SkyrimNet API - diary update skipped");
        return;
    }
    
    // Check if memory system is ready
    if (!SkyrimNetDiaries::Database::IsMemorySystemReady()) {
        SKSE::log::warn("SkyrimNet memory system not ready yet - diary update deferred");
        return;
    }
    
    try {
        auto bookManager = SkyrimNetDiaries::BookManager::GetSingleton();
        std::string uuid = SkyrimNetDiaries::Database::GetUUIDFromFormID(formId);
        
        if (uuid.empty() || uuid == "0") {
            SKSE::log::warn("Could not resolve FormID 0x{:X} to UUID - skipping update", formId);
            return;
        }
        
        std::string actorName = SkyrimNetDiaries::Database::GetActorName(uuid);
        // Fallback: if SkyrimNet hasn't registered this NPC yet, use the RE game name directly.
        if (actorName.empty()) {
            if (auto* actor = RE::TESForm::LookupByID<RE::Actor>(formId)) {
                actorName = actor->GetName();
            }
        }
        if (actorName.empty()) {
            SKSE::log::warn("UpdateDiaryForActor: could not resolve actor name for FormID 0x{:X}, skipping", formId);
            return;
        }
        SKSE::log::debug("Processing diary update for {} (UUID: {})", actorName, uuid);
        
        auto latestVolume = bookManager->GetBookForActor(uuid);
        
        if (!latestVolume) {
            // ----------------------------------------------------------------
            // No volumes at all — fetch every entry ever written and build all
            // volumes from the beginning in chronological order.
            // ----------------------------------------------------------------
            auto allEntries = SkyrimNetDiaries::Database::GetDiaryEntries(formId, 10000, 0.0, 0.0);
            
            if (allEntries.empty()) {
                SKSE::log::debug("No diary entries for {} (UUID: {})", actorName, uuid);
                return;
            }
            
            SKSE::log::info("First-time init for {}: {} total entries → creating all volumes from 1",
                           actorName, allEntries.size());
            
            std::string bioTemplateName = SkyrimNetDiaries::Database::GetTemplateNameByUUID(uuid);
            CreateAllVolumesForActor(uuid, actorName, formId, bioTemplateName, std::move(allEntries), 1);
            return;
        }
        
        // Volumes exist — only care about entries strictly newer than the latest volume's end.
        // Backfill bioTemplateName for saves loaded from an older co-save that didn't store it.
        // GetBookForActor returns a raw pointer into the stored vector — safe to mutate on the game thread.
        if (latestVolume->bioTemplateName.empty()) {
            latestVolume->bioTemplateName = SkyrimNetDiaries::Database::GetTemplateNameByUUID(uuid);
            if (!latestVolume->bioTemplateName.empty()) {
                SKSE::log::debug("Backfilled bioTemplateName '{}' for {} (migrated save)",
                               latestVolume->bioTemplateName, actorName);
            }
        }

        // GetDiaryEntries startTime is inclusive, so we fetch from endTime and then strip
        // any entries whose timestamp is <= endTime (they belong to the previous volume).
        auto newEntries = SkyrimNetDiaries::Database::GetDiaryEntries(formId, 10000, latestVolume->endTime, 0.0);
        newEntries.erase(
            std::remove_if(newEntries.begin(), newEntries.end(),
                [&](const SkyrimNetDiaries::DiaryEntry& e) { return e.entry_date <= latestVolume->endTime; }),
            newEntries.end());

        if (newEntries.empty()) {
            // No entries after the recorded endTime.  If this volume was never persisted
            // to a save and its endTime is ahead of current game-time (stale from a
            // quit-without-save + reload scenario), do a full rebuild from all entries.
            if (!latestVolume->persistedInSave) {
                auto calendar = RE::Calendar::GetSingleton();
                double currentTime = calendar ? calendar->GetCurrentGameTime() * 86400.0 : 0.0;
                if (latestVolume->endTime > currentTime) {
                    SKSE::log::info("Stale endTime {:.2f} > currentTime {:.2f} for {} — rebuilding from all entries",
                                   latestVolume->endTime, currentTime, actorName);
                    auto bookManager2 = SkyrimNetDiaries::BookManager::GetSingleton();
                    bookManager2->UnregisterBook(uuid);
                    auto allEntries = SkyrimNetDiaries::Database::GetDiaryEntries(formId, 10000, 0.0, 0.0);
                    if (!allEntries.empty()) {
                        std::string bioTemplateName = SkyrimNetDiaries::Database::GetTemplateNameByUUID(uuid);
                        CreateAllVolumesForActor(uuid, actorName, formId, bioTemplateName, std::move(allEntries), 1);
                    }
                    return;
                }
            }
            SKSE::log::debug("No new entries for {} since {:.2f}", actorName, latestVolume->endTime);
            return;
        }
        
        SKSE::log::debug("Found {} new entries for {} since {:.2f}", newEntries.size(), actorName, latestVolume->endTime);
        
        auto npcActor = RE::TESForm::LookupByID<RE::Actor>(formId);
        if (!npcActor) {
            SKSE::log::warn("Actor 0x{:X} not found - cannot update diary", formId);
            return;
        }

        // Check if NPC still has the latest volume
        bool npcHasBook = false;
        auto inv = npcActor->GetInventory();
        for (const auto& [item, invData] : inv) {
            if (item->GetFormID() == latestVolume->bookFormId && invData.first > 0) {
                npcHasBook = true;
                break;
            }
        }
        
        if (!npcHasBook) {
            // Player took it or it was stolen — start a fresh volume.
            SKSE::log::info("{} no longer has volume {} — creating new volumes from {} ({} new entries)",
                           actorName, latestVolume->volumeNumber, latestVolume->volumeNumber + 1, newEntries.size());
            std::string bioTemplateName = SkyrimNetDiaries::Database::GetTemplateNameByUUID(uuid);
            CreateAllVolumesForActor(uuid, actorName, formId, bioTemplateName, std::move(newEntries),
                                     latestVolume->volumeNumber + 1);
            return;
        }
        
        // NPC has the book — check if it's full
        const int MAX_ENTRIES = SkyrimNetDiaries::Config::GetSingleton()->GetEntriesPerVolume();
        auto currentVolumeEntries = SkyrimNetDiaries::Database::GetDiaryEntries(
            formId, MAX_ENTRIES + 1, latestVolume->startTime, 0.0);

        if (static_cast<int>(currentVolumeEntries.size()) >= MAX_ENTRIES) {
            // Volume is full — seal it with exactly MAX_ENTRIES entries, writing the final text,
            // then route everything strictly after the cut timestamp into new overflow volumes.

            // Sort oldest → newest before sealing.
            std::sort(currentVolumeEntries.begin(), currentVolumeEntries.end(),
                [](const SkyrimNetDiaries::DiaryEntry& a, const SkyrimNetDiaries::DiaryEntry& b) {
                    if (a.entry_date != b.entry_date) return a.entry_date < b.entry_date;
                    return a.creation_time < b.creation_time;
                });

            // Finalized slice: exactly the first MAX_ENTRIES entries.
            std::vector<SkyrimNetDiaries::DiaryEntry> finalizedEntries(
                currentVolumeEntries.begin(),
                currentVolumeEntries.begin() + MAX_ENTRIES);
            double cutTime = finalizedEntries.back().entry_date;

            // Write the sealed volume with its complete, final content.
            std::string bookName = SkyrimNetDiaries::Localization::GetSingleton()->FormatBookName(actorName, latestVolume->volumeNumber);
            std::string sealedText = FormatDiaryEntries(finalizedEntries, actorName,
                                                        latestVolume->startTime, cutTime, MAX_ENTRIES);
            SkyrimNetDiaries::DiaryDB::GetSingleton()->UpdateBookText(
                uuid, latestVolume->volumeNumber, sealedText, MAX_ENTRIES);
            latestVolume->cachedBookText = sealedText;
            bookManager->UpdateBookEndTime(uuid, latestVolume->volumeNumber, cutTime);
            bookManager->UpdateVolumeEntryCount(uuid, latestVolume->volumeNumber, MAX_ENTRIES);

            // Overflow: entries in currentVolumeEntries beyond MAX_ENTRIES.
            // Note: currentVolumeEntries already includes both old and new entries, so we don't
            // need to separately add from newEntries (doing so would duplicate entries).
            std::vector<SkyrimNetDiaries::DiaryEntry> overflowEntries;
            for (size_t oi = static_cast<size_t>(MAX_ENTRIES); oi < currentVolumeEntries.size(); ++oi) {
                overflowEntries.push_back(currentVolumeEntries[oi]);
            }

            SKSE::log::info("{} volume {} sealed at {} entries (cutTime {:.2f}), {} overflow entries → creating new volumes",
                           actorName, latestVolume->volumeNumber, MAX_ENTRIES, cutTime, overflowEntries.size());

            if (!overflowEntries.empty()) {
                std::string bioTemplateName = SkyrimNetDiaries::Database::GetTemplateNameByUUID(uuid);
                CreateAllVolumesForActor(uuid, actorName, formId, bioTemplateName, std::move(overflowEntries),
                                         latestVolume->volumeNumber + 1);
            }
        } else {
            // Update the current volume in place
            std::string bookName = SkyrimNetDiaries::Localization::GetSingleton()->FormatBookName(actorName, latestVolume->volumeNumber);
            
            // Sort current volume entries oldest-first before formatting.
            std::sort(currentVolumeEntries.begin(), currentVolumeEntries.end(),
                [](const SkyrimNetDiaries::DiaryEntry& a, const SkyrimNetDiaries::DiaryEntry& b) {
                    if (a.entry_date != b.entry_date) return a.entry_date < b.entry_date;
                    return a.creation_time < b.creation_time;
                });
            
            double newEndTime = currentVolumeEntries.back().entry_date;
            std::string bookText = FormatDiaryEntries(currentVolumeEntries, actorName,
                                                      latestVolume->startTime, newEndTime, MAX_ENTRIES);
            int newEntryCount = static_cast<int>(currentVolumeEntries.size());
            SkyrimNetDiaries::DiaryDB::GetSingleton()->UpdateBookText(
                uuid, latestVolume->volumeNumber, bookText, newEntryCount);
            latestVolume->cachedBookText = bookText;

            bookManager->UpdateBookEndTime(uuid, latestVolume->volumeNumber, newEndTime);
            bookManager->UpdateVolumeEntryCount(uuid, latestVolume->volumeNumber, newEntryCount);
            
            SKSE::log::info("Updated {} volume {} with {} entries",
                           actorName, latestVolume->volumeNumber, currentVolumeEntries.size());
        }
        
    } catch (const std::exception& e) {
        SKSE::log::error("Exception in UpdateDiaryForActor: {}", e.what());
    } catch (...) {
        SKSE::log::error("Unknown exception in UpdateDiaryForActor");
    }
}

// =============================================================================
// Queued batch catch-up scan used on save load.
//
// Pass 1 (discovery): Calls PublicGetDiaryEntries(formId=0) in pages of 50
//   entries to cheaply discover which actor UUIDs have any diary content,
//   without loading every entry into memory at once.  Each page is one game-
//   thread task, chained until fewer than 50 raw entries are returned.
//
// Pass 2 (per-actor fetch): Once discovery finishes, one task per actor does a
//   full GetDiaryEntries call for just that FormID, creates all its volumes,
//   then frees the memory.  Tasks run on successive game-thread ticks so the
//   load is spread out.
//
// The whole scan is skipped if any volumes are already tracked (i.e. this save
// has been loaded before with the mod active).
// =============================================================================

// Shared state carried across discovery batch tasks via shared_ptr.
struct DiscoveryState {
    std::unordered_map<std::string, std::string> actorUuidToName; // uuid -> name
    double oldestTimestampSeen = 0.0; // lower bound for next page query
    std::unordered_set<std::string> skip;  // UUIDs already queued for immediate recovery
};

// Forward declaration so QueueBatchCatchUpScan can reference it.
void RunDiscoveryBatch(std::shared_ptr<DiscoveryState> state);


// =============================================================================
// QueueSealedVolumeRecovery — detect entries written after a sealed volume
// (the revert+KEEP scenario: SkyrimNet retains entries our DB didn't track).
// For each actor whose latest volume is sealed, probes SkyrimNet for any entry
// strictly after the seal timestamp. If found, queues UpdateDiaryForActorInternal.
// =============================================================================
void QueueSealedVolumeRecovery(const std::unordered_set<std::string>& skipUuids = {}) {
    const auto& allBooks = SkyrimNetDiaries::BookManager::GetSingleton()->GetAllBooks();
    int recoveryCount = 0;

    for (const auto& [uuid, volumes] : allBooks) {
        if (volumes.empty()) continue;
        if (skipUuids.count(uuid)) continue;  // already queued for immediate recovery

        // Find the latest volume.
        const SkyrimNetDiaries::DiaryBookData* latest = nullptr;
        for (const auto& vol : volumes) {
            if (!latest || vol.volumeNumber > latest->volumeNumber) {
                latest = &vol;
            }
        }
        if (!latest) continue;

        uint32_t actorFormId = SkyrimNetDiaries::Database::GetFormIDForUUID(uuid);
        if (actorFormId == 0) continue;

        if (latest->persistedInSave == false) {
            // Volume was never committed to a .ess save.  If the current game-time
            // is earlier than the volume's endTime the player reverted to an older
            // save and the volume's time range is now stale — force a rebuild so
            // new entries at the reverted game-time aren't silently dropped.
            auto calendar = RE::Calendar::GetSingleton();
            if (calendar && latest->endTime > 0.0) {
                double currentTime = calendar->GetCurrentGameTime() * 86400.0;
                if (latest->endTime > currentTime) {
                    SKSE::log::info("[Recovery] {} vol {} not persisted and endTime {:.2f} > current {:.2f} — queuing forced rebuild",
                                   latest->actorName, latest->volumeNumber, latest->endTime, currentTime);
                    RE::FormID fid = static_cast<RE::FormID>(actorFormId);
                    SKSE::GetTaskInterface()->AddTask([fid]() {
                        UpdateDiaryForActorInternal(fid);
                    });
                    ++recoveryCount;
                    continue;
                }
            }
        }

        if (latest->endTime > 0.0) {
            // Sealed volume: probe for any entry strictly after the seal timestamp.
            // This handles the revert+KEEP scenario where SkyrimNet retained entries
            // our DB didn't track.
            auto checkEntries = SkyrimNetDiaries::Database::GetDiaryEntries(
                actorFormId, 1, latest->endTime + 0.001, 0.0);

            if (!checkEntries.empty()) {
                SKSE::log::info("[Recovery] {} vol {} sealed at {:.2f} but new entries exist — queuing update",
                               latest->actorName, latest->volumeNumber, latest->endTime);
                RE::FormID fid = static_cast<RE::FormID>(actorFormId);
                SKSE::GetTaskInterface()->AddTask([fid]() {
                    UpdateDiaryForActorInternal(fid);
                });
                ++recoveryCount;
            }
        } else {
            // Open volume (endTime == 0): check whether SkyrimNet wrote new entries
            // while the game was paused (e.g. via dashboard) that the mod event never
            // delivered.  Use the volume's startTime as the lower bound — anything
            // strictly after the latest tracked entry_date is new.
            // We fetch 2 entries from startTime so we can count how many the current
            // volume already accounts for vs how many now exist.
            auto allSinceStart = SkyrimNetDiaries::Database::GetDiaryEntries(
                actorFormId, 10000, latest->startTime, 0.0);

            int liveCount = static_cast<int>(allSinceStart.size());

            // Compare against lastKnownEntryCount — this is the count from the last
            // time UpdateBookText ran, persisted in DiaryDB.  Any positive delta means
            // SkyrimNet wrote entries (e.g. via dashboard while paused) that the book
            // text doesn't yet include.  This catches both the sub-overflow case
            // (new entries but still under maxPerVolume) and the overflow case.
            if (liveCount > latest->lastKnownEntryCount) {
                SKSE::log::info("[Recovery] {} vol {} (open) has {} live entries vs {} known — queuing update",
                               latest->actorName, latest->volumeNumber, liveCount,
                               latest->lastKnownEntryCount);
                RE::FormID fid = static_cast<RE::FormID>(actorFormId);
                SKSE::GetTaskInterface()->AddTask([fid]() {
                    UpdateDiaryForActorInternal(fid);
                });
                ++recoveryCount;
            }
        }
    }

    if (recoveryCount > 0) {
        SKSE::log::info("[Recovery] Queued {} actor(s) for sealed-volume recovery", recoveryCount);
    }
}

void QueueBatchCatchUpScan(std::unordered_set<std::string> skipUuids = {}) {
    SKSE::log::debug("QueueBatchCatchUpScan: checking all actors for missing diary books");
    auto state = std::make_shared<DiscoveryState>();
    state->skip = std::move(skipUuids);
    SKSE::GetTaskInterface()->AddTask([state]() { RunDiscoveryBatch(state); });
}

void RunDiscoveryBatch(std::shared_ptr<DiscoveryState> state) {
    try {
        // Fetch next page of up to 50 entries across all actors.
        // endTime=0.0 on the first call means no upper bound.
        // On subsequent calls we pass the oldest timestamp seen so far to page backward.
        double endTime = state->oldestTimestampSeen;
        auto rawEntries = SkyrimNetDiaries::Database::GetDiaryEntries(0, 50, 0.0, endTime);
        bool morePages = (static_cast<int>(rawEntries.size()) >= 50);

        // endTime is treated as inclusive by the API (same as startTime), so
        // filter out any entries at or after the boundary to avoid re-processing.
        if (state->oldestTimestampSeen > 0.0) {
            rawEntries.erase(
                std::remove_if(rawEntries.begin(), rawEntries.end(),
                    [&](const SkyrimNetDiaries::DiaryEntry& e) {
                        return e.entry_date >= state->oldestTimestampSeen;
                    }),
                rawEntries.end());
        }

        if (rawEntries.empty() && morePages) {
            // Entire batch was boundary duplicates — stop to avoid an infinite loop.
            morePages = false;
        }

        // Accumulate actor UUIDs and find the oldest timestamp for the next page.
        for (const auto& e : rawEntries) {
            if (!e.actor_uuid.empty()) {
                state->actorUuidToName.emplace(e.actor_uuid, e.actor_name);
            }
            if (state->oldestTimestampSeen == 0.0 || e.entry_date < state->oldestTimestampSeen) {
                state->oldestTimestampSeen = e.entry_date;
            }
        }

        SKSE::log::debug("DiscoveryBatch: got {} entries ({}), {} distinct actors so far",
                        rawEntries.size(), morePages ? "more pages" : "last page",
                        state->actorUuidToName.size());

        if (morePages) {
            // Chain the next discovery batch as a separate task.
            SKSE::GetTaskInterface()->AddTask([state]() { RunDiscoveryBatch(state); });
            return;
        }

        // Discovery complete — queue one full-fetch task per actor.
        if (state->actorUuidToName.empty()) {
            SKSE::log::debug("QueueBatchCatchUpScan: no actors with diary entries found");
            return;
        }

        SKSE::log::info("QueueBatchCatchUpScan: discovery done, queuing {} per-actor tasks",
                        state->actorUuidToName.size());

        auto bookManager = SkyrimNetDiaries::BookManager::GetSingleton();
        auto taskInterface = SKSE::GetTaskInterface();

        for (const auto& [uuid, name] : state->actorUuidToName) {
            // Skip actors already being handled by immediate recovery.
            if (state->skip.count(uuid)) continue;
            // Skip actors that got volumes from a regular diary event during discovery.
            if (bookManager->GetBookForActor(uuid)) continue;

            taskInterface->AddTask(
                [uuid, name]() {
                    try {
                        // Skip if volumes appeared between queue time and execution.
                        auto* bm = SkyrimNetDiaries::BookManager::GetSingleton();
                        if (bm->GetBookForActor(uuid)) return;

                        RE::FormID formId = SkyrimNetDiaries::Database::GetFormIDForUUID(uuid);
                        if (formId == 0) {
                            SKSE::log::warn("CatchUp: cannot resolve UUID {} to FormID, skipping", uuid);
                            return;
                        }

                        auto entries = SkyrimNetDiaries::Database::GetDiaryEntries(
                            formId, 10000, 0.0, 0.0);
                        if (entries.empty()) return;

                        // Resolve actor name — fall back to RE game name if the diary JSON had no name.
                        std::string actorName = name;
                        if (actorName.empty()) {
                            actorName = SkyrimNetDiaries::Database::GetActorName(uuid);
                        }
                        if (actorName.empty()) {
                            if (auto* actor = RE::TESForm::LookupByID<RE::Actor>(formId)) {
                                actorName = actor->GetName();
                            }
                        }
                        if (actorName.empty()) {
                            SKSE::log::warn("CatchUp: could not resolve actor name for UUID {} (FormID 0x{:X}), skipping", uuid, formId);
                            return;
                        }

                        // formId already in hand — skip the redundant UUID→FormID call inside GetTemplateNameByUUID.
                        std::string bioTemplate = SkyrimNetDiaries::Database::GetBioTemplateName(formId);
                        SKSE::log::info("CatchUp: creating books for {} ({} entries)", actorName, entries.size());
                        CreateAllVolumesForActor(uuid, actorName, formId, bioTemplate, std::move(entries), 1);

                    } catch (const std::exception& e) {
                        SKSE::log::error("CatchUp task exception for {}: {}", name, e.what());
                    } catch (...) {
                        SKSE::log::error("CatchUp task unknown exception for {}", name);
                    }
                });
        }

    } catch (const std::exception& e) {
        SKSE::log::error("RunDiscoveryBatch exception: {}", e.what());
    } catch (...) {
        SKSE::log::error("RunDiscoveryBatch unknown exception");
    }
}

// =============================================================================
// MCM Reset: remove all tracked diary books from NPC inventories and clear all
// BookManager and DiaryDB tracking.  SkyrimNet diary ENTRIES are
// NOT touched - books will be regenerated on the next diary event or Rebuild.
// Returns the number of actor records cleared (negative on exception).
// =============================================================================
int ResetAllDiariesInternal() {
    SKSE::log::info("ResetAllDiariesInternal: starting");

    auto bookManager   = SkyrimNetDiaries::BookManager::GetSingleton();

    int actorsAffected = 0;
    int booksRemoved   = 0;

    try {
        const auto& allBooks = bookManager->GetAllBooks();

        // Build a flat list of (uuid, bookFormId) pairs to remove from inventory.
        // Inventory removal MUST happen on the game thread — queue a single task for it.
        // We use uuid (not a cached FormID) so ESL load-order shifts don't matter.
        struct RemovalEntry { std::string uuid; RE::FormID bookFormId; std::string label; };
        std::vector<RemovalEntry> pendingRemovals;

        // Collect UUIDs while we iterate — needed below for DiaryDB row deletion
        // (must be captured before bookManager->Revert() empties the map).
        std::vector<std::string> uuidsToDelete;
        uuidsToDelete.reserve(allBooks.size());

        for (const auto& [uuid, volumes] : allBooks) {
            if (volumes.empty()) continue;
            ++actorsAffected;
            uuidsToDelete.push_back(uuid);

            for (const auto& vol : volumes) {
                pendingRemovals.push_back({uuid, vol.bookFormId,
                    vol.actorName + " vol " + std::to_string(vol.volumeNumber)});

                ++booksRemoved;
            }
        }

        // Wipe DiaryDB rows BEFORE clearing in-memory state.  Without this,
        // LoadFromDB() on the next save reload reads the persisted rows back,
        // the catch-up scan sees actors already have books, and skips
        // regeneration — making Reset appear to "stop working" after one cycle.
        // DeleteActor removes from both `volumes` and `actor_templates` tables.
        // Also clear stolen-volume tracking so theft state doesn't linger.
        auto* diaryDb = SkyrimNetDiaries::DiaryDB::GetSingleton();
        if (diaryDb && diaryDb->IsOpen()) {
            int dbRowsCleared = 0;
            for (const auto& uuid : uuidsToDelete) {
                if (diaryDb->DeleteActor(uuid)) ++dbRowsCleared;
                diaryDb->ClearAllStolenVolumes(uuid);
            }
            SKSE::log::info("ResetAllDiariesInternal: deleted {} actor(s) from DiaryDB", dbRowsCleared);
        } else {
            SKSE::log::warn("ResetAllDiariesInternal: DiaryDB not open — DB rows NOT deleted "
                            "(reload will restore the diaries)");
        }

        // Clear all in-memory tracking immediately (safe — no game-thread state involved).
        bookManager->Revert();
        g_currentSaveFolder.clear();

        // Dispatch inventory removals to the game thread.  We deliberately do NOT
        // dispose the DPF forms (see the per-entry comment below) to avoid
        // poisoning DPF's FormID recycle pool.
        if (!pendingRemovals.empty()) {
            SKSE::GetTaskInterface()->AddTask([pendingRemovals]() {
                for (const auto& entry : pendingRemovals) {
                    auto* bookForm = RE::TESForm::LookupByID<RE::TESObjectBOOK>(entry.bookFormId);
                    if (!bookForm) {
                        SKSE::log::warn("  Reset: book form 0x{:X} not found for {}", entry.bookFormId, entry.label);
                        continue;
                    }

                    // --- Step 1: Sweep all currently-loaded references ---
                    // ForEachReference covers the active worldspace/interior, so books in
                    // nearby NPC inventories, containers, shelves, etc. are removed
                    // immediately without waiting for a reload.
                    {
                        auto* tesWorld = RE::TES::GetSingleton();
                        if (tesWorld) {
                            RE::TESBoundObject* filterForm = bookForm;
                            auto filter = [filterForm](RE::TESBoundObject& obj) {
                                return &obj == filterForm;
                            };
                            tesWorld->ForEachReference([&](RE::TESObjectREFR* ref) -> RE::BSContainer::ForEachResult {
                                if (!ref || ref->IsDeleted()) return RE::BSContainer::ForEachResult::kContinue;
                                auto inv = ref->GetInventory(filter);
                                auto it  = inv.find(bookForm);
                                if (it != inv.end() && it->second.first > 0) {
                                    ref->RemoveItem(bookForm, it->second.first,
                                        RE::ITEM_REMOVE_REASON::kRemove, nullptr, nullptr);
                                    SKSE::log::debug("  Removed {} from ref 0x{:X} ({})",
                                        entry.label, ref->GetFormID(),
                                        ref->GetBaseObject() ? ref->GetBaseObject()->GetName() : "?");
                                }
                                return RE::BSContainer::ForEachResult::kContinue;
                            });
                        }
                    }

                    // --- We intentionally do NOT Dispose or SetDelete the form ---
                    //
                    // DPF.Dispose() marks the form's FormRecord as `deleted`, which
                    // adds it to DPF's recycle pool.  DPF's AddForm() recycles deleted
                    // records by FormID before allocating fresh ones, and its persisted
                    // pool (co-save + cache file) accumulates DUPLICATE deleted records
                    // for the same FormID across repeated reset/reload cycles.  Those
                    // duplicates get recycled more than once, handing the SAME FormID to
                    // two different actors — the root cause of cross-linked diary content
                    // (e.g. "Frea's Diary" showing Fetri El's text).
                    //
                    // SetDelete(true) is also avoided: the form would be dropped on the
                    // next load, DPF's restore would fail, and DPF would re-mark the
                    // record deleted — re-poisoning the pool the same way.
                    //
                    // Leaving the form fully alive keeps DPF's lastFormId monotonically
                    // increasing, so every future Create() gets a unique FormID.  The
                    // orphaned book is inert: removed from all loaded inventories above,
                    // no longer in our DiaryDB, never re-added.  Trade-off: NPCs in
                    // unloaded cells keep a stale (untracked) copy until regeneration,
                    // and orphaned forms slowly accumulate — both harmless versus the
                    // alternative of corrupted, cross-linked diaries.
                    (void)bookForm;
                }
            });
        }

        SKSE::log::info("ResetAllDiariesInternal: cleared {} volumes for {} actor(s), {} inventory removals queued",
                        booksRemoved, actorsAffected, pendingRemovals.size());
        return actorsAffected;

    } catch (const std::exception& e) {
        SKSE::log::error("ResetAllDiariesInternal exception: {}", e.what());
        return -1;
    }
}

namespace {

    void SaveCallback(SKSE::SerializationInterface* a_intfc) {
        // Ensure the DiaryDB is open before writing the sentinel.
        // On a fresh/new game kPostLoadGame never fires, so the DB may not have been
        // opened yet.  Detect the save folder now and flush all in-memory books so
        // nothing is lost when the save is later reloaded.
        auto* db = SkyrimNetDiaries::DiaryDB::GetSingleton();
        if (!db->IsOpen()) {
            if (g_currentSaveFolder.empty()) {
                DetectSaveFolderFromLog();
            }
            if (!g_currentSaveFolder.empty()) {
                db->Open(g_currentSaveFolder);
            }
        }
        if (db->IsOpen()) {
            SkyrimNetDiaries::BookManager::GetSingleton()->FlushToDB();
            
            // Update last_known_game_time for all actors to capture current game time
            // This is critical for backwards time travel detection when loading older saves
            auto calendar = RE::Calendar::GetSingleton();
            if (calendar) {
                double currentTime = calendar->GetCurrentGameTime() * 86400.0;
                auto actorTemplates = db->LoadActorTemplates();
                int updatedCount = 0;
                for (const auto& [uuid, templateName] : actorTemplates) {
                    db->UpdateLastKnownGameTime(uuid, currentTime);
                    updatedCount++;
                }
                SKSE::log::debug("[SaveCallback] Updated last_known_game_time to {:.2f} for {} actors", currentTime, updatedCount);
            }
        }

        // Save book data
        if (!a_intfc->OpenRecord(kSerializationTypeBooks, kSerializationVersion)) {
            SKSE::log::error("Failed to open book serialization record");
            return;
        }
        SkyrimNetDiaries::BookManager::GetSingleton()->Save(a_intfc);

        // Save current save folder name
        if (!g_currentSaveFolder.empty()) {
            if (!a_intfc->OpenRecord(kSerializationTypeFolder, kSerializationVersion)) {
                SKSE::log::error("Failed to open folder serialization record");
                return;
            }
            
            std::uint32_t folderLen = static_cast<std::uint32_t>(g_currentSaveFolder.length());
            if (!a_intfc->WriteRecordData(&folderLen, sizeof(folderLen))) {
                SKSE::log::error("Failed to write folder name length");
                return;
            }
            if (!a_intfc->WriteRecordData(g_currentSaveFolder.c_str(), folderLen)) {
                SKSE::log::error("Failed to write folder name");
                return;
            }
            
            SKSE::log::debug("Saved current save folder: {}", g_currentSaveFolder);
        }
    }

    void LoadCallback(SKSE::SerializationInterface* a_intfc) {
        // Clear save folder cache - will be restored from serialized data below (or detected on first diary event)
        g_currentSaveFolder.clear();
        
        std::uint32_t type;
        std::uint32_t version;
        std::uint32_t length;

        while (a_intfc->GetNextRecordInfo(type, version, length)) {
            if (version > kSerializationVersion) {
                SKSE::log::error("Serialization version too new for type {}: expected <={}, got {}", type, kSerializationVersion, version);
                continue;
            }

            if (type == kSerializationTypeBooks) {
                SkyrimNetDiaries::BookManager::GetSingleton()->Load(a_intfc, version);
            }
            else if (type == kSerializationTypeFolder) {
                // Load save folder name
                std::uint32_t folderLen;
                if (!a_intfc->ReadRecordData(&folderLen, sizeof(folderLen))) {
                    SKSE::log::error("Failed to read folder name length");
                    continue;
                }
                
                g_currentSaveFolder.resize(folderLen);
                if (!a_intfc->ReadRecordData(g_currentSaveFolder.data(), folderLen)) {
                    SKSE::log::error("Failed to read folder name");
                    g_currentSaveFolder.clear();
                    continue;
                }
                
                SKSE::log::debug("Restored save folder from co-save: {}", g_currentSaveFolder);
            }
        }
    }

    void RevertCallback([[maybe_unused]] SKSE::SerializationInterface* a_intfc) {
        // Called when starting a new game - clear all books and tracking
        SkyrimNetDiaries::BookManager::GetSingleton()->Revert();
        g_currentSaveFolder.clear();
        SKSE::log::info("Reverted all diary data and caches (new game)");
    }

    void OnMessage(SKSE::MessagingInterface::Message* msg)
    {
        if (!msg) {
            return;
        }

        switch (msg->type) {
        case SKSE::MessagingInterface::kDataLoaded: {
            try {
                // Check for required dependency: Dynamic Persistent Forms
                {
                    auto* dataHandler = RE::TESDataHandler::GetSingleton();
                    bool dpfInstalled = dataHandler && dataHandler->LookupModByName("Dynamic Persistent Forms.esp");
                    if (!dpfInstalled) {
                        SKSE::log::error("kDataLoaded: 'Dynamic Persistent Forms.esp' is not installed — diary books cannot be created");
                        SKSE::GetTaskInterface()->AddTask([]() {
                            auto* msgBoxData = RE::UIMessageDataFactory::Create<RE::MessageBoxData>();
                            if (msgBoxData) {
                                msgBoxData->bodyText = "SkyrimNet Physical Diaries requires 'Dynamic Persistent Forms' to be installed.\n\nDiary books cannot be created without it. Please install Dynamic Persistent Forms and restart the game.";
                                msgBoxData->buttonText.push_back("OK");
                                msgBoxData->cancelOptionIndex = 0;
                                RE::MessageBoxMenu::QueueMessage(msgBoxData);
                                SKSE::log::info("kDataLoaded: DPF missing warning queued");
                            }
                        });
                    }
                }
                
                // Verify the diary template books resolve.  If they don't, every
                // diary creation silently fails with "Template book not found" spam.
                // Common causes: ESP not actually enabled, an EditorID-exposure
                // plugin (e.g. po3_Tweaks / Native EditorID Fix) missing or the wrong
                // runtime build, or a tool stripped the template records.
                {
                    auto* t1 = RE::TESForm::LookupByEditorID<RE::TESObjectBOOK>("SkyrimNetDiaryTemplate");
                    auto* t2 = RE::TESForm::LookupByEditorID<RE::TESObjectBOOK>("SkyrimNetDiaryTemplate2");
                    auto* t3 = RE::TESForm::LookupByEditorID<RE::TESObjectBOOK>("SkyrimNetDiaryTemplate3");
                    auto* tN = RE::TESForm::LookupByEditorID<RE::TESObjectBOOK>("SkyrimNetDiaryTemplateN");
                    if (!t1 || !t2 || !t3 || !tN) {
                        SKSE::log::error("================================================================");
                        SKSE::log::error("[Physical Diaries] DIARY TEMPLATE BOOKS NOT FOUND — diaries cannot be created!");
                        SKSE::log::error("  SkyrimNetDiaryTemplate:  {}", t1 ? "OK" : "MISSING");
                        SKSE::log::error("  SkyrimNetDiaryTemplate2: {}", t2 ? "OK" : "MISSING");
                        SKSE::log::error("  SkyrimNetDiaryTemplate3: {}", t3 ? "OK" : "MISSING");
                        SKSE::log::error("  SkyrimNetDiaryTemplateN: {}", tN ? "OK" : "MISSING");
                        SKSE::log::error("  Possible causes:");
                        SKSE::log::error("   1. 'SkyrimNet Physical Diaries.esp' is not enabled in the load order");
                        SKSE::log::error("   2. An EditorID-exposure plugin (po3_Tweaks / Native EditorID Fix) is");
                        SKSE::log::error("      missing or is the wrong runtime build (SE vs AE vs VR)");
                        SKSE::log::error("   3. The ESP was modified by a tool that stripped the template records");
                        SKSE::log::error("================================================================");
                        SKSE::GetTaskInterface()->AddTask([]() {
                            auto* msgBoxData = RE::UIMessageDataFactory::Create<RE::MessageBoxData>();
                            if (msgBoxData) {
                                msgBoxData->bodyText = "SkyrimNet Physical Diaries: diary template books were not found.\n\nDiaries cannot be created. Check that:\n - 'SkyrimNet Physical Diaries.esp' is enabled\n - Native EditorID Fix (or po3_Tweaks) is installed for your game version\n\nSee SkyrimNetPhysicalDiaries.log for details.";
                                msgBoxData->buttonText.push_back("OK");
                                msgBoxData->cancelOptionIndex = 0;
                                RE::MessageBoxMenu::QueueMessage(msgBoxData);
                            }
                        });
                    } else {
                        SKSE::log::info("[Physical Diaries] Diary template books verified (all 4 resolved)");
                    }
                }

                // Now that GMSTs are loaded, read localized month/day names
                SkyrimNetDiaries::Localization::GetSingleton()->ReadGMSTs();
            } catch (const std::exception& e) {
                SKSE::log::error("Exception in kDataLoaded: {}", e.what());
            } catch (...) {
                SKSE::log::error("Unknown exception in kDataLoaded");
            }
            break;
        }
        // ── Inter-plugin API ───────────────────────────────────────────────
        case SkyrimNetPhysicalDiaries_API::SNPD_QUERY_BOOK: {
            if (!msg->data || msg->dataLen < sizeof(SkyrimNetPhysicalDiaries_API::SNPDBookQuery)) {
                SKSE::log::warn("SNPD_QUERY_BOOK: invalid message size {} from '{}'",
                               msg->dataLen, msg->sender ? msg->sender : "unknown");
                break;
            }
            auto* query = static_cast<SkyrimNetPhysicalDiaries_API::SNPDBookQuery*>(msg->data);
            query->isDiaryBook = false;
            query->resultCode  = SkyrimNetPhysicalDiaries_API::SNPDResultCode::NotADiary;
            query->text[0] = '\0';

            auto* bm       = SkyrimNetDiaries::BookManager::GetSingleton();
            auto* bookData = bm->GetBookForFormID(static_cast<RE::FormID>(query->bookFormId));
            if (bookData && !bookData->cachedBookText.empty()) {
                query->isDiaryBook   = true;
                query->entryCount    = bookData->lastKnownEntryCount;
                query->volumeNumber  = bookData->volumeNumber;
                auto* allVols        = bm->GetAllVolumesForActor(bookData->actorUuid);
                query->totalVolumes  = allVols ? static_cast<std::int32_t>(allVols->size()) : 1;

                const auto& t = bookData->cachedBookText;
                // Don't expose the "entries removed" placeholder — it would be read aloud by TTS mods.
                // Leave text empty so callers know there's nothing to read for this volume.
                if (t.find("<!-- SNPD_EMPTY -->") == std::string::npos) {
                    query->resultCode = SkyrimNetPhysicalDiaries_API::SNPDResultCode::Success;
                    size_t copyLen = std::min(t.size(), sizeof(query->text) - 1);
                    std::memcpy(query->text, t.c_str(), copyLen);
                    query->text[copyLen] = '\0';
                    SKSE::log::debug("SNPD_QUERY_BOOK: returned {} chars for FormID 0x{:X} (vol {}/{}, {} entries)",
                                   copyLen, query->bookFormId, query->volumeNumber, query->totalVolumes, query->entryCount);
                } else {
                    query->resultCode = SkyrimNetPhysicalDiaries_API::SNPDResultCode::NoEntries;
                    SKSE::log::debug("SNPD_QUERY_BOOK: FormID 0x{:X} is a diary but has no readable entries (all removed)", query->bookFormId);
                }
            } else if (bookData) {
                SKSE::log::warn("SNPD_QUERY_BOOK: FormID 0x{:X} found but cachedBookText is empty", query->bookFormId);
            }
            break;
        }

        case SkyrimNetPhysicalDiaries_API::SNPD_QUERY_ENTRY: {
            if (!msg->data || msg->dataLen < sizeof(SkyrimNetPhysicalDiaries_API::SNPDEntryQuery)) {
                SKSE::log::warn("SNPD_QUERY_ENTRY: invalid message size {} from '{}'",
                               msg->dataLen, msg->sender ? msg->sender : "unknown");
                break;
            }
            auto* query = static_cast<SkyrimNetPhysicalDiaries_API::SNPDEntryQuery*>(msg->data);
            query->isValid       = false;
            query->resultCode    = SkyrimNetPhysicalDiaries_API::SNPDResultCode::NotADiary;
            query->totalEntries  = 0;
            query->returnedIndex = -1;
            query->content[0]    = '\0';

            auto* bookData = SkyrimNetDiaries::BookManager::GetSingleton()
                                 ->GetBookForFormID(static_cast<RE::FormID>(query->bookFormId));
            if (!bookData || bookData->cachedBookText.empty()) {
                SKSE::log::warn("SNPD_QUERY_ENTRY: FormID 0x{:X} not found or empty", query->bookFormId);
                break;
            }

            // Split cachedBookText on "[pagebreak]\n\n".
            // Page layout: [0]=blank, [1]=title+date-range, [2+]=one page per entry.
            static constexpr std::string_view kPageSep = "[pagebreak]\n\n";
            std::vector<std::string_view> pages;
            std::string_view text = bookData->cachedBookText;
            std::size_t pos = 0;
            while (true) {
                auto found = text.find(kPageSep, pos);
                if (found == std::string_view::npos) { pages.push_back(text.substr(pos)); break; }
                pages.push_back(text.substr(pos, found - pos));
                pos = found + kPageSep.size();
            }

            // Entry pages start at index 2; check for the "removed" placeholder.
            int entryPageCount = static_cast<int>(pages.size()) - 2;
            if (entryPageCount <= 0 ||
                pages[2].find("<!-- SNPD_EMPTY -->") != std::string_view::npos) {
                query->resultCode = SkyrimNetPhysicalDiaries_API::SNPDResultCode::NoEntries;
                SKSE::log::debug("SNPD_QUERY_ENTRY: FormID 0x{:X} has no entries", query->bookFormId);
                break;
            }

            query->totalEntries = entryPageCount;

            std::int32_t idx = query->entryIndex;
            if (idx < 0) idx = entryPageCount - 1;
            if (idx >= entryPageCount) {
                query->resultCode = SkyrimNetPhysicalDiaries_API::SNPDResultCode::IndexOutOfRange;
                SKSE::log::warn("SNPD_QUERY_ENTRY: entryIndex {} out of range (0-{})",
                               query->entryIndex, entryPageCount - 1);
                break;
            }

            std::string_view page = pages[static_cast<std::size_t>(idx + 2)];
            while (!page.empty() && (page.back() == '\n' || page.back() == '\r'))
                page.remove_suffix(1);
            query->isValid       = true;
            query->resultCode    = SkyrimNetPhysicalDiaries_API::SNPDResultCode::Success;
            query->returnedIndex = idx;

            std::size_t copyLen = std::min(page.size(), sizeof(query->content) - 1);
            std::memcpy(query->content, page.data(), copyLen);
            query->content[copyLen] = '\0';

            SKSE::log::debug("SNPD_QUERY_ENTRY: returned entry {}/{} for FormID 0x{:X} ({} chars)",
                           idx, entryPageCount - 1, query->bookFormId, copyLen);
            break;
        }

        case SkyrimNetPhysicalDiaries_API::SNPD_QUERY_ALL_ENTRIES: {
            if (!msg->data || msg->dataLen < sizeof(SkyrimNetPhysicalDiaries_API::SNPDAllEntriesQuery)) {
                SKSE::log::warn("SNPD_QUERY_ALL_ENTRIES: invalid message size {} from '{}'",
                               msg->dataLen, msg->sender ? msg->sender : "unknown");
                break;
            }
            auto* query = static_cast<SkyrimNetPhysicalDiaries_API::SNPDAllEntriesQuery*>(msg->data);
            query->isValid        = false;
            query->resultCode     = SkyrimNetPhysicalDiaries_API::SNPDResultCode::NotADiary;
            query->entryCount     = 0;
            query->truncatedCount = 0;
            query->content[0]     = '\0';

            auto* bookData = SkyrimNetDiaries::BookManager::GetSingleton()
                                 ->GetBookForFormID(static_cast<RE::FormID>(query->bookFormId));
            if (!bookData || bookData->cachedBookText.empty()) {
                SKSE::log::warn("SNPD_QUERY_ALL_ENTRIES: FormID 0x{:X} not found or empty", query->bookFormId);
                break;
            }

            // Split cachedBookText on "[pagebreak]\n\n".
            // Page layout: [0]=blank, [1]=title+date-range, [2+]=one page per entry.
            static constexpr std::string_view kPageSep = "[pagebreak]\n\n";
            std::vector<std::string_view> pages;
            std::string_view text = bookData->cachedBookText;
            std::size_t pos = 0;
            while (true) {
                auto found = text.find(kPageSep, pos);
                if (found == std::string_view::npos) { pages.push_back(text.substr(pos)); break; }
                pages.push_back(text.substr(pos, found - pos));
                pos = found + kPageSep.size();
            }

            query->isValid    = true;
            query->resultCode = SkyrimNetPhysicalDiaries_API::SNPDResultCode::Success;

            int entryPageCount = static_cast<int>(pages.size()) - 2;
            if (entryPageCount <= 0 ||
                pages[2].find("<!-- SNPD_EMPTY -->") != std::string_view::npos) {
                SKSE::log::debug("SNPD_QUERY_ALL_ENTRIES: FormID 0x{:X} has no entries", query->bookFormId);
                break;
            }

            // Pack null-terminated page strings back-to-back into the buffer.
            constexpr std::size_t kBufSize = sizeof(query->content) - 1;
            std::size_t writePos = 0;
            int packed = 0, dropped = 0;
            for (int i = 0; i < entryPageCount; ++i) {
                std::string_view page = pages[static_cast<std::size_t>(i + 2)];
                while (!page.empty() && (page.back() == '\n' || page.back() == '\r'))
                    page.remove_suffix(1);
                if (writePos + page.size() + 1 > kBufSize) { ++dropped; continue; }
                std::memcpy(query->content + writePos, page.data(), page.size());
                writePos += page.size();
                query->content[writePos++] = '\0';
                ++packed;
            }
            query->content[writePos] = '\0';
            query->entryCount     = packed;
            query->truncatedCount = dropped;

            SKSE::log::debug("SNPD_QUERY_ALL_ENTRIES: packed {}/{} entries for FormID 0x{:X} ({} bytes, {} truncated)",
                           packed, entryPageCount, query->bookFormId, writePos, dropped);
            break;
        }

        case SKSE::MessagingInterface::kSaveGame: {
            // Mark every tracked volume as having been written into a .ess save file.
            // On the next kPostLoadGame, QueueInventoryCheck will skip these volumes
            // so legitimately taken/stolen books are not re-added to NPC inventories.
            SkyrimNetDiaries::DiaryDB::GetSingleton()->MarkAllVolumesPersisted();
            // Sync the in-memory flag too so the current session stays consistent.
            for (auto& [uuid, volumes] : SkyrimNetDiaries::BookManager::GetSingleton()->GetAllBooksRef()) {
                for (auto& vol : volumes) vol.persistedInSave = true;
            }
            SKSE::log::debug("kPostSaveGame: all volumes marked as persisted");
            break;
        }

        case SKSE::MessagingInterface::kPostLoadGame: {
            // First, and independent of SkyrimNet: DPF has restored its forms by now.
            SkyrimNetDiaries::BookManager::SanitizeLoadedBookForms();

            if (!SkyrimNetDiaries::Database::InitializeAPI()) {
                SKSE::log::warn("Failed to initialize API (SkyrimNet may not be loaded yet)");
                break;
            }
            SKSE::log::info("✓ SkyrimNet API ready");

            // Detect save folder (from co-save SNDF record or SkyrimNet.log fallback).
            // Always re-detect on each load in case the player loaded a different save.
            g_currentSaveFolder.clear();
            DetectSaveFolderFromLog();

            // Open the per-save SQLite diary DB.
            if (!g_currentSaveFolder.empty()) {
                SkyrimNetDiaries::DiaryDB::GetSingleton()->Open(g_currentSaveFolder);
            } else {
                SKSE::log::warn("kPostLoadGame: save folder still unknown — DiaryDB not opened");
            }

            // Clear the actor reference cache — pointers from the previous load session
            // may be stale (or nullptr from failed lookups).  A fresh search runs on this load.
            SkyrimNetDiaries::BookManager::ClearActorCache();

            // Queue the post-load setup task with a retry loop: SkyrimNet's Papyrus-based
            // memory system re-initialises asynchronously on reload and may not be ready
            // for several frames.  We poll IsMemorySystemReady() and re-queue ourselves
            // on the next game frame until it is (capped at 300 attempts ≈ ~5 seconds).
            auto runSetup = std::make_shared<std::function<void()>>();
            *runSetup = [runSetup, attempt = 0]() mutable {
                if (!SkyrimNetDiaries::Database::IsMemorySystemReady()) {
                    if (++attempt >= 300) {
                        SKSE::log::error("kPostLoadGame: SkyrimNet memory system never became ready after 300 retries — giving up");
                        return;
                    }
                    if (attempt % 30 == 1) {
                        SKSE::log::debug("kPostLoadGame: waiting for SkyrimNet memory system (attempt {})...", attempt);
                    }
                    SKSE::GetTaskInterface()->AddTask([runSetup]() { (*runSetup)(); });
                    return;
                }

                if (attempt > 0) {
                    SKSE::log::info("kPostLoadGame: SkyrimNet memory system ready after {} retries", attempt);
                }

                // Re-register the snpd_diary_stolen decorator with SkyrimNet's prompt engine.
                // SkyrimNet resets all Papyrus decorator registrations on every game load.
                // Papyrus OnInit (which originally called RegisterDecorator) only fires on
                // new game creation, so existing saves would lose the decorator after any
                // reload.  We call SkyrimNetApi::RegisterDecorator via the Papyrus VM
                // directly from C++ here, which works for every load of every save.
                {
                    auto* vm = RE::BSScript::Internal::VirtualMachine::GetSingleton();
                    if (vm) {
                        auto* args = RE::MakeFunctionArguments(
                            RE::BSFixedString("snpd_diary_stolen"),
                            RE::BSFixedString("SkyrimNetDiaries_Decorators"),
                            RE::BSFixedString("IsDiaryStolen"));
                        RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor> nullCb;
                        bool ok = vm->DispatchStaticCall(
                            "SkyrimNetApi", "RegisterDecorator", args, nullCb);
                        delete args;
                        SKSE::log::info("kPostLoadGame: registered snpd_diary_stolen decorator via Papyrus VM ({})",
                                        ok ? "dispatched" : "FAILED");
                    } else {
                        SKSE::log::warn("kPostLoadGame: Papyrus VM not available — snpd_diary_stolen not registered");
                    }
                }

                auto invalidActors = SkyrimNetDiaries::BookManager::GetSingleton()->LoadFromDB();

                // For volumes whose DPF form still exists in process memory but whose
                // inventory entry was wiped by a reload-without-save, re-add the book.
                SkyrimNetDiaries::BookManager::GetSingleton()->QueueInventoryCheck();

                // === Theft Tracking Reconciliation ===
                // Detect backwards time travel and clear stolen volumes if detected
                auto calendar = RE::Calendar::GetSingleton();
                if (calendar) {
                    double currentTime = calendar->GetCurrentGameTime() * 86400.0;
                    auto* diaryDB = SkyrimNetDiaries::DiaryDB::GetSingleton();
                    auto actorTemplates = diaryDB->LoadActorTemplates();
                    
                    SKSE::log::debug("[Theft Reconciliation] Checking {} actors for backwards time travel (current time: {:.2f} seconds)",
                                   actorTemplates.size(), currentTime);
                    
                    int clearedCount = 0;
                    for (const auto& [uuid, templateName] : actorTemplates) {
                        double lastKnownTime = diaryDB->GetLastKnownGameTime(uuid);
                        std::string actorName = SkyrimNetDiaries::Database::GetActorName(uuid);
                        if (actorName.empty()) {
                            actorName = templateName;  // Fallback to template name if lookup fails
                        }
                        
                        SKSE::log::debug("[Theft Reconciliation] {} ({}): last_known={:.2f}, current={:.2f}, delta={:.2f}",
                                       actorName, uuid.substr(0, 8), lastKnownTime, currentTime, currentTime - lastKnownTime);
                        
                        // Detect backwards time travel - clear all stolen volumes if save is from earlier in time
                        if (currentTime < lastKnownTime) {
                            SKSE::log::warn("[Physical Diaries] ⚠ Backwards time travel detected for {} - loaded save from {:.2f} but last session was at {:.2f} (went back {:.2f} seconds)",
                                           actorName, currentTime, lastKnownTime, lastKnownTime - currentTime);
                            diaryDB->ClearAllStolenVolumes(uuid);
                            SKSE::log::debug("[Physical Diaries] ✓ Cleared all stolen volumes for {} due to time travel", actorName);
                            clearedCount++;
                        }
                        
                        // Update last known game time for all actors
                        diaryDB->UpdateLastKnownGameTime(uuid, currentTime);
                    }
                    
                    if (clearedCount > 0) {
                        SKSE::log::info("[Theft Reconciliation] Cleared stolen volumes for {} actors due to backwards time travel", clearedCount);
                    } else {
                        SKSE::log::debug("[Theft Reconciliation] No backwards time travel detected - all actors up to date");
                    }
                }

                // Build skip set: deduplicated UUIDs being immediately recovered.
                std::unordered_set<std::string> skipUuids;
                if (!invalidActors.empty()) {
                    SKSE::log::info("kPostLoadGame: {} actor(s) had invalid FormIDs — queuing immediate recreation", invalidActors.size());
                    std::unordered_set<std::string> seen;
                    for (const auto& uuid : invalidActors) {
                        if (!seen.insert(uuid).second) continue;
                        uint32_t formId = SkyrimNetDiaries::Database::GetFormIDForUUID(uuid);
                        if (formId == 0) {
                            SKSE::log::warn("kPostLoadGame: could not resolve FormID for UUID {} — catch-up scan will handle it", uuid);
                            continue;
                        }
                        skipUuids.insert(uuid);
                        RE::FormID fid = static_cast<RE::FormID>(formId);
                        SKSE::GetTaskInterface()->AddTask([fid]() {
                            UpdateDiaryForActorInternal(fid);
                        });
                    }
                }

                // Pass skip set so these two paths don't re-queue the same actors.
                QueueSealedVolumeRecovery(skipUuids);
                QueueBatchCatchUpScan(std::move(skipUuids));

            }; // end of runSetup lambda body

            SKSE::GetTaskInterface()->AddTask([runSetup]() { (*runSetup)(); });
            break;
        }
        }
    }

    void InitializeLog()
    {
        auto path = SKSE::log::log_directory();
        if (!path) {
            return;
        }

        *path /= "SkyrimNetPhysicalDiaries.log"sv;
        auto sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(path->string(), true);

        auto log = std::make_shared<spdlog::logger>("global log"s, std::move(sink));
        log->set_level(spdlog::level::info);
        log->flush_on(spdlog::level::info);

        spdlog::set_default_logger(std::move(log));
        spdlog::set_pattern("[%H:%M:%S] [%l] %v"s);

        const auto* plugin = SKSE::PluginDeclaration::GetSingleton();
        SKSE::log::info("{} v{}", plugin->GetName(), plugin->GetVersion());
    }
}

// SKSEPlugin_Version and SKSEPlugin_Query are auto-generated by add_commonlibsse_plugin
// in CMakeLists.txt via cmake/CommonLibSSE.cmake — do NOT declare them manually here.

extern "C" DLLEXPORT bool SKSEAPI SKSEPlugin_Load(const SKSE::LoadInterface* a_skse)
{
    InitializeLog();
    SKSE::log::info("Loading SkyrimNetPhysicalDiaries...");

    SKSE::Init(a_skse);
    
    // Load configuration, then immediately save it back so MO2 writes the file into
    // the Overwrite folder.  This ensures user settings survive future mod updates
    // that would otherwise replace the shipped INI inside the mod folder.
    auto configPath = std::filesystem::current_path() / "Data" / "SKSE" / "Plugins" / "SkyrimNetPhysicalDiaries.ini";
    SkyrimNetDiaries::Config::GetSingleton()->Load(configPath);
    SkyrimNetDiaries::Config::GetSingleton()->Save();  // Persist to MO2 Overwrite on first run

    // Apply log level from config (must come after Load so INI value is available)
    if (SkyrimNetDiaries::Config::GetSingleton()->GetDebugLog()) {
        spdlog::default_logger()->set_level(spdlog::level::debug);
        SKSE::log::info("Debug logging enabled via config");
    }
    
    SKSE::log::debug("Registering for SKSE messaging interface...");
    auto messaging = SKSE::GetMessagingInterface();
    if (messaging) {
        messaging->RegisterListener(OnMessage);
    }

    SKSE::log::debug("Registering for SKSE serialization...");
    auto serialization = SKSE::GetSerializationInterface();
    serialization->SetUniqueID(kSerializationTypeBooks);
    serialization->SetSaveCallback(SaveCallback);
    serialization->SetLoadCallback(LoadCallback);
    serialization->SetRevertCallback(RevertCallback);

    // Initialize BookManager with template book Editor IDs from ESP
    // 4 templates total: base, 2 variants, and Nightingale special
    SkyrimNetDiaries::BookManager::GetSingleton()->Initialize(
        "SkyrimNetDiaryTemplate",      // Base template
        "SkyrimNetDiaryTemplate2",     // Variant 2
        "SkyrimNetDiaryTemplate3",     // Variant 3
        "",                              // Unused
        "",                              // Unused
        "SkyrimNetDiaryTemplateN"      // Nightingale journal
    );

    // Register C++ event handler for diary theft/return detection
    SKSE::log::debug("Registering diary theft/return event handler...");
    DiaryTheftHandler::Register();

    // Detect game language and initialize localization (must happen before BookTextHook).
    SkyrimNetDiaries::Localization::GetSingleton()->Initialize();

    // Install book text injection hook (replaces Dynamic Book Framework text delivery,
    // covers SE, AE and VR — see BookTextHook.cpp).
    BookTextHook::Install();

    // Register Papyrus native functions
    SKSE::log::debug("Registering Papyrus native functions...");
    PapyrusAPI::Register();

    SKSE::log::info("SkyrimNetPhysicalDiaries loaded successfully!");
    
    return true;
}

