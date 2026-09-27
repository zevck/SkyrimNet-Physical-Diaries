#include "BookText.h"
#include "Config.h"
#include "Localization.h"

namespace SkyrimNetDiaries {

    namespace {

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

    } // namespace

    // Format diary entries - accessible from BookManager
    // maxEntries: Maximum number of entries to include in this book (default 10)
    std::string FormatDiaryEntries(const std::vector<SkyrimNetDiaries::DiaryEntry>& entries,
                                   const std::string& actorName,
                                   double startTime, double endTime,
                                   int maxEntries) {
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

} // namespace SkyrimNetDiaries
