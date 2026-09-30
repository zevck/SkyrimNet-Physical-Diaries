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

#include "BookText.h"
#include "Config.h"
#include "Localization.h"
#include <cmath>

namespace SkyrimNetDiaries {

    namespace {

        // Drops the first line and the blank lines after it ("" if there is only one line).
        void DropFirstLine(std::string& text) {
            const std::size_t nl = text.find('\n');
            if (nl == std::string::npos) {
                text.clear();
                return;
            }
            text.erase(0, nl + 1);
            const std::size_t body = text.find_first_not_of("\r\n");
            text.erase(0, body == std::string::npos ? text.size() : body);
        }

        void ReplaceAll(std::string& text, std::string_view from, std::string_view to) {
            for (std::size_t pos = 0; (pos = text.find(from, pos)) != std::string::npos; pos += to.size()) {
                text.replace(pos, from.size(), to);
            }
        }

        // LLM text goes into Scaleform's book HTML.  A "<" would start a tag and swallow
        // text ("<sigh>", "<3"), so escape the three markup characters.  The
        // inter-plugin API turns them back before handing text to other mods.
        std::string EscapeMarkup(std::string text) {
            std::string out;
            out.reserve(text.size());
            for (const char c : text) {
                switch (c) {
                case '&': out += "&amp;"; break;
                case '<': out += "&lt;"; break;
                case '>': out += "&gt;"; break;
                default:  out += c;
                }
            }
            return out;
        }

        // An entry must not contain the page separator: it would add a page and shift
        // the entry numbering the inter-plugin API relies on.
        std::string NeutralizePageBreaks(std::string text) {
            std::size_t pos = 0;
            while ((pos = text.find("[pagebreak]", pos)) != std::string::npos) {
                text.replace(pos, 11, "[page break]");
                pos += 12;
            }
            return text;
        }

