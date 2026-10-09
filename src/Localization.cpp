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

#include "Localization.h"
#include "PluginPaths.h"
#include "Config.h"

#include <algorithm>
#include <fstream>
#include <shlobj.h>
#include <cstdio>
#include <filesystem>
#include <sstream>

namespace SkyrimNetDiaries {

    // ── Language detection ─────────────────────────────────────────────
    static std::string DetectSkyrimLanguage() {
        char docsPath[MAX_PATH] = {};
        if (FAILED(SHGetFolderPathA(nullptr, CSIDL_PERSONAL, nullptr,
                                    SHGFP_TYPE_CURRENT, docsPath))) {
            SKSE::log::warn("[Localization] SHGetFolderPath failed; assuming ENGLISH");
            return "ENGLISH";
        }
        std::string iniPath = std::string(docsPath)
                            + "\\My Games\\Skyrim Special Edition\\Skyrim.ini";
        std::ifstream f(iniPath);
        if (!f.is_open()) {
            SKSE::log::warn("[Localization] Could not open '{}'; assuming ENGLISH", iniPath);
            return "ENGLISH";
        }
        std::string line;
        while (std::getline(f, line)) {
            auto ns = line.find_first_not_of(" \t");
            if (ns == std::string::npos) continue;
            line = line.substr(ns);
            if (line.empty() || line[0] == ';' || line[0] == '[') continue;
            if (line.size() >= 9 && line.substr(0, 9) == "sLanguage") {
                auto eq = line.find('=');
                if (eq != std::string::npos) {
                    std::string val = line.substr(eq + 1);
                    while (!val.empty() && (val.back() == ' ' || val.back() == '\r'
                                        || val.back() == '\n' || val.back() == '\t'))
                        val.pop_back();
                    auto vs = val.find_first_not_of(" \t");
                    if (vs != std::string::npos) val = val.substr(vs);
                    std::transform(val.begin(), val.end(), val.begin(), ::toupper);
                    return val;
                }
            }
        }
        return "ENGLISH";
    }

    // ── INI parsing helpers ────────────────────────────────────────────
    static std::string TrimString(const std::string& s) {
        auto start = s.find_first_not_of(" \t\r\n");
        if (start == std::string::npos) return "";
        auto end = s.find_last_not_of(" \t\r\n");
        return s.substr(start, end - start + 1);
    }

    // ── Chinese numeral helper ─────────────────────────────────────────
    static std::string ChineseNumeral(int n) {
        static const char* digits[] = {
            "", "\xe4\xb8\x80", "\xe4\xba\x8c", "\xe4\xb8\x89", "\xe5\x9b\x9b",
            "\xe4\xba\x94", "\xe5\x85\xad", "\xe4\xb8\x83", "\xe5\x85\xab", "\xe4\xb9\x9d"
        };  // 一二三四五六七八九
        static const char* ten = "\xe5\x8d\x81"; // 十

        if (n <= 0) return std::to_string(n);
        if (n < 10) return digits[n];
        if (n == 10) return ten;
        if (n < 20) return std::string(ten) + digits[n - 10];
        if (n < 100) {
            std::string result = std::string(digits[n / 10]) + ten;
            if (n % 10 != 0) result += digits[n % 10];
            return result;
        }
        return std::to_string(n);
    }

    // ── English defaults ───────────────────────────────────────────────
    static const std::array<std::string, 12> kEnglishMonths = {
        "Morning Star", "Sun's Dawn", "First Seed", "Rain's Hand",
        "Second Seed", "Midyear", "Sun's Height", "Last Seed",
        "Hearthfire", "Frostfall", "Sun's Dusk", "Evening Star"
    };
    static const std::array<std::string, 7> kEnglishDays = {
        "Sundas", "Morndas", "Tirdas", "Middas", "Turdas", "Fredas", "Loredas"
    };

    // ── Template substitution ──────────────────────────────────────────
    // Replaces: {Day}, {d}, {Month}, {y}, {Name}, {n}, {cn}
    std::string Localization::ApplyTemplate(const std::string& tmpl,
                                            const char* dayName, int day,
                                            const char* monthName, int year) const {
        std::string result;
        result.reserve(tmpl.size() + 64);
        for (size_t i = 0; i < tmpl.size(); ++i) {
            if (tmpl[i] == '{') {
                auto close = tmpl.find('}', i + 1);
                if (close != std::string::npos) {
                    std::string key = tmpl.substr(i + 1, close - i - 1);
                    if (key == "Day" && dayName) {
                        result += dayName;
                    } else if (key == "d") {
                        result += std::to_string(day);
                    } else if (key == "Month" && monthName) {
                        result += monthName;
                    } else if (key == "y") {
                        result += std::to_string(year);
                    } else if (key == "Name") {
                        // Name is not passed via this function — handled separately
                        result += "{Name}";
                    } else if (key == "n") {
                        result += std::to_string(day);  // reused for volume number
                    } else if (key == "cn") {
                        result += ChineseNumeral(day);  // Chinese numeral for volume
                    } else {
                        result += tmpl.substr(i, close - i + 1); // unknown placeholder
                    }
                    i = close;
                    continue;
                }
            }
            result += tmpl[i];
        }
        return result;
    }

