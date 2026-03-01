#include "Database.h"
#include "BookManager.h"
#include "DiaryTheftHandler.h"
#include "DynamicBookFrameworkAPI.h"
#include "SkyrimNetAPITest.h"
#include "Config.h"
#include <spdlog/sinks/basic_file_sink.h>
#include <fstream>
#include <sstream>
#include <unordered_set>
#include <unordered_map>

// Helper function to sanitize text for Skyrim's book renderer
std::string SanitizeBookText(const std::string& text) {
    std::string result = text;
        
        // Strip leading date headers that LLM sometimes includes (e.g., "Sundas, 17th of Last Seed, 4E 201")
        // We add our own date headers, so remove any at the start of the content
        if (result.find("4E ") != std::string::npos) {
            size_t firstNewline = result.find('\n');
            if (firstNewline != std::string::npos && firstNewline < 100) {
                // Check if first line looks like a date (contains day name or "4E")
                std::string firstLine = result.substr(0, firstNewline);
                if (firstLine.find("das,") != std::string::npos || 
                    firstLine.find("4E") != std::string::npos ||
                    (firstLine.length() < 50 && firstLine.find(",") != std::string::npos)) {
                    // Skip the first line and any following blank lines
                    result = result.substr(firstNewline + 1);
                    while (result.length() > 0 && (result[0] == '\n' || result[0] == '\r')) {
                        result = result.substr(1);
                    }
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

    // Track last processed game day for daily updates
    float g_lastProcessedDay = -1.0f;
    double g_lastDiaryCheckTimestamp = 0.0; // Unix timestamp of last diary database check
    constexpr float EVENING_HOUR = 20.0f; // 8 PM - when daily diary updates should occur
    constexpr float TIME_CHECK_INTERVAL = 300.0f; // Check time every 5 minutes of real time (in seconds)

    constexpr std::uint32_t kSerializationVersion = 1;
    constexpr std::uint32_t kSerializationTypeBooks = 'SNDB'; // SkyrimNet Diary Books
    constexpr std::uint32_t kSerializationTypeCache = 'SNDC'; // SkyrimNet Diary Cache (UUID mappings)

    // Forward declarations
    void CheckDailyDiaryUpdates(bool forcedUpdate = false);
    void ScheduleTimeCheck();

} // end anonymous namespace

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
        SKSE::log::info("Resolved actor {} (FormID 0x{:X}) to UUID: {}", actor->GetActorBase()->GetName(), actorRefID, uuid);
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
    bookText = "<font size='" + std::to_string(fontTitle) + "'> </font>\n\n\n\n";
    
    // Title page with centering
    bookText += "<font face='$HandwrittenFont' size='" + std::to_string(fontTitle) + "'><p align='center'>";
    bookText += actorName + "'s Diary";
    bookText += "</p></font>\n\n";
    
    if (entries.empty()) {
        bookText += "\n\n<font size='" + std::to_string(fontContent) + "'><p align='center'>All entries from this time period have been removed.</p></font>";
    } else {
        // Add date range showing first and last entry dates (without day of week for title page)
        std::string firstDate = FormatGameDateShort(entries.front().entry_date);
        std::string lastDate = FormatGameDateShort(entries.back().entry_date);
        
        bookText += "<font size='" + std::to_string(fontSmall) + "'><p align='center'>";
        if (firstDate == lastDate) {
            // All entries on same day - just show single date
            bookText += firstDate;
        } else {
            // Multiple days - show range
            bookText += firstDate + " - " + lastDate;
        }
        bookText += "</p></font>\n\n";
        
        // Page break after title page
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
            bookText += "<font size='" + std::to_string(fontDate) + "'></font>";
            // Date header
            bookText += "<font size='" + std::to_string(fontDate) + "'>" + dateStr + "</font>";
            bookText += "<font size='" + std::to_string(fontContent) + "'></font>\n\n";  // Reset to content font size
            
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
                    bookText += "<font size='" + std::to_string(fontContent) + "'>" + paragraph + "</font>\n\n";
                }
                pos = found + 2;
            }
            // Last paragraph
            std::string lastParagraph = content.substr(pos);
            if (!lastParagraph.empty()) {
                bookText += "<font size='" + std::to_string(fontContent) + "'>" + lastParagraph + "</font>\n\n";
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

// Register diary with Dynamic Book Framework INI
bool RegisterDiaryInDBF(const std::string& bookTitle, const std::string& saveFolder)
{
    try {
        auto iniPath = std::filesystem::current_path() / "Data" / "SKSE" / "Plugins" / "DynamicBookFramework" / "Configs" / "SkyrimNetPhysicalDiaries.ini";
        
        SKSE::log::info("RegisterDiaryInDBF called for '{}' in folder '{}'", bookTitle, saveFolder);
        SKSE::log::info("INI path: {}", iniPath.string());
        
        // Ensure parent directories exist
        std::filesystem::create_directories(iniPath.parent_path());
        SKSE::log::info("Created parent directories");
        
        // Check if this book is already registered (exact match on line start)
        bool needsSection = false;
        bool alreadyRegistered = false;
        if (std::filesystem::exists(iniPath)) {
            SKSE::log::info("INI file exists, checking for existing registration");
            std::ifstream checkFile(iniPath);
            if (checkFile.is_open()) {
                std::string line;
                bool foundSection = false;
                while (std::getline(checkFile, line)) {
                    if (line == "[Books]") {
                        foundSection = true;
                    }
                    // Check for exact match: line starts with "bookTitle = "
                    std::string searchPattern = bookTitle + " = ";
                    if (line.find(searchPattern) == 0) {
                        alreadyRegistered = true;
                        SKSE::log::info("Book '{}' already registered in DBF INI", bookTitle);
                        break;
                    }
                }
                needsSection = !foundSection;
                checkFile.close();
                
                if (alreadyRegistered) {
                    return true;
                }
            }
        } else {
            SKSE::log::info("INI file does not exist, will create");
            needsSection = true;
        }
        
        // Append to INI file
        SKSE::log::info("Opening INI file for append");
        std::ofstream file(iniPath, std::ios::app);
        if (!file.is_open()) {
            SKSE::log::error("Failed to open DBF INI for writing: {}", iniPath.string());
            return false;
        }
        
        // Add [Books] section if needed
        if (needsSection) {
            SKSE::log::info("Adding [Books] section");
            file << "[Books]\n";
        }
        
        // Format: BookTitle = subfolder/Filename.txt
        std::string entry = bookTitle + " = " + saveFolder + "/" + bookTitle + ".txt\n";
        SKSE::log::info("Writing entry: {}", entry);
        file << entry;
        file.close();
        
        SKSE::log::info("Successfully registered '{}' in DBF INI at: {}", bookTitle, iniPath.string());
        return true;
        
    } catch (const std::exception& e) {
        SKSE::log::error("Exception registering diary in DBF INI: {}", e.what());
        return false;
    }
}

// Write diary text to file for Dynamic Book Framework
bool WriteDynamicBookFile(const std::string& bookTitle, const std::string& text)
{
    auto booksPath = std::filesystem::current_path() / "Data" / "SKSE" / "Plugins" / "DynamicBookFramework" / "Books";
    
    try {
        // Use fixed folder name for SkyrimNet diaries
        std::string saveFolder = "SkyrimNet";
        
        auto saveBooksPath = booksPath / saveFolder;
        std::filesystem::create_directories(saveBooksPath);
        
        auto filePath = saveBooksPath / (bookTitle + ".txt");
        std::ofstream file(filePath);
        
        if (!file.is_open()) {
            SKSE::log::error("Failed to open file for writing: {}", filePath.string());
            return false;
        }
        
        file << text;
        file.close();
        
        SKSE::log::info("Wrote book file: {}", filePath.string());
        
        // Register in INI with subfolder path
        RegisterDiaryInDBF(bookTitle, saveFolder);
        
        // Reload DBF mappings
        DynamicBookFramework_API::ReloadBookMappings();
        
        return true;
        
    } catch (const std::exception& e) {
        SKSE::log::error("Exception writing diary file: {}", e.what());
        return false;
    }
}

namespace {

    // BookMenu event sink for tracking when books open
    class BookMenuEventSink : public RE::BSTEventSink<RE::MenuOpenCloseEvent> {
    public:
        static BookMenuEventSink* GetSingleton() {
            static BookMenuEventSink singleton;
            return &singleton;
        }

        std::chrono::steady_clock::time_point bookOpenTime;
        bool pendingJump = false;

        RE::BSEventNotifyControl ProcessEvent(const RE::MenuOpenCloseEvent* a_event, 
                                               RE::BSTEventSource<RE::MenuOpenCloseEvent>*) override {
            if (!a_event || a_event->menuName != RE::BookMenu::MENU_NAME) {
                return RE::BSEventNotifyControl::kContinue;
            }

            // Only process when book opens
            if (a_event->opening) {
                // Get the book that was opened
                auto* book = RE::BookMenu::GetTargetForm();
                if (!book) {
                    return RE::BSEventNotifyControl::kContinue;
                }
                
                RE::FormID bookFormID = book->GetFormID();
                auto* bookManager = SkyrimNetDiaries::BookManager::GetSingleton();
                
                // Check if this is one of our diary books
                auto volumeInfo = bookManager->GetBookByFormID(bookFormID);
                if (volumeInfo.has_value()) {
                    SKSE::log::info("Diary book opened: {} (FormID: 0x{:X}) - restoring content", 
                                   volumeInfo->actorName, bookFormID);
                    
                    // Build book name
                    std::string bookName = volumeInfo->actorName + "'s Diary";
                    if (volumeInfo->volumeNumber > 1) {
                        bookName += ", v" + std::to_string(volumeInfo->volumeNumber);
                    }
                    
                    // Read content from txt file
                    std::string saveFolder = "SkyrimNet";
                    std::filesystem::path bookPath = std::filesystem::path("Data/SKSE/Plugins/DynamicBookFramework/Books") / saveFolder / (bookName + ".txt");
                    
                    std::ifstream fileStream(bookPath);
                    if (fileStream.is_open()) {
                        std::stringstream buffer;
                        buffer << fileStream.rdbuf();
                        std::string bookText = buffer.str();
                        fileStream.close();
                        
                        // Pass content to DBF for display (session-only, doesn't rewrite file)
                        DynamicBookFramework_API::SetDynamicText(bookName.c_str(), bookText.c_str());
                        
                        SKSE::log::info("Restored diary content from file: {}", bookPath.string());
                    } else {
                        SKSE::log::error("Failed to read diary file: {}", bookPath.string());
                    }
                }
            }

            return RE::BSEventNotifyControl::kContinue;
        }

    private:
        BookMenuEventSink() = default;
        BookMenuEventSink(const BookMenuEventSink&) = delete;
        BookMenuEventSink& operator=(const BookMenuEventSink&) = delete;
    };

    // Sleep event sink to check for diary updates after sleeping
    class SleepEventSink : public RE::BSTEventSink<RE::TESSleepStopEvent> {
    public:
        static SleepEventSink* GetSingleton() {
            static SleepEventSink singleton;
            return &singleton;
        }

        RE::BSEventNotifyControl ProcessEvent(const RE::TESSleepStopEvent* a_event,
                                               RE::BSTEventSource<RE::TESSleepStopEvent>*) override {
            if (!a_event) {
                return RE::BSEventNotifyControl::kContinue;
            }

            SKSE::log::info("Player finished sleeping (interrupted: {}) - scheduling delayed diary update", a_event->interrupted);
            
            // Delay the update to ensure the game is in a stable state after sleep
            auto task = SKSE::GetTaskInterface();
            if (task) {
                task->AddTask([]() {
                    SKSE::log::info("Executing delayed diary update after sleep");
                    CheckDailyDiaryUpdates(true);
                    
                    // Run API tests to verify diary API integration
                    SKSE::log::info("Running SkyrimNet API tests after sleep...");
                    SkyrimNetAPITest::RunAllTests();
                });
            }

            return RE::BSEventNotifyControl::kContinue;
        }

    private:
        SleepEventSink() = default;
        SleepEventSink(const SleepEventSink&) = delete;
        SleepEventSink& operator=(const SleepEventSink&) = delete;
    };

    // Wait event sink to check for diary updates after waiting
    class WaitEventSink : public RE::BSTEventSink<RE::TESWaitStopEvent> {
    public:
        static WaitEventSink* GetSingleton() {
            static WaitEventSink singleton;
            return &singleton;
        }

        RE::BSEventNotifyControl ProcessEvent(const RE::TESWaitStopEvent* a_event,
                                               RE::BSTEventSource<RE::TESWaitStopEvent>*) override {
            if (!a_event) {
                return RE::BSEventNotifyControl::kContinue;
            }

            SKSE::log::info("Player finished waiting (interrupted: {}) - scheduling delayed diary update", a_event->interrupted);
            
            // Delay the update to ensure the game is in a stable state after wait
            auto task = SKSE::GetTaskInterface();
            if (task) {
                task->AddTask([]() {
                    SKSE::log::info("Executing delayed diary update after wait");
                    CheckDailyDiaryUpdates(true);
                    
                    // Run API tests to verify diary API integration
                    SKSE::log::info("Running SkyrimNet API tests after wait...");
                    SkyrimNetAPITest::RunAllTests();
                });
            }

            return RE::BSEventNotifyControl::kContinue;
        }

    private:
        WaitEventSink() = default;
        WaitEventSink(const WaitEventSink&) = delete;
        WaitEventSink& operator=(const WaitEventSink&) = delete;
    };

    // Periodic time check function (runs every few minutes)
    void CheckGameTime()
    {
        auto calendar = RE::Calendar::GetSingleton();
        if (!calendar) {
            ScheduleTimeCheck(); // Reschedule
            return;
        }

        float currentDay = calendar->GetDaysPassed();
        float currentHour = calendar->GetHour();
        
        // Check if we've crossed into a new day and it's evening
        if (static_cast<int>(currentDay) != static_cast<int>(g_lastProcessedDay)) {
            if (currentHour >= EVENING_HOUR || g_lastProcessedDay < 0.0f) {
                SKSE::log::info("Daily diary check triggered at day {:.2f}, hour {:.2f}", 
                              currentDay, currentHour);
                CheckDailyDiaryUpdates();
            }
        }
        
        // Schedule next check
        ScheduleTimeCheck();
    }

    // Schedule the next time check
    void ScheduleTimeCheck()
    {
        auto taskInterface = SKSE::GetTaskInterface();
        if (taskInterface) {
            // Schedule to run again after interval (in milliseconds)
            taskInterface->AddTask([]() {
                // Use a delayed task to run after TIME_CHECK_INTERVAL seconds
                std::thread([]() {
                    std::this_thread::sleep_for(std::chrono::milliseconds(static_cast<int>(TIME_CHECK_INTERVAL * 1000)));
                    SKSE::GetTaskInterface()->AddTask(CheckGameTime);
                }).detach();
            });
        }
    }

    void CheckDailyDiaryUpdates(bool forcedUpdate)
    {
        SKSE::log::info("=== CheckDailyDiaryUpdates called (forcedUpdate={}) ===", forcedUpdate);
        
        auto calendar = RE::Calendar::GetSingleton();
        if (!calendar) {
            SKSE::log::error("Calendar is null - cannot check diaries");
            return;
        }
        
        // Initialize SkyrimNet API if not already done
        if (!SkyrimNetDiaries::Database::InitializeAPI()) {
            SKSE::log::error("Failed to initialize SkyrimNet API - diaries cannot be created");
            return;
        }
        
        // Check if memory system is ready
        if (!SkyrimNetDiaries::Database::IsMemorySystemReady()) {
            SKSE::log::warn("SkyrimNet memory system not ready yet - diary processing deferred");
            return;
        }

        float currentDay = calendar->GetDaysPassed();
        
        // Only process once per game day (but allow forced updates from sleep/wait)
        if (!forcedUpdate && g_lastProcessedDay >= 0.0f && static_cast<int>(currentDay) == static_cast<int>(g_lastProcessedDay)) {
            SKSE::log::info("Already processed diaries today (day {:.2f}), skipping", currentDay);
            return;
        }

        SKSE::log::info("{} game day detected ({:.2f}) - processing diary updates", 
                      forcedUpdate ? "Forced update" : "New", currentDay);
        g_lastProcessedDay = currentDay;

        // Structure to cache actor data from API
        struct CachedActorData {
            std::string uuid;
            std::string name;
            std::string bioTemplateName;  // For ESL actor lookup (name + last 3 digits)
            uint32_t formID;
            std::vector<SkyrimNetDiaries::DiaryEntry> allEntries;
        };

        // Process all actors with diary entries
        try {
            double currentGameTime = calendar->GetCurrentGameTime() * 86400.0;
            SKSE::log::info("Starting diary processing for game time {:.2f}", currentGameTime);

            // STEP 1: Quickly batch all database queries and close connection immediately
            // Database is opened in READ-ONLY mode to avoid blocking SkyrimNet's writes
            // Uses API instead of direct database access - no retry logic needed
            std::unordered_map<std::string, CachedActorData> cachedData;
            {
                SKSE::log::info("Fetching diary entries via SkyrimNet API (thread-safe)...");

                try {
                    // PERFORMANCE OPTIMIZATION: Fetch and group ALL diary entries in ONE API call
                    // This eliminates 50+ redundant GetDiaryEntries calls on first load
                    SKSE::log::info("Querying for entries since timestamp: {:.2f} (current game time: {:.2f})", 
                                  g_lastDiaryCheckTimestamp, currentGameTime);
                    auto entriesByActor = SkyrimNetDiaries::Database::GetAllDiaryEntriesGroupedByActor(g_lastDiaryCheckTimestamp);
                    SKSE::log::info("Found {} actors with diary entries", entriesByActor.size());
                
                // Also check all existing diary holders for deletions
                auto bookManager = SkyrimNetDiaries::BookManager::GetSingleton();
                std::unordered_set<std::string> actorsToProcess;
                
                // Add actors with new entries from the grouped data
                for (const auto& [uuid, entries] : entriesByActor) {
                    if (!entries.empty()) {
                        SKSE::log::info("  - {} ({} entries, UUID: {})", entries[0].actor_name, entries.size(), uuid);
                        actorsToProcess.insert(uuid);
                    }
                }
                
                // Add actors with existing diaries (to check for deletions/updates)
                int existingDiaryHolders = 0;
                for (const auto& [uuid, volumes] : bookManager->GetAllBooks()) {
                    actorsToProcess.insert(uuid);
                    existingDiaryHolders++;
                }
                
                if (actorsToProcess.empty()) {
                    SKSE::log::warn("No diary updates needed: {} actors with new entries, {} existing diary holders", 
                                   entriesByActor.size(), existingDiaryHolders);
                    SKSE::log::warn("If you expect diary entries, check that SkyrimNet is writing to the database");
                    // Only update timestamp after successful processing
                    g_lastDiaryCheckTimestamp = currentGameTime;
                    return;
                }

                SKSE::log::info("Prefetching data for {} actors...", actorsToProcess.size());

                // Batch-fetch ALL data we need (using pre-grouped entries from above)
                for (const auto& uuid : actorsToProcess) {
                    CachedActorData data;
                    data.uuid = uuid;
                    data.name = SkyrimNetDiaries::Database::GetActorName(uuid);
                    data.bioTemplateName = SkyrimNetDiaries::Database::GetTemplateNameByUUID(uuid);
                    data.formID = SkyrimNetDiaries::Database::GetFormIDForUUID(uuid);
                    
                    SKSE::log::info("Cached actor: name='{}', formID=0x{:X}, bioTemplate='{}'", data.name, data.formID, data.bioTemplateName);
                    
                    // Use pre-fetched entries if available, otherwise query API
                    auto entriesIt = entriesByActor.find(uuid);
                    if (entriesIt != entriesByActor.end()) {
                        data.allEntries = entriesIt->second;  // OPTIMIZATION: Use cached entries
                        SKSE::log::info("Using pre-fetched {} entries for {}", data.allEntries.size(), data.name);
                    } else {
                        // Existing diary holder with no new entries - fetch all their entries
                        data.allEntries = SkyrimNetDiaries::Database::GetDiaryEntries(uuid, 10000, 0.0);
                        SKSE::log::info("Fetched {} entries for existing diary holder {}", data.allEntries.size(), data.name);
                    }
                    
                    cachedData[uuid] = std::move(data);
                    SKSE::log::info("Cached {} entries for {} (FormID: 0x{:X})", 
                                   data.allEntries.size(), data.name, data.formID);
                }
                
                } catch (const std::exception& e) {
                    SKSE::log::error("API query error: {}", e.what());
                    SKSE::log::info("Diary processing aborted");
                    return;
                }

               SKSE::log::info("All data fetched from API. Processing {} actors...", cachedData.size());
            }

            // STEP 2: Process all actors from cached data
            auto bookManager = SkyrimNetDiaries::BookManager::GetSingleton();
            int diariesCreated = 0;
            int diariesUpdated = 0;

            for (const auto& [uuid, cachedActor] : cachedData) {
                try {
                    // Use cached data instead of database calls
                    const std::string& actorName = cachedActor.name;
                    const uint32_t actorFormID = cachedActor.formID;
                    
                    if (actorFormID == 0) {
                        SKSE::log::warn("Skipping {} - no FormID found", actorName);
                        continue;
                    }

                    // Helper lambda to filter diary entries by time range from cached data
                    // NOTE: For subsequent volumes, exclude boundary to avoid duplicate entries
                    // IMPORTANT: Filters out entries dated in the future (handles old save + kept future events)
                    auto filterEntries = [&cachedActor, currentGameTime](double startTime, double endTime = 0.0, int maxCount = 10000) {
                        std::vector<SkyrimNetDiaries::DiaryEntry> filtered;
                        for (const auto& entry : cachedActor.allEntries) {
                            // First volume (startTime==0): include all from start
                            // Subsequent volumes: use > to exclude previous volume's last entry (prevents duplicates)  
                            bool afterStart = (startTime == 0.0) ? true : (entry.entry_date > startTime);
                            
                            // CRITICAL: Never include entries dated in the future relative to current save time
                            // This handles the case where user loads an old save but SkyrimNet kept future event history
                            bool notInFuture = (entry.entry_date <= currentGameTime);
                            
                            if (afterStart && notInFuture && (endTime == 0.0 || entry.entry_date <= endTime)) {
                                filtered.push_back(entry);
                                if (static_cast<int>(filtered.size()) >= maxCount) break;
                            }
                        }
                        return filtered;
                    };

                    auto latestVolume = bookManager->GetBookForActor(uuid);
                    
                    // Check if actor has an existing diary volume
                    if (latestVolume) {
                        // Check if NPC still has the book in their inventory
                        auto npcActor = RE::TESForm::LookupByID<RE::Actor>(actorFormID);
                        if (npcActor) {
                            auto inv = npcActor->GetInventory();
                            bool npcHasBook = false;
                            
                            for (const auto& [item, invData] : inv) {
                                if (item->GetFormID() == latestVolume->bookFormId && invData.first > 0) {
                                    npcHasBook = true;
                                    break;
                                }
                            }
                            
                            if (npcHasBook) {
                                // Check for new entries since this volume's endTime
                                double currentTime = calendar->GetCurrentGameTime() * 86400.0;
                                
                                // First, check how many entries this volume currently has
                                const int MAX_ENTRIES_PER_VOLUME = SkyrimNetDiaries::Config::GetSingleton()->GetEntriesPerVolume();
                                auto currentVolumeEntries = filterEntries(latestVolume->startTime, latestVolume->endTime, MAX_ENTRIES_PER_VOLUME);
                                bool volumeIsFull = (currentVolumeEntries.size() >= MAX_ENTRIES_PER_VOLUME);
                                
                                // DELETION DETECTION: Check if entries were deleted from this volume's time range
                                // We can't track the exact count, but if the volume was supposedly full and now isn't, entries were deleted
                                bool possibleDeletion = false;
                                if (volumeIsFull && currentVolumeEntries.size() < MAX_ENTRIES_PER_VOLUME) {
                                    SKSE::log::warn("{} volume {} may have had entries deleted (expected {}, found {})", 
                                                   actorName, latestVolume->volumeNumber, MAX_ENTRIES_PER_VOLUME, currentVolumeEntries.size());
                                    possibleDeletion = true;
                                }
                                
                                // Check for new entries after this volume's endTime
                                auto newEntries = filterEntries(latestVolume->endTime, currentTime, 10000);
                                
                                if (!newEntries.empty() || possibleDeletion) {
                                    if (volumeIsFull && !possibleDeletion) {
                                        // Volume is full - create new volumes for new entries
                                        SKSE::log::info("{} volume {} is full ({} entries), creating new volume(s) for {} new entries", 
                                                          actorName, latestVolume->volumeNumber, MAX_ENTRIES_PER_VOLUME, newEntries.size());
                                        
                                        int totalNewEntries = static_cast<int>(newEntries.size());
                                        int volumesNeeded = (totalNewEntries + MAX_ENTRIES_PER_VOLUME - 1) / MAX_ENTRIES_PER_VOLUME;
                                        
                                        // Use background thread to stagger creation without freezing game
                                        std::thread([volumesNeeded, latestVolume, newEntries, MAX_ENTRIES_PER_VOLUME, totalNewEntries,
                                                    uuid, actorName, actorFormID, bookManager, bioTemplateName = cachedActor.bioTemplateName]() {
                                            for (int i = 0; i < volumesNeeded; ++i) {
                                                int volumeNumber = latestVolume->volumeNumber + 1 + i;
                                                int entryStartIndex = i * MAX_ENTRIES_PER_VOLUME;
                                                int entryEndIndex = std::min((i + 1) * MAX_ENTRIES_PER_VOLUME, totalNewEntries);
                                                
                                                double volumeStartTime = newEntries[entryStartIndex].entry_date;
                                                double volumeEndTime = newEntries[entryEndIndex - 1].entry_date;
                                                
                                                // Extract entries for this volume to avoid DB reopening in callback
                                                std::vector<SkyrimNetDiaries::DiaryEntry> volumeEntries(
                                                    newEntries.begin() + entryStartIndex, 
                                                    newEntries.begin() + entryEndIndex
                                                );
                                                
                                                // Stagger with 100ms delay (on background thread - doesn't freeze game)
                                                if (i > 0) {
                                                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                                                }
                                                
                                                bookManager->CreateDiaryBook(uuid, actorName, volumeStartTime, volumeEndTime, volumeNumber, actorFormID, volumeEntries, bioTemplateName);
                                                SKSE::log::info("Created new volume {} for {} with {} entries (no DB access needed)", 
                                                              volumeNumber, actorName, volumeEntries.size());
                                                
                                                // Clear stolen diary marker after first new volume (NPC has now written about it)
                                                if (i == 0) {
                                                    auto* npcActor = RE::TESForm::LookupByID<RE::Actor>(actorFormID);
                                                    if (npcActor && DiaryTheftHandler::ClearStolenDiaryMarker(npcActor)) {
                                                        SKSE::log::info("{} wrote about stolen diary in new volume - marker cleared", actorName);
                                                    }
                                                }
                                            }
                                        }).detach();
                                        diariesCreated += volumesNeeded;
                                    } else {
                                        // Volume has < max entries - update it with new entries (up to max total)
                                        // OR entries were deleted and we need to update the book
                                        if (possibleDeletion) {
                                            SKSE::log::info("Detected deletions in {} volume {} - updating book content", 
                                                          actorName, latestVolume->volumeNumber);
                                        } else {
                                            SKSE::log::info("Found {} new entries for {} since volume {} endTime {}", 
                                                          newEntries.size(), actorName, latestVolume->volumeNumber, latestVolume->endTime);
                                        }
                                        
                                        // Get all entries for this volume (from startTime to now, up to max entries)
                                        auto allVolumeEntries = filterEntries(latestVolume->startTime, currentTime, MAX_ENTRIES_PER_VOLUME);
                                    
                                        if (!allVolumeEntries.empty()) {
                                            // Reformat the book with updated entries (reflects both additions and deletions)
                                            std::string bookText = FormatDiaryEntries(allVolumeEntries, actorName, 
                                                                                     latestVolume->startTime, currentTime, MAX_ENTRIES_PER_VOLUME);
                                            
                                            // Update the book text via file persistence
                                            std::string bookName = actorName + "'s Diary";
                                            if (latestVolume->volumeNumber > 1) {
                                                bookName += ", v" + std::to_string(latestVolume->volumeNumber);
                                            }
                                            
                                            // Write to file, will display on next book open
                                            WriteDynamicBookFile(bookName, bookText);
                                            
                                            // Update the volume's endTime
                                            double newEndTime = allVolumeEntries.back().entry_date;
                                            bookManager->UpdateBookEndTime(uuid, latestVolume->volumeNumber, newEndTime);
                                            
                                            SKSE::log::info("Updated {} volume {} with {} total entries (endTime: {})", 
                                                          actorName, latestVolume->volumeNumber, allVolumeEntries.size(), newEndTime);
                                            diariesUpdated++;
                                            
                                            // Clear stolen diary marker if present (NPC has now written about it)
                                            if (DiaryTheftHandler::ClearStolenDiaryMarker(npcActor)) {
                                                SKSE::log::info("{} wrote about stolen diary - marker cleared", actorName);
                                            }
                                        } else {
                                            // All entries were deleted - the book becomes empty
                                            SKSE::log::warn("{} volume {} has no remaining entries - all were deleted", 
                                                          actorName, latestVolume->volumeNumber);
                                            
                                            std::string bookName = actorName + "'s Diary";
                                            if (latestVolume->volumeNumber > 1) {
                                                bookName += ", v" + std::to_string(latestVolume->volumeNumber);
                                            }
                                            
                                            // Create empty diary with deletion notice
                                            std::string emptyBookText = FormatDiaryEntries({}, actorName, 
                                                                                          latestVolume->startTime, currentTime, MAX_ENTRIES_PER_VOLUME);
                                            // Write to file, will display on next book open
                                            WriteDynamicBookFile(bookName, emptyBookText);
                                            diariesUpdated++;
                                        }
                                    }
                                }
                                
                                // NPC has diary and it's updated - skip creation
                                continue;
                            } else {
                                // NPC doesn't have the book anymore (player stole it or it was lost)
                                SKSE::log::info("{} no longer has volume {} - checking if new volume needed", 
                                              actorName, latestVolume->volumeNumber);
                            }
                        }
                    }

                    // NPC doesn't have latest volume (stolen/lost) OR no diary exists yet
                    // Determine starting point for new volume
                    double startTime = 0.0;
                    int startingVolumeNumber = 1;
                    
                    // Check if we have an existing diary record
                    if (latestVolume && latestVolume->endTime > 0.0) {
                        startingVolumeNumber = latestVolume->volumeNumber + 1;
                        startTime = latestVolume->endTime;
                        SKSE::log::info("{} has existing volume {}, next volume will be {} starting from {}", 
                                       actorName, latestVolume->volumeNumber, startingVolumeNumber, startTime);
                    }

                    // Get all entries from startTime onward (from cached data)
                    SKSE::log::info("Filtering cached entries for {} (UUID: {}) from timestamp {}", 
                                   actorName, uuid, startTime);
                    auto allEntries = filterEntries(startTime, 0.0, 10000);
                    
                    if (allEntries.empty()) {
                        SKSE::log::warn("No diary entries found for {} (UUID: {}) - no book will be created", 
                                       actorName, uuid);
                        continue;
                    }
                    
                    SKSE::log::info("Found {} diary entries for {}", allEntries.size(), actorName);

                    const int ENTRIES_PER_VOLUME = SkyrimNetDiaries::Config::GetSingleton()->GetEntriesPerVolume();
                    int totalEntries = static_cast<int>(allEntries.size());
                    int volumesNeeded = (totalEntries + ENTRIES_PER_VOLUME - 1) / ENTRIES_PER_VOLUME;
                    
                    SKSE::log::info("NPC {} has {} entries, creating {} volume(s) starting from v{}", 
                                  actorName, totalEntries, volumesNeeded, startingVolumeNumber);

                    // Create all needed volumes with staggered timing to avoid frame drops
                    // Use background thread to handle delays without freezing game
                    std::thread([volumesNeeded, startingVolumeNumber, allEntries, ENTRIES_PER_VOLUME, totalEntries, 
                                uuid, actorName, actorFormID, bookManager, bioTemplateName = cachedActor.bioTemplateName]() {
                        for (int i = 0; i < volumesNeeded; ++i) {
                            int volumeNumber = startingVolumeNumber + i;
                            int entryStartIndex = i * ENTRIES_PER_VOLUME;
                            int entryEndIndex = std::min((i + 1) * ENTRIES_PER_VOLUME, totalEntries);
                            
                            // Use the first and last entry timestamps as boundaries for this volume
                            double volumeStartTime = allEntries[entryStartIndex].entry_date;
                            double volumeEndTime = allEntries[entryEndIndex - 1].entry_date;
                            
                            // Extract entries for this volume to avoid DB reopening in callback
                            std::vector<SkyrimNetDiaries::DiaryEntry> volumeEntries(
                                allEntries.begin() + entryStartIndex, 
                                allEntries.begin() + entryEndIndex
                            );
                            
                            // Stagger with 100ms delay per volume (on background thread - doesn't freeze game)
                            if (i > 0) {
                                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                            }
                            
                            SKSE::log::info("Creating volume {} with entries {}-{} (time {}-{})", 
                                          volumeNumber, entryStartIndex + 1, entryEndIndex, volumeStartTime, volumeEndTime);
                            
                            bookManager->CreateDiaryBook(uuid, actorName, volumeStartTime, volumeEndTime, volumeNumber, actorFormID, volumeEntries, bioTemplateName);
                            
                            // Clear stolen diary marker after first volume (NPC has now written about it)
                            if (i == 0) {
                                auto* npcActor = RE::TESForm::LookupByID<RE::Actor>(actorFormID);
                                if (npcActor && DiaryTheftHandler::ClearStolenDiaryMarker(npcActor)) {
                                    SKSE::log::info("{} wrote about stolen diary in first volume - marker cleared", actorName);
                                }
                            }
                        }
                    }).detach();

                    SKSE::log::info("Dispatched {} diary volume(s) for {} - target actor 0x{:X}", 
                                  volumesNeeded, actorName, actorFormID);
                    
                    diariesCreated += volumesNeeded;

                } catch (const std::exception& e) {
                    SKSE::log::error("Error processing actor diary: {}", e.what());
                }
            }

            // Database was already closed after fetching data
            
            // Update last check timestamp
            g_lastDiaryCheckTimestamp = currentGameTime;
            SKSE::log::info("Diary processing complete: {} created, {} updated (timestamp: {:.2f})", 
                          diariesCreated, diariesUpdated, currentGameTime);

        } catch (const std::exception& e) {
            SKSE::log::error("Exception during daily diary check: {}", e.what());
        }
    }


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
                if (toActor && toActor->GetActorBase()) {
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
                                        SKSE::log::info("Latest volume v{} returned to {} (UUID: {})", 
                                                       volNum, actorName, actorUuid);
                                    }
                                } else {
                                    // Old volume returned but newer volumes exist
                                    SKSE::log::info("Old volume v{} returned - NPC has moved on to newer volumes", volNum);
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
        SKSE::log::info("========================================");
        SKSE::log::info("DIARY ENTRIES FOUND: {}", entries.size());
        SKSE::log::info("========================================");

        for (size_t i = 0; i < entries.size(); ++i) {
            const auto& entry = entries[i];
            
            SKSE::log::info("");
            SKSE::log::info("--- Entry {} ---", i + 1);
            SKSE::log::info("Actor: {} (UUID: {})", entry.actor_name, entry.actor_uuid);
            SKSE::log::info("Location: {}", entry.location);
            SKSE::log::info("Emotion: {}", entry.emotion);
            SKSE::log::info("Date: {}", entry.entry_date);
            SKSE::log::info("Importance: {}", entry.importance_score);
            SKSE::log::info("Content:");
            SKSE::log::info("{}", entry.content);
            SKSE::log::info("");
        }

        SKSE::log::info("========================================");
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

        SKSE::log::info("Saved {} UUID cache entries", cacheSize);
    }

    void LoadCallback(SKSE::SerializationInterface* a_intfc) {
        
        std::uint32_t type;
        std::uint32_t version;
        std::uint32_t length;

        while (a_intfc->GetNextRecordInfo(type, version, length)) {
            if (version != kSerializationVersion) {
                SKSE::log::error("Serialization version mismatch for type {}: expected {}, got {}", type, kSerializationVersion, version);
                continue;
            }

            if (type == kSerializationTypeBooks) {
                SkyrimNetDiaries::BookManager::GetSingleton()->Load(a_intfc);
            }
            else if (type == kSerializationTypeCache) {
                // Load UUID cache
                std::uint32_t cacheSize;
                if (!a_intfc->ReadRecordData(&cacheSize, sizeof(cacheSize))) {
                    SKSE::log::error("Failed to read cache size");
                    continue;
                }

                SKSE::log::info("Loading {} UUID cache entries", cacheSize);
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

                SKSE::log::info("Restored {} UUID cache entries", g_actorUuidCache.size());
            }
        }
    }

    void RevertCallback([[maybe_unused]] SKSE::SerializationInterface* a_intfc) {
        // Called when starting a new game - clear all books and tracking
        SkyrimNetDiaries::BookManager::GetSingleton()->Revert();
        g_actorUuidCache.clear();
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
                    SKSE::log::info("Registered TESContainerChangedEvent sink");

                    scriptEventSource->AddEventSink<RE::TESSleepStopEvent>(SleepEventSink::GetSingleton());
                    SKSE::log::info("Registered TESSleepStopEvent sink");

                    scriptEventSource->AddEventSink<RE::TESWaitStopEvent>(WaitEventSink::GetSingleton());
                    SKSE::log::info("Registered TESWaitStopEvent sink");
                }

                auto ui = RE::UI::GetSingleton();
                if (ui) {
                    ui->AddEventSink<RE::MenuOpenCloseEvent>(BookMenuEventSink::GetSingleton());
                    SKSE::log::info("Registered BookMenu event sink");
                }
                
                // Start periodic time checking for daily updates
                SKSE::log::info("Starting periodic game time monitoring (every {:.0f} seconds)", TIME_CHECK_INTERVAL);
                ScheduleTimeCheck();

            } catch (const std::exception& e) {
                SKSE::log::error("Exception in kDataLoaded: {}", e.what());
            } catch (...) {
                SKSE::log::error("Unknown exception in kDataLoaded");
            }
            break;
        }
        case SKSE::MessagingInterface::kPostLoadGame: {
            // Initialize SkyrimNet API after save load
            SKSE::log::info("Save loaded - initializing SkyrimNet API");
            
            if (SkyrimNetDiaries::Database::InitializeAPI()) {
                SKSE::log::info("✓ SkyrimNet API ready");
            } else {
                SKSE::log::warn("Failed to initialize API (SkyrimNet may not be loaded yet)");
            }
            
            // Initialize last processed day to current day to prevent immediate processing on load
            if (auto calendar = RE::Calendar::GetSingleton()) {
                g_lastProcessedDay = calendar->GetDaysPassed();
                // DON'T set g_lastDiaryCheckTimestamp here - need to process existing entries!
                // It will be set to 0.0 by default, which will catch all existing entries on first check
                SKSE::log::info("Initialized daily check at day {:.2f} (timestamp at 0.0 to process existing entries)", 
                              g_lastProcessedDay);
            }
            break;
        }
        case SKSE::MessagingInterface::kNewGame:
            SKSE::log::info("New game started - resetting diary state");
            // Reset tracking for new game
            g_lastProcessedDay = -1.0f;
            g_lastDiaryCheckTimestamp = 0.0;
            break;
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

        SKSE::log::info("SkyrimNetPhysicalDiaries v{}", "1.0.0");
    }
}

