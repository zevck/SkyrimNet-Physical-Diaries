#include "Localization.h"
#include "Config.h"
#include "SKSE/SKSE.h"

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
        // Build path relative to our DLL: same folder as SkyrimNetPhysicalDiaries.dll
        // DLL is at .../SKSE/Plugins/SkyrimNetPhysicalDiaries.dll
        // Locales at .../SKSE/Plugins/SkyrimNetPhysicalDiaries/Locales/
        std::filesystem::path localeDir;

        HMODULE hModule = nullptr;
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                               GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               reinterpret_cast<LPCSTR>(&DetectSkyrimLanguage),
                               &hModule)) {
            char dllPath[MAX_PATH] = {};
            GetModuleFileNameA(hModule, dllPath, MAX_PATH);
            localeDir = std::filesystem::path(dllPath).parent_path()
                        / "SkyrimNetPhysicalDiaries" / "Locales";
        } else {
            localeDir = std::filesystem::path("Data") / "SKSE" / "Plugins"
                        / "SkyrimNetPhysicalDiaries" / "Locales";
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

        // Map month keys to indices
        static const std::pair<const char*, int> monthKeys[] = {
            {"January", 0}, {"February", 1}, {"March", 2}, {"April", 3},
            {"May", 4}, {"June", 5}, {"July", 6}, {"August", 7},
            {"September", 8}, {"October", 9}, {"November", 10}, {"December", 11}
        };
        // Map day keys to indices
        static const std::pair<const char*, int> dayKeys[] = {
            {"Sunday", 0}, {"Monday", 1}, {"Tuesday", 2}, {"Wednesday", 3},
            {"Thursday", 4}, {"Friday", 5}, {"Saturday", 6}
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
                else if (key == "VolumeSuffix") volumeSuffixFmt_ = value;
                else if (key == "EmptyVolumeText") emptyVolumeText_ = value;
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
        volumeSuffixFmt_ = ", v{n}";
        emptyVolumeText_ = "All entries from this time period have been removed.";

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

    std::string Localization::FormatDiaryTitle(const std::string& actorName) const {
        std::string result;
        result.reserve(diaryTitleFmt_.size() + actorName.size());
        for (size_t i = 0; i < diaryTitleFmt_.size(); ++i) {
            if (diaryTitleFmt_[i] == '{') {
                auto close = diaryTitleFmt_.find('}', i + 1);
                if (close != std::string::npos) {
                    std::string key = diaryTitleFmt_.substr(i + 1, close - i - 1);
                    if (key == "Name") {
                        result += actorName;
                    } else {
                        result += diaryTitleFmt_.substr(i, close - i + 1);
                    }
                    i = close;
                    continue;
                }
            }
            result += diaryTitleFmt_[i];
        }
        return result;
    }

    std::string Localization::FormatVolumeSuffix(int volumeNumber) const {
        if (volumeNumber <= 1) return "";
        // Reuse ApplyTemplate with day=volumeNumber for {n} and {cn}
        return ApplyTemplate(volumeSuffixFmt_, nullptr, volumeNumber, nullptr, 0);
    }

    std::string Localization::FormatBookName(const std::string& actorName, int volumeNumber) const {
        std::string name = FormatDiaryTitle(actorName);
        name += FormatVolumeSuffix(volumeNumber);
        return name;
    }

}  // namespace SkyrimNetDiaries