    // ── Locale file loading ────────────────────────────────────────────
    bool Localization::LoadLocaleFile(const std::string& language) {
        // Relative to our DLL (.../SKSE/Plugins/PhysicalDiaries.dll):
        // .../SKSE/Plugins/PhysicalDiaries/Locales/
        std::filesystem::path localeDir;

        HMODULE hModule = nullptr;
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                               GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               reinterpret_cast<LPCSTR>(&DetectSkyrimLanguage),
                               &hModule)) {
            char dllPath[MAX_PATH] = {};
            GetModuleFileNameA(hModule, dllPath, MAX_PATH);
            localeDir = std::filesystem::path(dllPath).parent_path() / PluginPaths::kName / "Locales";
        } else {
            localeDir = PluginPaths::DataDir() / "Locales";
        }

        std::filesystem::path localePath = localeDir / (language + ".ini");
        std::ifstream file(localePath);
        if (!file.is_open()) {
            SKSE::log::info("[Localization] No locale file found at '{}' — using defaults",
                           localePath.string());
            return false;
        }

        SKSE::log::info("[Localization] Loading locale file: '{}'", localePath.string());

        std::string currentSection;
        std::string line;

        // Map month keys to indices (Tamrielic names)
        static const std::pair<const char*, int> monthKeys[] = {
            {"Morning Star", 0}, {"Sun's Dawn", 1}, {"First Seed", 2}, {"Rain's Hand", 3},
            {"Second Seed", 4}, {"Midyear", 5}, {"Sun's Height", 6}, {"Last Seed", 7},
            {"Hearthfire", 8}, {"Frostfall", 9}, {"Sun's Dusk", 10}, {"Evening Star", 11}
        };
        // Map day keys to indices (Tamrielic names)
        static const std::pair<const char*, int> dayKeys[] = {
            {"Sundas", 0}, {"Morndas", 1}, {"Tirdas", 2}, {"Middas", 3},
            {"Turdas", 4}, {"Fredas", 5}, {"Loredas", 6}
        };

        while (std::getline(file, line)) {
            line = TrimString(line);
            if (line.empty() || line[0] == ';' || line[0] == '#') continue;

            if (line[0] == '[' && line.back() == ']') {
                currentSection = line.substr(1, line.size() - 2);
                // Normalize section name to lowercase for comparison
                std::transform(currentSection.begin(), currentSection.end(),
                             currentSection.begin(), ::tolower);
                continue;
            }

            auto eq = line.find('=');
            if (eq == std::string::npos) continue;

            std::string key = TrimString(line.substr(0, eq));
            std::string value = TrimString(line.substr(eq + 1));

            if (currentSection == "format") {
                if (key == "DateLong") dateLongFmt_ = value;
                else if (key == "DateShort") dateShortFmt_ = value;
                else if (key == "DiaryTitle") diaryTitleFmt_ = value;
                else if (key == "JournalTitle") journalTitleFmt_ = value;
                else if (key == "BlankJournal") blankJournalName_ = value;
                else if (key == "VolumeSuffix") volumeSuffixFmt_ = value;
                else if (key == "EmptyVolumeText") emptyVolumeText_ = value;
            } else if (currentSection == "messages") {
                // One line per message; "\n" in the file is a line break.
                std::string text;
                for (std::size_t i = 0; i < value.size(); ++i) {
                    if (value[i] == '\\' && i + 1 < value.size() && value[i + 1] == 'n') {
                        text += '\n';
                        ++i;
                    } else {
                        text += value[i];
                    }
                }
                if (key == "TemplatesMissing") templatesMissingText_ = text;
                else if (key == "OldFiles") oldFilesText_ = text;
                else if (key == "WritingOff") writingOffText_ = text;
                else if (key == "SkyrimNetTooOld") skyrimNetTooOldText_ = text;
                else if (key == "EditSaveFailed") editSaveFailed_ = text;
                else if (key == "EditDeletePrompt") editDeletePrompt_ = text;
                else if (key == "EditDelete") editDelete_ = text;
                else if (key == "EditKeep") editKeep_ = text;
                else if (key == "EditDeleteFailed") editDeleteFailed_ = text;
                else if (key == "EditNeedsSkyrimNet") editNeedsSkyrimNet_ = text;
                else if (key == "EditEmptiedHint") editEmptiedHint_ = text;
                else if (key == "EditJournalFull") editJournalFull_ = text;
                else if (key == "EditStartedVolume") editStartedVolume_ = text;
                else if (key == "JournalRestored") journalRestored_ = text;
                else if (key == "BlankJournalFailed") blankJournalFailed_ = text;
                else if (key == "DailyDiaryOn") dailyDiaryOn_ = text;
                else if (key == "DailyDiaryOff") dailyDiaryOff_ = text;
            } else if (currentSection == "months") {
                for (auto& [mk, idx] : monthKeys) {
                    if (key == mk) {
                        monthNames_[idx] = value;
                        monthsFromLocaleFile_ = true;
                        break;
                    }
                }
            } else if (currentSection == "days") {
                for (auto& [dk, idx] : dayKeys) {
                    if (key == dk) {
                        dayNames_[idx] = value;
                        daysFromLocaleFile_ = true;
                        break;
                    }
                }
            }
        }

        return true;
    }

    // ── GMST reading ───────────────────────────────────────────────────
    void Localization::ReadGMSTs() {
        auto* dh = RE::TESDataHandler::GetSingleton();
        if (!dh) {
            SKSE::log::warn("[Localization] TESDataHandler not available — skipping GMST read");
            return;
        }

        // GMST names for months (map to Tamrielic calendar order)
        static const std::pair<const char*, int> gmstMonths[] = {
            {"sMonthJanuary", 0}, {"sMonthFebruary", 1}, {"sMonthMarch", 2},
            {"sMonthApril", 3}, {"sMonthMay", 4}, {"sMonthJune", 5},
            {"sMonthJuly", 6}, {"sMonthAugust", 7}, {"sMonthSeptember", 8},
            {"sMonthOctober", 9}, {"sMonthNovember", 10}, {"sMonthDecember", 11}
        };
        static const std::pair<const char*, int> gmstDays[] = {
            {"sDaySunday", 0}, {"sDayMonday", 1}, {"sDayTuesday", 2},
            {"sDayWednesday", 3}, {"sDayThursday", 4}, {"sDayFriday", 5},
            {"sDaySaturday", 6}
        };

        if (!monthsFromLocaleFile_) {
            int loaded = 0;
            for (auto& [gmstName, idx] : gmstMonths) {
                auto* setting = RE::GameSettingCollection::GetSingleton();
                if (setting) {
                    auto* gmst = setting->GetSetting(gmstName);
                    if (gmst && gmst->GetType() == RE::Setting::Type::kString) {
                        const char* val = gmst->GetString();
                        if (val && val[0] != '\0') {
                            monthNames_[idx] = val;
                            loaded++;
                        }
                    }
                }
            }
            if (loaded > 0) {
                SKSE::log::info("[Localization] Loaded {}/12 month names from GMSTs", loaded);
                for (int i = 0; i < 12; ++i) {
                    SKSE::log::info("[Localization]   Month {}: '{}'", i, monthNames_[i]);
                }
            }
        } else {
            SKSE::log::info("[Localization] Month names from locale file — skipping GMSTs");
        }

        if (!daysFromLocaleFile_) {
            int loaded = 0;
            for (auto& [gmstName, idx] : gmstDays) {
                auto* setting = RE::GameSettingCollection::GetSingleton();
                if (setting) {
                    auto* gmst = setting->GetSetting(gmstName);
                    if (gmst && gmst->GetType() == RE::Setting::Type::kString) {
                        const char* val = gmst->GetString();
                        if (val && val[0] != '\0') {
                            dayNames_[idx] = val;
                            loaded++;
                        }
                    }
                }
            }
            if (loaded > 0) {
                SKSE::log::info("[Localization] Loaded {}/7 day names from GMSTs", loaded);
            }
        } else {
            SKSE::log::info("[Localization] Day names from locale file — skipping GMSTs");
        }
    }

    // ── Initialize ─────────────────────────────────────────────────────
    void Localization::Initialize() {
        // Check for INI override first, fall back to Skyrim.ini detection
        std::string override = Config::GetSingleton()->GetLanguageOverride();
        if (!override.empty()) {
            std::transform(override.begin(), override.end(), override.begin(), ::toupper);
            languageString_ = override;
            SKSE::log::info("[Localization] Language override from INI: '{}'", languageString_);
        } else {
            languageString_ = DetectSkyrimLanguage();
        }

        // Start with English defaults
        monthNames_ = kEnglishMonths;
        dayNames_ = kEnglishDays;

        // English format defaults
        dateLongFmt_ = "{Day}, {d} {Month}, 4E {y}";
        dateShortFmt_ = "{d} {Month}, 4E {y}";
        diaryTitleFmt_ = "{Name}'s Diary";
        journalTitleFmt_ = "{Name}'s Journal";
        blankJournalName_ = "Blank Journal";
        volumeSuffixFmt_ = ", v{n}";
        emptyVolumeText_ = "All entries from this time period have been removed.";
        templatesMissingText_ =
            "Physical Diaries\n\nThe diary template books were not found.\n\n"
            "Diaries can't be created. Check that:\n"
            " - Physical Diaries.esp is enabled\n"
            " - powerofthree's Tweaks or Native EditorID Fix is installed for your game version\n\n"
            "See PhysicalDiaries.log for details.";
        oldFilesText_ =
            "Physical Diaries\n\nFiles from version 1 are still installed (SkyrimNet Physical Diaries.esp or "
            "SkyrimNetPhysicalDiaries.dll).\n\nVersion 2 renamed them to Physical Diaries.esp and PhysicalDiaries.dll. "
            "Remove the old files before continuing.";
        writingOffText_ =
            "Ink & Quill - Writing Framework isn't installed, or its writing is off (see InkAndQuill.log). Your journals "
            "are read-only until it's back.";
        skyrimNetTooOldText_ =
            "Physical Diaries needs a newer version of SkyrimNet. Diaries and journals are disabled until SkyrimNet "
            "is updated.";
        editSaveFailed_ = "Your journal couldn't be saved. See PhysicalDiaries.log.";
        editDeletePrompt_ = "Tear out the entry from {Date}?\n\nIt will be gone from your journal and from your memory.";
        editDelete_ = "Tear out";
        editKeep_ = "Keep it";
        editDeleteFailed_ = "The entry couldn't be torn out. See PhysicalDiaries.log.";
        editNeedsSkyrimNet_ = "Writing in your journal needs a newer SkyrimNet.";
        editEmptiedHint_ = "An emptied entry is kept. To remove an entry, tear it out.";
        editJournalFull_ = "Your journal is full. Read a blank journal to begin another.";
        editStartedVolume_ = "You begin a new journal.";
        journalRestored_ = "You still have your journal.";
        blankJournalFailed_ = "The journal couldn't be started. See PhysicalDiaries.log.";
        dailyDiaryOn_ = "{Name} will write in their diary every day.";
        dailyDiaryOff_ = "{Name} will no longer write in their diary every day.";

        // Load locale file (may override formats, months, days)
        LoadLocaleFile(languageString_);

        // GMSTs are not available yet at plugin load — ReadGMSTs() is called
        // later from the DataLoaded event handler.

        SKSE::log::info("[Localization] Initialized for language '{}' (GMST read deferred to DataLoaded)",
                       languageString_);
    }

    // ── Format functions ───────────────────────────────────────────────
    std::string Localization::FormatDateLong(const char* dayName, int day,
                                            const char* monthName, int year) const {
        return ApplyTemplate(dateLongFmt_, dayName, day, monthName, year);
    }

    std::string Localization::FormatDateShort(int day, const char* monthName, int year) const {
        return ApplyTemplate(dateShortFmt_, nullptr, day, monthName, year);
    }

    std::string Localization::FormatTitle(const std::string& actorName, VolumeKind kind) const {
        const std::string& fmt = kind == VolumeKind::Written ? journalTitleFmt_ : diaryTitleFmt_;
        std::string result;
        result.reserve(fmt.size() + actorName.size());
        for (size_t i = 0; i < fmt.size(); ++i) {
            if (fmt[i] == '{') {
                auto close = fmt.find('}', i + 1);
                if (close != std::string::npos) {
                    std::string key = fmt.substr(i + 1, close - i - 1);
                    if (key == "Name") {
                        result += actorName;
                    } else {
                        result += fmt.substr(i, close - i + 1);
                    }
                    i = close;
                    continue;
                }
            }
            result += fmt[i];
        }
        return result;
    }

    std::string Localization::FormatVolumeSuffix(int volumeNumber) const {
        if (volumeNumber <= 1) return "";
        // Reuse ApplyTemplate with day=volumeNumber for {n} and {cn}
        return ApplyTemplate(volumeSuffixFmt_, nullptr, volumeNumber, nullptr, 0);
    }

    std::string Localization::FormatBookName(const std::string& actorName, int volumeNumber, VolumeKind kind) const {
        std::string name = FormatTitle(actorName, kind);
        name += FormatVolumeSuffix(volumeNumber);
        return name;
    }

}  // namespace SkyrimNetDiaries