extern "C" DLLEXPORT constinit auto SKSEPlugin_Version = []() {
    SKSE::PluginVersionData v{};
    v.PluginVersion({ 1, 0, 0, 0 });
    v.PluginName("SkyrimNetPhysicalDiaries");
    v.AuthorName("SkyrimModder");
    v.UsesAddressLibrary(true);
    v.HasNoStructUse(true);
    return v;
}();

extern "C" DLLEXPORT bool SKSEAPI SKSEPlugin_Query(const SKSE::QueryInterface* a_skse, SKSE::PluginInfo* a_info)
{
    a_info->infoVersion = SKSE::PluginInfo::kVersion;
    a_info->name = "SkyrimNetPhysicalDiaries";
    a_info->version = 1;

    if (a_skse->IsEditor()) {
        return false;
    }

    return true;
}

extern "C" DLLEXPORT bool SKSEAPI SKSEPlugin_Load(const SKSE::LoadInterface* a_skse)
{
    InitializeLog();
    SKSE::log::info("Loading SkyrimNetPhysicalDiaries...");

    SKSE::Init(a_skse);
    
    // Load configuration
    auto configPath = std::filesystem::current_path() / "Data" / "SKSE" / "Plugins" / "SkyrimNetPhysicalDiaries.ini";
    SkyrimNetDiaries::Config::GetSingleton()->Load(configPath);
    
    SKSE::log::info("Registering for SKSE messaging interface...");
    auto messaging = SKSE::GetMessagingInterface();
    if (messaging) {
        messaging->RegisterListener(OnMessage);
    }

    SKSE::log::info("Registering for SKSE serialization...");
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
    SKSE::log::info("Registering diary theft/return event handler...");
    DiaryTheftHandler::Register();

    SKSE::log::info("SkyrimNetPhysicalDiaries loaded successfully!");
    
    return true;
}