        // An entry's text as the book shows it, before markup escaping.  `stripDates` false
        // (the player wrote it) keeps a leading heading or date: it's theirs, not the LLM's.
        std::string SanitizePlain(const std::string& text, bool stripDates) {
            std::string result = text;

                // Strip leading date headers/prefixes that LLM sometimes includes.
                // e.g. "# Sundas, 17th Last Seed - Late Morning"   (whole-line header, any language)
                //      "Sundas, 17th of Last Seed, 4E 201"         (whole-line English Tamrielic)
                //      "Sundas, 17th Last Seed. Today I saw..."    (inline date prefix in prose)
                // Only strip when ShowDateHeaders is enabled: we're supplying our own formatted headers
                // so LLM-generated ones would duplicate. When ShowDateHeaders is disabled the user
                // wants the LLM's dates to show through — don't strip them.
                if (stripDates && SkyrimNetDiaries::Config::GetSingleton()->GetShowDateHeaders())
                {
                    // Step 1: Strip any leading markdown heading line unconditionally.
                    // A line starting with '#' is always AI formatting noise regardless of language.
                    if (!result.empty() && result[0] == '#') {
                        DropFirstLine(result);
                    }

                    // Step 1.5: Strip a leading line that is entirely bold markdown (**...**).
                    // LLMs sometimes write "**17th of Last Seed, 4E 201 - Morning**" as a header.
                    if (result.size() >= 5 && result[0] == '*' && result[1] == '*') {
                        size_t nl = result.find('\n');
                        size_t lineEnd = (nl != std::string::npos) ? nl : result.size();
                        if (lineEnd >= 5 &&
                            result[lineEnd - 1] == '*' && result[lineEnd - 2] == '*') {
                            DropFirstLine(result);
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
                                        DropFirstLine(result);
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
                            // The whole first line is a date header.
                            DropFirstLine(result);
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

            // Typographic characters the handwriting fonts lack.
            static constexpr std::pair<std::string_view, std::string_view> kPlainForms[] = {
                { "\xE2\x80\x94", "-" },    // em dash
                { "\xE2\x80\x93", "-" },    // en dash
                { "\xE2\x80\x9C", "\"" },   // left double quote
                { "\xE2\x80\x9D", "\"" },   // right double quote
                { "\xE2\x80\x98", "'" },    // left single quote
                { "\xE2\x80\x99", "'" },    // right single quote
                { "\xE2\x80\xA6", "..." },  // ellipsis
            };
            for (const auto& [from, to] : kPlainForms) {
                ReplaceAll(result, from, to);
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

            return NeutralizePageBreaks(std::move(result));
        }

        std::string SanitizeBookText(const DiaryEntry& entry) {
            return EscapeMarkup(SanitizePlain(entry.content, !IsPlayerWritten(entry)));
        }

        struct GameDate {
            int day;        // 1-based
            int month;      // 0 = Morning Star
            int year;       // 4E year
            int dayOfWeek;  // 0 = Sundas
        };

        // Game seconds (entry_date units) → calendar date.  The game starts on Sundas,
        // 17 Last Seed 4E 201, and Skyrim's months have the Gregorian lengths with no
        // leap years.
        GameDate ToGameDate(double gameTime) {
            static constexpr int kMonthDays[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
            const int totalDays = static_cast<int>(std::floor(gameTime / kSecondsPerGameDay));

            GameDate date{ 17 + totalDays, 7, 201, ((totalDays % 7) + 7) % 7 };
            // Before the start date (only test entries): walk back month by month.
            while (date.day <= 0) {
                if (--date.month < 0) { date.month = 11; --date.year; }
                date.day += kMonthDays[date.month];
            }
            while (date.day > kMonthDays[date.month]) {
                date.day -= kMonthDays[date.month];
                if (++date.month > 11) { date.month = 0; ++date.year; }
            }
            return date;
        }

        std::string FormatGameDate(double gameTime) {
            const auto date = ToGameDate(gameTime);
            auto* loc = SkyrimNetDiaries::Localization::GetSingleton();
            return loc->FormatDateLong(loc->GetDayName(date.dayOfWeek).c_str(), date.day,
                                       loc->GetMonthName(date.month).c_str(), date.year);
        }

        // Without the weekday, for the title page.
        std::string FormatGameDateShort(double gameTime) {
            const auto date = ToGameDate(gameTime);
            auto* loc = SkyrimNetDiaries::Localization::GetSingleton();
            return loc->FormatDateShort(date.day, loc->GetMonthName(date.month).c_str(), date.year);
        }

    } // namespace

    std::string EditableEntryText(const DiaryEntry& entry) {
        return SanitizePlain(entry.content, !IsPlayerWritten(entry));
    }

    std::string EntryDate(const DiaryEntry& entry) {
        return FormatGameDate(entry.entry_date);
    }

    std::string EntryHeading(const DiaryEntry& entry) {
        return SkyrimNetDiaries::Config::GetSingleton()->GetShowDateHeaders() ? FormatGameDate(entry.entry_date)
                                                                              : std::string();
    }

    std::string TitlePageDates(const std::vector<DiaryEntry>& entries) {
        if (entries.empty()) return {};
        std::string first = FormatGameDateShort(entries.front().entry_date);
        std::string last = FormatGameDateShort(entries.back().entry_date);
        return first == last ? first : first + " - " + last;
    }

    std::string FormatDiaryEntries(const std::vector<SkyrimNetDiaries::DiaryEntry>& entries,
                                   const std::string& actorName, bool playerDiary) {
        std::string bookText;
        auto config = SkyrimNetDiaries::Config::GetSingleton();
        int fontTitle = config->GetFontSizeTitle();
        int fontDate = config->GetFontSizeDate();
        int fontContent = config->GetFontSizeContent();
        int fontSmall = config->GetFontSizeSmall();
        std::string fontFace = config->GetFontFace();

        // Blank first page
        bookText = std::string(kPageBreak);

        // Title page — handwriting font, centred; leading newlines push it down visually
        bookText += "\n\n\n\n\n\n";
        bookText += "<font face='" + fontFace + "' size='" + std::to_string(fontTitle) + "'><p align='center'>";
        auto* loc = SkyrimNetDiaries::Localization::GetSingleton();
        bookText += loc->FormatDiaryTitle(EscapeMarkup(actorName));
        bookText += "</p></font>\n\n";

        if (entries.empty()) {
            // The player's own diary is just blank (they tore its entries out); the sentinel
            // still tells the inter-plugin API there are no entries.
            bookText += std::string(kPageBreak);
            bookText += "<font face='" + fontFace + "' size='" + std::to_string(fontContent) + "'><p align='center'>"
                      + std::string(SkyrimNetDiaries::Localization::kEmptySentinel)
                      + (playerDiary ? std::string() : loc->GetEmptyVolumeText()) + "</p></font>";
        } else {
            // Date range below title
            bookText += "<font face='" + fontFace + "' size='" + std::to_string(fontSmall) + "'><p align='center'>";
            bookText += TitlePageDates(entries);
            bookText += "</p></font>\n\n";

            // Page break — entries start on the next page
            bookText += std::string(kPageBreak);

            for (size_t i = 0; i < entries.size(); ++i) {
                const auto& entry = entries[i];

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
                std::string content = SanitizeBookText(entry);

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
                    bookText += std::string(kPageBreak);
                }
            }
        }

        return bookText;
    }

} // namespace SkyrimNetDiaries
