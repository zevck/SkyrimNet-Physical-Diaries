#include "Database.h"
#include "BookManager.h"
#include "BookTextHook.h"
#include "DiaryTheftHandler.h"
#include "SkyrimNetPhysicalDiariesAPI.h"
#include "Config.h"
#include "DiaryDB.h"
#include "PapyrusAPI.h"
#include "RE/F/FunctionArguments.h"
#include "RE/V/VirtualMachine.h"
#include <spdlog/sinks/basic_file_sink.h>
#include <fstream>
#include <sstream>
#include <unordered_set>
#include <unordered_map>



// Helper function to sanitize text for Skyrim's book renderer
std::string SanitizeBookText(const std::string& text) {
    std::string result = text;
        
        // Strip leading date headers/prefixes that LLM sometimes includes.
        // e.g. "# Sundas, 17th Last Seed - Late Morning"   (whole-line header)
        //      "Sundas, 17th of Last Seed, 4E 201"         (whole-line, no year prefix needed)
        //      "Sundas, 17th Last Seed. Today I saw..."    (inline date prefix in prose)
        // We supply our own date headers, so always remove any at the start of the content.
        {
            static const std::vector<std::string> skyrimDateWords = {
                // Days of the week
                "Sundas", "Morndas", "Tirdas", "Middas", "Turdas", "Fredas", "Loredas",
                // Months
                "Morning Star", "Sun's Dawn", "First Seed", "Rain's Hand",
                "Second Seed", "Midyear", "Sun's Height", "Last Seed",
                "Hearthfire", "Frostfall", "Sun's Dusk", "Evening Star",
                // Explicit era marker
                "4E "
            };

            // Strip optional leading markdown heading chars ("# ", "## ", etc.)
            size_t contentStart = 0;
            while (contentStart < result.size() && result[contentStart] == '#') ++contentStart;
            while (contentStart < result.size() && result[contentStart] == ' ') ++contentStart;

            // Check whether the content (after any heading prefix) starts with a date word
            std::string_view view(result.c_str() + contentStart, result.size() - contentStart);
            const std::string* matchedWord = nullptr;
            for (const auto& word : skyrimDateWords) {
                if (view.substr(0, word.size()) == word) {
                    matchedWord = &word;
                    break;
                }
            }

            if (matchedWord) {
                size_t firstNewline = result.find('\n', contentStart);
                bool isWholeLine = (firstNewline != std::string::npos && firstNewline < contentStart + 120);

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
                    size_t searchFrom = contentStart + matchedWord->size();
                    size_t breakPos = std::string::npos;
                    for (size_t i = searchFrom; i + 1 < result.size(); ++i) {
                        char c = result[i];
                        char next = result[i + 1];
                        if ((c == '.' || c == '!' || c == '?') && (next == ' ' || next == '\n')) {
                            breakPos = i + 2; // skip the punctuation and the space/newline
                            break;
                        }
                    }
                    if (breakPos != std::string::npos && breakPos < contentStart + 200) {
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
        
        // Replace em dashes (U+2014) with double hyphens
        size_t pos = 0;
        while ((pos = result.find("\xE2\x80\x94", pos)) != std::string::npos) {
        result.replace(pos, 3, "--");
        pos += 2;
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
    
    // Skyrim months (30 days each)
    const char* monthNames[] = {
        "Morning Star", "Sun's Dawn", "First Seed", "Rain's Hand",
        "Second Seed", "Midyear", "Sun's Height", "Last Seed",
        "Hearthfire", "Frostfall", "Sun's Dusk", "Evening Star"
    };
    
    // Skyrim day names (7-day week)
    const char* dayNames[] = {
        "Sundas", "Morndas", "Tirdas", "Middas", "Turdas", "Fredas", "Loredas"
    };
    
    // Calculate absolute day number from game start (17 Last Seed)
    int absoluteDay = startDay + totalDays;
    int currentMonth = startMonth;
    int currentYear = startYear;
    
    // Handle month/year overflow
    while (absoluteDay > 30) {
        absoluteDay -= 30;
        currentMonth++;
        if (currentMonth >= 12) {
            currentMonth = 0;
            currentYear++;
        }
    }
    
    // Calculate day of week from start day
    int dayOfWeek = (startDayOfWeek + totalDays) % 7;
    
    // Format: "Sundas, 17 Last Seed, 4E 201" or "17 Last Seed, 4E 201" (without day of week)
    char buffer[128];
    snprintf(buffer, sizeof(buffer), "%s, %d %s, 4E %d",
            dayNames[dayOfWeek], absoluteDay, monthNames[currentMonth], currentYear);
    
    return std::string(buffer);
}

std::string FormatGameDateShort(double gameTime) {
    // Same as FormatGameDate but without day of week - for title page
    int totalDays = static_cast<int>(gameTime / 86400.0);
    
    const int startDay = 17;
    const int startMonth = 7; // Last Seed (0-indexed)
    const int startYear = 201;
    
    // Skyrim months (30 days each)
    const char* monthNames[] = {
        "Morning Star", "Sun's Dawn", "First Seed", "Rain's Hand",
        "Second Seed", "Midyear", "Sun's Height", "Last Seed",
        "Hearthfire", "Frostfall", "Sun's Dusk", "Evening Star"
    };
    
    // Calculate absolute day number from game start (17 Last Seed)
    int absoluteDay = startDay + totalDays;
    int currentMonth = startMonth;
    int currentYear = startYear;
    
    // Handle month/year overflow
    while (absoluteDay > 30) {
        absoluteDay -= 30;
        currentMonth++;
        if (currentMonth >= 12) {
            currentMonth = 0;
            currentYear++;
        }
    }
    
    // Format without day of week: "17 Last Seed, 4E 201"
    char buffer[128];
    snprintf(buffer, sizeof(buffer), "%d %s, 4E %d",
            absoluteDay, monthNames[currentMonth], currentYear);
    
    return std::string(buffer);
}

namespace
{
    // Cache actor reference ID → UUID (persistent across sessions)
    std::unordered_map<RE::FormID, std::string> g_actorUuidCache;
    
    // Cache the current save's folder name (e.g., "SkyrimNet-1772379483115-796523")
    std::string g_currentSaveFolder;

    constexpr std::uint32_t kSerializationVersion = 3;
    constexpr std::uint32_t kSerializationTypeBooks = 'SNDB'; // SkyrimNet Diary Books
    constexpr std::uint32_t kSerializationTypeCache = 'SNDC'; // SkyrimNet Diary Cache (UUID mappings)
    constexpr std::uint32_t kSerializationTypeFolder = 'SNDF'; // SkyrimNet Diary Folder (save-specific database name)

} // end anonymous namespace

// Forward declaration — defined below after GetCurrentSaveFolder.
std::string DetectSaveFolderFromLog();

// Get the current save's folder name (e.g., "SkyrimNet-1772379483115-796523").
// Returns the cached value when available; otherwise delegates to DetectSaveFolderFromLog
// which reads the authoritative save ID from SkyrimNet.log.
std::string GetCurrentSaveFolder() {
    if (!g_currentSaveFolder.empty()) {
        return g_currentSaveFolder;
    }
    return DetectSaveFolderFromLog();
}

// Parse SkyrimNet.log to detect the current save folder (more reliable than filesystem scan).
// Finds the most recent "Using save ID: " entry, verifies the .db file exists, and caches
// the result in g_currentSaveFolder. Returns "" on failure (caller falls back to GetCurrentSaveFolder).
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

// Generate bio_template_name from actor (for UUID resolution)
std::string GetBioTemplateName(RE::Actor* actor) {
    if (!actor || !actor->GetActorBase()) {
        return "";
    }
    
    // Get actor name and normalize it (lowercase, replace spaces with underscores)
    std::string name = actor->GetActorBase()->GetName();
    std::transform(name.begin(), name.end(), name.begin(), ::tolower);
    std::replace(name.begin(), name.end(), ' ', '_');
    std::replace(name.begin(), name.end(), '-', '_');
    std::replace(name.begin(), name.end(), '\'', '_');
    
    // Get last 3 hex digits of reference ID (stable across ESL load order changes)
    RE::FormID refID = actor->GetFormID();
    std::string last3 = fmt::format("{:03X}", refID & 0xFFF);
    
    return fmt::format("{}_{}", name, last3);
}

// Resolve actor to UUID with caching
std::string ResolveActorUUID(RE::Actor* actor) {
    if (!actor) {
        return "";
    }
    
    RE::FormID actorRefID = actor->GetFormID();
    
    // Check cache first
    auto it = g_actorUuidCache.find(actorRefID);
    if (it != g_actorUuidCache.end()) {
        return it->second;
    }
    
    // Not cached - resolve via SkyrimNet API
    std::string uuid = SkyrimNetDiaries::Database::GetUUIDFromFormID(actorRefID);
    if (!uuid.empty()) {
        // Cache for future lookups
        g_actorUuidCache[actorRefID] = uuid;
        SKSE::log::debug("Resolved actor {} (FormID 0x{:X}) to UUID: {}", actor->GetActorBase()->GetName(), actorRefID, uuid);
    }
    
    return uuid;
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
    
    // Blank first page
    bookText = "[pagebreak]\n\n";

    // Title page — handwriting font, centred; leading newlines push it down visually
    bookText += "\n\n\n\n\n";
    bookText += "<font face='$HandwrittenFont' size='" + std::to_string(fontTitle) + "'><p align='center'>";
    bookText += actorName + "'s Diary";
    bookText += "</p></font>\n\n";

    if (entries.empty()) {
        bookText += "[pagebreak]\n\n";
        bookText += "<font face='$HandwrittenFont' size='" + std::to_string(fontContent) + "'><p align='center'>All entries from this time period have been removed.</p></font>";
    } else {
        // Date range below title
        std::string firstDate = FormatGameDateShort(entries.front().entry_date);
        std::string lastDate = FormatGameDateShort(entries.back().entry_date);

        bookText += "<font face='$HandwrittenFont' size='" + std::to_string(fontSmall) + "'><p align='center'>";
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
            
            // Reset font size explicitly before date header (title page font might bleed through pagebreak)
            bookText += "<font face='$HandwrittenFont' size='" + std::to_string(fontDate) + "'></font>";
            // Date header
            bookText += "<font face='$HandwrittenFont' size='" + std::to_string(fontDate) + "'>" + dateStr + "</font>";
            bookText += "<font face='$HandwrittenFont' size='" + std::to_string(fontContent) + "'></font>\n\n";  // Reset to content font size
            
            // Entry content - wrap EACH paragraph in font tag since Skyrim resets after \n\n
            std::string content = SanitizeBookText(entry.content);
            
            // Strip LLM-generated date headers at the start
            // Examples: "9:28 AM, Sundas, 17th of Last Seed" OR "Sundas, 17th of Last Seed"
            // Only strip short lines that look like date headers, not narrative paragraphs
            size_t firstNewline = content.find('\n');
            if (firstNewline != std::string::npos && firstNewline < 80) {
                std::string firstLine = content.substr(0, firstNewline);
                
                // Check if line looks like a date header:
                // - Starts with time pattern (9:28 AM) OR
                // - Contains ordinal number with "of" (17th of Last Seed) OR
                // - Is short and starts with a day name
                bool isDateHeader = false;
                
                // Pattern 1: Starts with time
                std::regex timePattern("^\\s*\\d{1,2}:\\d{2}\\s*(?:AM|PM)");
                if (std::regex_search(firstLine, timePattern)) {
                    isDateHeader = true;
                }
                
                // Pattern 2: Contains ordinal + "of" (17th of, 1st of, etc.)
                std::regex ordinalPattern("\\d{1,2}(?:st|nd|rd|th)\\s+of\\s+");
                if (std::regex_search(firstLine, ordinalPattern)) {
                    isDateHeader = true;
                }
                
                // Pattern 3: Short line starting with day name (Sundas, Morndas, etc.)
                if (firstLine.length() < 60) {
                    std::regex dayStart("^\\s*(?:Sundas|Morndas|Tirdas|Middas|Turdas|Fredas|Loredas)[,\\s]");
                    if (std::regex_search(firstLine, dayStart)) {
                        isDateHeader = true;
                    }
                }
                
                if (isDateHeader) {
                    // Strip the date header line
                    content = content.substr(firstNewline + 1);
                    // Strip leading whitespace/newlines after removal
                    while (!content.empty() && (content[0] == '\n' || content[0] == '\r' || content[0] == ' ')) {
                        content = content.substr(1);
                    }
                }
            }
            
            // Split by double newlines (paragraph breaks) and wrap each
            size_t pos = 0;
            size_t found;
            while ((found = content.find("\n\n", pos)) != std::string::npos) {
                std::string paragraph = content.substr(pos, found - pos);
                if (!paragraph.empty()) {
                    bookText += "<font face='$HandwrittenFont' size='" + std::to_string(fontContent) + "'>" + paragraph + "</font>\n\n";
                }
                pos = found + 2;
            }
            // Last paragraph
            std::string lastParagraph = content.substr(pos);
            if (!lastParagraph.empty()) {
                bookText += "<font face='$HandwrittenFont' size='" + std::to_string(fontContent) + "'>" + lastParagraph + "</font>\n\n";
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


namespace {

    // BookMenu event sink for tracking when books open
    class BookMenuEventSink : public RE::BSTEventSink<RE::MenuOpenCloseEvent> {
    public:
        static BookMenuEventSink* GetSingleton() {
            static BookMenuEventSink singleton;
            return &singleton;
        }

        std::chrono::steady_clock::time_point bookOpenTime;  // reserved for future use
        RE::BSEventNotifyControl ProcessEvent(const RE::MenuOpenCloseEvent* a_event, 
                                               RE::BSTEventSource<RE::MenuOpenCloseEvent>*) override {
            // Refresh + text injection is handled by BookTextHook before the book renders.
            // Nothing to do here.
            (void)a_event;
            return RE::BSEventNotifyControl::kContinue;
        }

    private:
        BookMenuEventSink() = default;
        BookMenuEventSink(const BookMenuEventSink&) = delete;
        BookMenuEventSink& operator=(const BookMenuEventSink&) = delete;
    };

} // end anonymous namespace (event sinks)

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
            std::string bookName = actorName + "'s Diary";
            if (latestVolume->volumeNumber > 1) {
                bookName += ", v" + std::to_string(latestVolume->volumeNumber);
            }
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
            std::string bookName = actorName + "'s Diary";
            if (latestVolume->volumeNumber > 1) {
                bookName += ", v" + std::to_string(latestVolume->volumeNumber);
            }
            
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
// MCM Reset: remove all tracked diary books from NPC inventories, delete their
// .txt files, and clear all BookManager tracking.  SkyrimNet diary ENTRIES are
// NOT touched - books will be regenerated on the next diary event or Rebuild.
// Returns the number of actor records cleared (negative on exception).
// =============================================================================
int ResetAllDiariesInternal() {
    SKSE::log::info("ResetAllDiariesInternal: starting");

    auto bookManager   = SkyrimNetDiaries::BookManager::GetSingleton();
    auto booksBasePath = std::filesystem::current_path() / "Data" / "SKSE" / "Plugins" / "SkyrimNetPhysicalDiaries" / "Books";
    std::string saveFolder = g_currentSaveFolder; // snapshot before clearing

    int actorsAffected = 0;
    int booksRemoved   = 0;

    try {
        const auto& allBooks = bookManager->GetAllBooks();

        // Build a flat list of (uuid, bookFormId) pairs to remove from inventory,
        // plus delete .txt files now (filesystem ops are safe off the game thread).
        // Inventory removal MUST happen on the game thread — queue a single task for it.
        // We use uuid (not a cached FormID) so ESL load-order shifts don't matter.
        struct RemovalEntry { std::string uuid; RE::FormID bookFormId; std::string label; };
        std::vector<RemovalEntry> pendingRemovals;

        for (const auto& [uuid, volumes] : allBooks) {
            if (volumes.empty()) continue;
            ++actorsAffected;

            for (const auto& vol : volumes) {
                pendingRemovals.push_back({uuid, vol.bookFormId,
                    vol.actorName + " vol " + std::to_string(vol.volumeNumber)});

                // Delete .txt file (safe to do here on the Papyrus thread).
                if (!saveFolder.empty() && !vol.bioTemplateName.empty()) {
                    std::string bookName = vol.actorName + "'s Diary";
                    if (vol.volumeNumber > 1) {
                        bookName += ", v" + std::to_string(vol.volumeNumber);
                    }
                    auto txtPath = booksBasePath / saveFolder / vol.bioTemplateName / (bookName + ".txt");
                    if (std::filesystem::exists(txtPath)) {
                        std::filesystem::remove(txtPath);
                        SKSE::log::debug("  Deleted {}", txtPath.string());
                    }
                }
                ++booksRemoved;
            }
        }

        // Clear all in-memory tracking immediately (safe — no game-thread state involved).
        bookManager->Revert();
        g_actorUuidCache.clear();
        g_currentSaveFolder.clear();

        // Dispatch inventory removals and DPF disposal to the game thread.
        if (!pendingRemovals.empty()) {
            SKSE::GetTaskInterface()->AddTask([pendingRemovals]() {
                auto  vm     = RE::BSScript::Internal::VirtualMachine::GetSingleton();

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

                    // --- Step 2: Tell DPF to stop persisting this form ---
                    // Removes the entry from DPF's save data so it won't be recreated
                    // on the next game load.
                    if (vm) {
                        RE::TESForm* formPtr = bookForm;
                        auto disposeArgs = RE::MakeFunctionArguments(std::move(formPtr));
                        RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor> noopCallback;
                        vm->DispatchStaticCall("DynamicPersistentForms", "Dispose", disposeArgs, noopCallback);
                        SKSE::log::debug("  Disposed DPF form 0x{:X} for {}", entry.bookFormId, entry.label);
                    }

                    // --- Step 3: Mark the base form as deleted globally ---
                    // SetDelete(true) sets the kDeleted flag on the TESObjectBOOK base form.
                    // Any reference to this form ID in an unloaded cell (a chest in a far-
                    // away dungeon, a shelf in an unloaded house, etc.) will be treated as
                    // a missing/orphaned form reference when those cells eventually load,
                    // and Skyrim silently drops such references. This is the only reliable
                    // way to "reach" containers that are not currently in memory.
                    bookForm->SetDelete(true);
                    SKSE::log::debug("  Marked base form 0x{:X} as deleted for {}", entry.bookFormId, entry.label);
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

// =============================================================================
// MCM Rebuild: reset then immediately recreate all books from existing entries.
// Requires that the SkyrimNet database can be found (save folder detectable).
// =============================================================================

namespace {

    // Container change event sink to track diary theft/removal
    class ContainerChangedEventSink : public RE::BSTEventSink<RE::TESContainerChangedEvent> {
    public:
        static ContainerChangedEventSink* GetSingleton() {
            static ContainerChangedEventSink singleton;
            return &singleton;
        }

        RE::BSEventNotifyControl ProcessEvent(const RE::TESContainerChangedEvent* a_event,
                                               RE::BSTEventSource<RE::TESContainerChangedEvent>*) override {
            if (!a_event) {
                return RE::BSEventNotifyControl::kContinue;
            }

            // Check if a diary was removed from an NPC
            auto bookManager = SkyrimNetDiaries::BookManager::GetSingleton();
            auto diaryData = bookManager->GetBookForFormID(a_event->baseObj);
            
            if (!diaryData) {
                return RE::BSEventNotifyControl::kContinue;
            }

            // Get the source (who lost the item) and destination (who received it)
            auto fromRef = a_event->oldContainer ? RE::TESForm::LookupByID<RE::TESObjectREFR>(a_event->oldContainer) : nullptr;
            auto toRef = a_event->newContainer ? RE::TESForm::LookupByID<RE::TESObjectREFR>(a_event->newContainer) : nullptr;
            
            // Check if diary was removed from an NPC (oldContainer = NPC, newContainer = player or world)
            if (fromRef) {
                auto fromActor = fromRef->As<RE::Actor>();
                if (fromActor && fromActor->GetActorBase()) {
                    // Diary was removed from this NPC - record the time
                    std::string actorName = fromActor->GetActorBase()->GetName();
                    
                    try {
                        // Resolve actor UUID using API
                        std::string actorUuid = ResolveActorUUID(fromActor);
                        
                        if (!actorUuid.empty()) {
                            auto calendar = RE::Calendar::GetSingleton();
                            if (calendar) {
                                // Convert from days to seconds to match SkyrimNet's database format
                                double currentTime = calendar->GetCurrentGameTime() * 86400.0;
                                
                                SKSE::log::info("Diary v{} removed from {} (UUID: {}) at game time {} seconds", 
                                               diaryData->volumeNumber, actorName, actorUuid, currentTime);
                                // Note: Faction marker added by DiaryTheftHandler for SkyrimNet prompt
                            }
                        }
                    } catch (const std::exception& e) {
                        SKSE::log::error("Error tracking stolen diary: {}", e.what());
                    }
                }
            }
            
            // Check if diary was returned to an NPC (oldContainer = player, newContainer = NPC)
            if (toRef) {
                auto toActor = toRef->As<RE::Actor>();
                auto* player = RE::PlayerCharacter::GetSingleton();
                if (toActor && toActor->GetActorBase() && toActor != player) {
                    // Diary was returned to this NPC
                    std::string actorName = toActor->GetActorBase()->GetName();
                    
                    // Get the baseObj to check if it's a diary book
                    auto* baseObj = RE::TESForm::LookupByID(a_event->baseObj);
                    if (baseObj && baseObj->GetFormType() == RE::FormType::Book) {
                        auto* book = baseObj->As<RE::TESObjectBOOK>();
                        if (book) {
                            // Check if this is one of our diary books by looking up in BookManager
                            auto bookManager = SkyrimNetDiaries::BookManager::GetSingleton();
                            auto* bookData = bookManager->GetBookForFormID(book->GetFormID());
                            
                            if (bookData) {
                                // This is a diary book being returned
                                std::string bookName = book->GetName();
                                int volNum = bookData->volumeNumber;
                                SKSE::log::info("Diary '{}' (v{}) being returned to {}", 
                                              bookName, volNum, actorName);
                                
                                // Don't remove the book from BookManager - keep it tracked for history
                                // Just check if it was the latest volume to determine if we should clear session tracking
                                auto* allVolumes = bookManager->GetAllVolumesForActor(bookData->actorUuid);
                                bool wasLatest = true;
                                if (allVolumes) {
                                    for (const auto& vol : *allVolumes) {
                                        if (vol.volumeNumber > volNum) {
                                            wasLatest = false;
                                            break;
                                        }
                                    }
                                }
                                
                                if (wasLatest) {
                                    // Latest volume returned - NPC can now receive new updates (inventory check on next daily update)
                                    std::string actorUuid = ResolveActorUUID(toActor);
                                    
                                    if (!actorUuid.empty()) {
                                        SKSE::log::debug("Latest volume v{} returned to {} (UUID: {})", 
                                                       volNum, actorName, actorUuid);
                                    }
                                } else {
                                    // Old volume returned but newer volumes exist
                                    SKSE::log::debug("Old volume v{} returned - NPC has moved on to newer volumes", volNum);
                                }
                            }
                        }
                    }
                }
            }

            return RE::BSEventNotifyControl::kContinue;
        }

    private:
        ContainerChangedEventSink() = default;
        ContainerChangedEventSink(const ContainerChangedEventSink&) = delete;
        ContainerChangedEventSink& operator=(const ContainerChangedEventSink&) = delete;
    };

    void WriteDiaryEntriesToLog(const std::vector<SkyrimNetDiaries::DiaryEntry>& entries) {
        SKSE::log::debug("========================================");
        SKSE::log::debug("DIARY ENTRIES FOUND: {}", entries.size());
        SKSE::log::debug("========================================");

        for (size_t i = 0; i < entries.size(); ++i) {
            const auto& entry = entries[i];
            
            SKSE::log::debug("");
            SKSE::log::debug("--- Entry {} ---", i + 1);
            SKSE::log::debug("Actor: {} (UUID: {})", entry.actor_name, entry.actor_uuid);
            SKSE::log::debug("Location: {}", entry.location);
            SKSE::log::debug("Emotion: {}", entry.emotion);
            SKSE::log::debug("Date: {}", entry.entry_date);
            SKSE::log::debug("Importance: {}", entry.importance_score);
            SKSE::log::debug("Content:");
            SKSE::log::debug("{}", entry.content);
            SKSE::log::debug("");
        }

        SKSE::log::debug("========================================");
    }

    // DEPRECATED: Database polling functions - no longer needed with API
    /* 
    void TestDatabaseConnection() {
        // This function is deprecated - uses direct database access
    }

    void PollForActiveDatabase() {
        // This function is deprecated - API handles database access
    }

    void UpdateDatabasePath() {
        // This function is deprecated - API handles database path
    }
    */

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

        // Save UUID cache
        if (!a_intfc->OpenRecord(kSerializationTypeCache, kSerializationVersion)) {
            SKSE::log::error("Failed to open cache serialization record");
            return;
        }

        std::uint32_t cacheSize = static_cast<std::uint32_t>(g_actorUuidCache.size());
        if (!a_intfc->WriteRecordData(&cacheSize, sizeof(cacheSize))) {
            SKSE::log::error("Failed to write cache size");
            return;
        }

        for (const auto& [formId, uuid] : g_actorUuidCache) {
            // Write FormID
            if (!a_intfc->WriteRecordData(&formId, sizeof(formId))) {
                SKSE::log::error("Failed to write cached FormID");
                continue;
            }

            // Write UUID length and string
            std::uint32_t uuidLen = static_cast<std::uint32_t>(uuid.length());
            if (!a_intfc->WriteRecordData(&uuidLen, sizeof(uuidLen))) {
                SKSE::log::error("Failed to write UUID length");
                continue;
            }
            if (!a_intfc->WriteRecordData(uuid.c_str(), uuidLen)) {
                SKSE::log::error("Failed to write UUID");
                continue;
            }
        }

        SKSE::log::debug("Saved {} UUID cache entries", cacheSize);
        
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
            else if (type == kSerializationTypeCache) {
                // Load UUID cache
                std::uint32_t cacheSize;
                if (!a_intfc->ReadRecordData(&cacheSize, sizeof(cacheSize))) {
                    SKSE::log::error("Failed to read cache size");
                    continue;
                }

                SKSE::log::debug("Loading {} UUID cache entries", cacheSize);
                g_actorUuidCache.clear();

                for (std::uint32_t i = 0; i < cacheSize; ++i) {
                    // Read FormID and resolve it
                    RE::FormID oldFormId;
                    if (!a_intfc->ReadRecordData(&oldFormId, sizeof(oldFormId))) {
                        SKSE::log::error("Failed to read cached FormID");
                        break;
                    }

                    RE::FormID newFormId;
                    if (!a_intfc->ResolveFormID(oldFormId, newFormId)) {
                        SKSE::log::warn("Failed to resolve cached FormID 0x{:X}, skipping", oldFormId);
                        // Still need to read UUID to advance the stream
                        std::uint32_t uuidLen;
                        if (a_intfc->ReadRecordData(&uuidLen, sizeof(uuidLen))) {
                            std::string dummy;
                            dummy.resize(uuidLen);
                            a_intfc->ReadRecordData(dummy.data(), uuidLen);
                        }
                        continue;
                    }

                    // Read UUID
                    std::uint32_t uuidLen;
                    if (!a_intfc->ReadRecordData(&uuidLen, sizeof(uuidLen))) {
                        SKSE::log::error("Failed to read UUID length");
                        break;
                    }

                    std::string uuid;
                    uuid.resize(uuidLen);
                    if (!a_intfc->ReadRecordData(uuid.data(), uuidLen)) {
                        SKSE::log::error("Failed to read UUID");
                        break;
                    }

                    // Store with resolved FormID
                    g_actorUuidCache[newFormId] = uuid;
                }

                SKSE::log::debug("Restored {} UUID cache entries", g_actorUuidCache.size());
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
        g_actorUuidCache.clear();
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
                // Verify ESP setup now that forms are loaded
                DiaryTheftHandler::VerifyESPSetup();
                
                // Register event sinks on game startup (don't load database yet)
                auto scriptEventSource = RE::ScriptEventSourceHolder::GetSingleton();
                if (scriptEventSource) {
                    scriptEventSource->AddEventSink<RE::TESContainerChangedEvent>(ContainerChangedEventSink::GetSingleton());
                    SKSE::log::debug("Registered TESContainerChangedEvent sink");
                }

                auto ui = RE::UI::GetSingleton();
                if (ui) {
                    ui->AddEventSink<RE::MenuOpenCloseEvent>(BookMenuEventSink::GetSingleton());
                    SKSE::log::debug("Registered BookMenu event sink");
                }

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
                if (t.find("All entries from this time period have been removed") == std::string::npos) {
                    size_t copyLen = std::min(t.size(), sizeof(query->text) - 1);
                    std::memcpy(query->text, t.c_str(), copyLen);
                    query->text[copyLen] = '\0';
                    SKSE::log::debug("SNPD_QUERY_BOOK: returned {} chars for FormID 0x{:X} (vol {}/{}, {} entries)",
                                   copyLen, query->bookFormId, query->volumeNumber, query->totalVolumes, query->entryCount);
                } else {
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
                pages[2].find("All entries from this time period have been removed") != std::string_view::npos) {
                SKSE::log::debug("SNPD_QUERY_ENTRY: FormID 0x{:X} has no entries", query->bookFormId);
                break;
            }

            query->totalEntries = entryPageCount;

            std::int32_t idx = query->entryIndex;
            if (idx < 0) idx = entryPageCount - 1;
            if (idx >= entryPageCount) {
                SKSE::log::warn("SNPD_QUERY_ENTRY: entryIndex {} out of range (0-{})",
                               query->entryIndex, entryPageCount - 1);
                break;
            }

            std::string_view page = pages[static_cast<std::size_t>(idx + 2)];
            while (!page.empty() && (page.back() == '\n' || page.back() == '\r'))
                page.remove_suffix(1);
            query->isValid       = true;
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

            query->isValid = true;

            int entryPageCount = static_cast<int>(pages.size()) - 2;
            if (entryPageCount <= 0 ||
                pages[2].find("All entries from this time period have been removed") != std::string_view::npos) {
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

    // Install book text injection hook (replaces Dynamic Book Framework text delivery,
    // works on both SSE and VR via RELOCATION_ID(50122, 51053)).
    BookTextHook::Install();

    // Register Papyrus native functions
    SKSE::log::debug("Registering Papyrus native functions...");
    PapyrusAPI::Register();

    SKSE::log::info("SkyrimNetPhysicalDiaries loaded successfully!");
    
    return true;
}

