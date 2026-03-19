#pragma once

#include <array>
#include <string>
#include <functional>

namespace SkyrimNetDiaries {

    enum class Language {
        English,
        French,
        German,
        Italian,
        Spanish,
        Polish,
        Russian,
        ChineseTraditional,
        Japanese
    };

    struct LocaleData {
        // Tamrielic month names (12 months, indexed 0-11)
        std::array<const char*, 12> monthNames;

        // Tamrielic day names (7 days, indexed 0-6: Sundas=0 .. Loredas=6)
        std::array<const char*, 7> dayNames;

        // Format a full date: "Sundas, 17 Last Seed, 4E 201"
        std::function<std::string(const char* dayName, int day, const char* monthName, int year)> formatDateLong;

        // Format a short date (no weekday): "17 Last Seed, 4E 201"
        std::function<std::string(int day, const char* monthName, int year)> formatDateShort;

        // Format diary title: "Stromm's Diary"
        std::function<std::string(const std::string& actorName)> formatDiaryTitle;

        // Format volume suffix: ", v2"  (returns empty string for volume 1)
        std::function<std::string(int volumeNumber)> formatVolumeSuffix;

        // "All entries from this time period have been removed."
        const char* emptyVolumeText;
    };

    class Localization {
    public:
        static Localization* GetSingleton() {
            static Localization singleton;
            return &singleton;
        }

        // Detect game language and select locale. Call once during plugin load.
        void Initialize();

        // Active locale data
        const LocaleData& Get() const { return *activeLocale_; }

        // Detected language enum
        Language GetLanguage() const { return language_; }

        // Detected language as uppercase string (e.g. "RUSSIAN")
        const std::string& GetLanguageString() const { return languageString_; }

        // Convenience: format full book name (title + volume suffix)
        std::string FormatBookName(const std::string& actorName, int volumeNumber) const;

        // Language-independent sentinel prepended to empty volume text.
        // Used for detection in API queries so we don't need to match translated strings.
        static constexpr const char* kEmptySentinel = "<!-- SNPD_EMPTY -->";

    private:
        Localization() = default;
        ~Localization() = default;
        Localization(const Localization&) = delete;
        Localization& operator=(const Localization&) = delete;

        Language language_ = Language::English;
        std::string languageString_ = "ENGLISH";
        const LocaleData* activeLocale_ = nullptr;
    };

}  // namespace SkyrimNetDiaries
