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

#pragma once

#include <array>
#include <string>

namespace SkyrimNetDiaries {

    class Localization {
    public:
        static Localization* GetSingleton() {
            static Localization singleton;
            return &singleton;
        }

        // Detect game language and load the locale file.  Call once during plugin load;
        // GMST names are read later by ReadGMSTs().
        void Initialize();

        // Must be called after DataLoaded (GMSTs available). Reads sMonth*/sDay* GMSTs
        // and overwrites month/day arrays unless the locale file provided overrides.
        void ReadGMSTs();

        // Month/day names (populated from locale file, GMSTs, or English fallback)
        const std::string& GetMonthName(int index) const { return monthNames_[index % 12]; }
        const std::string& GetDayName(int index) const { return dayNames_[index % 7]; }

        // Format a full date: "Sundas, 17 Last Seed, 4E 201"
        std::string FormatDateLong(const char* dayName, int day, const char* monthName, int year) const;

        // Format a short date (no weekday): "17 Last Seed, 4E 201"
        std::string FormatDateShort(int day, const char* monthName, int year) const;

        // Format diary title: "Stromm's Diary"
        std::string FormatDiaryTitle(const std::string& actorName) const;

        // Format volume suffix: ", v2" (empty string for volume 1)
        std::string FormatVolumeSuffix(int volumeNumber) const;

        // Convenience: title + volume suffix combined
        std::string FormatBookName(const std::string& actorName, int volumeNumber) const;

        // Empty volume placeholder text
        const std::string& GetEmptyVolumeText() const { return emptyVolumeText_; }

        // Startup warning ([Messages] in the locale file).
        const std::string& GetTemplatesMissingText() const { return templatesMissingText_; }
        // The book editor's prompt on closing with unsaved changes, and its buttons.
        const std::string& GetEditSavePrompt() const { return editSavePrompt_; }
        const std::string& GetEditSave() const { return editSave_; }
        const std::string& GetEditDiscard() const { return editDiscard_; }
        const std::string& GetEditKeepWriting() const { return editKeepWriting_; }
        const std::string& GetEditSaveFailed() const { return editSaveFailed_; }

        // Detected language as uppercase string (e.g. "RUSSIAN")
        const std::string& GetLanguageString() const { return languageString_; }

        // Language-independent sentinel for empty volume detection
        static constexpr const char* kEmptySentinel = "<!-- SNPD_EMPTY -->";

    private:
        Localization() = default;
        ~Localization() = default;
        Localization(const Localization&) = delete;
        Localization& operator=(const Localization&) = delete;

        // Apply template substitution: {Day}, {d}, {Month}, {y}, {Name}, {n}
        std::string ApplyTemplate(const std::string& tmpl,
                                  const char* dayName, int day,
                                  const char* monthName, int year) const;

        // Load locale .ini from Locales/{LANGUAGE}.ini
        bool LoadLocaleFile(const std::string& language);

        std::string languageString_ = "ENGLISH";

        // Calendar data — 12 months, 7 days
        std::array<std::string, 12> monthNames_;
        std::array<std::string, 7> dayNames_;
        bool monthsFromLocaleFile_ = false;  // true = locale file provided months, skip GMST
        bool daysFromLocaleFile_ = false;    // true = locale file provided days, skip GMST

        // Format templates (loaded from locale file or defaults)
        std::string dateLongFmt_;    // e.g. "{Day}, {d} {Month}, 4E {y}"
        std::string dateShortFmt_;   // e.g. "{d} {Month}, 4E {y}"
        std::string diaryTitleFmt_;  // e.g. "{Name}'s Diary"
        std::string volumeSuffixFmt_; // e.g. ", v{n}"
        std::string emptyVolumeText_;
        std::string templatesMissingText_;
        std::string editSavePrompt_;
        std::string editSave_;
        std::string editDiscard_;
        std::string editKeepWriting_;
        std::string editSaveFailed_;
    };

}  // namespace SkyrimNetDiaries
