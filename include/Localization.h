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
#include "Database.h"  // VolumeKind

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

        // A volume's title by kind: "Stromm's Diary", "Prisoner's Journal"
        std::string FormatTitle(const std::string& actorName, VolumeKind kind) const;

        // Format volume suffix: ", v2" (empty string for volume 1)
        std::string FormatVolumeSuffix(int volumeNumber) const;

        // Convenience: title + volume suffix combined
        std::string FormatBookName(const std::string& actorName, int volumeNumber, VolumeKind kind) const;

        // The blank journal item's name ("Blank Journal")
        const std::string& GetBlankJournalName() const { return blankJournalName_; }

        // Empty volume placeholder text
        const std::string& GetEmptyVolumeText() const { return emptyVolumeText_; }

        // Startup warning ([Messages] in the locale file).
        const std::string& GetTemplatesMissingText() const { return templatesMissingText_; }
        // Startup: 1.x's ESP or DLL is still installed (docs/ARCHITECTURE.md#names).
        const std::string& GetOldFilesText() const { return oldFilesText_; }
        // After a load: writing is off, but the save has journals.
        const std::string& GetWritingOffText() const { return writingOffText_; }
        // Startup: SkyrimNet is older than the public API SNPD needs.
        const std::string& GetSkyrimNetTooOldText() const { return skyrimNetTooOldText_; }
        // SkyrimNet didn't save the journal.
        const std::string& GetEditSaveFailed() const { return editSaveFailed_; }
        // Tearing out an entry: the question ({Date} = the entry's date) and its buttons, and the
        // notification when SkyrimNet doesn't delete it.
        const std::string& GetEditDeletePrompt() const { return editDeletePrompt_; }
        const std::string& GetEditDelete() const { return editDelete_; }
        const std::string& GetEditKeep() const { return editKeep_; }
        const std::string& GetEditDeleteFailed() const { return editDeleteFailed_; }
        // Why writing can't start (SkyrimNet too old),
        // and the hint when a save leaves an emptied entry as it was.
        const std::string& GetEditNeedsSkyrimNet() const { return editNeedsSkyrimNet_; }
        const std::string& GetEditEmptiedHint() const { return editEmptiedHint_; }
        // A journal holds EntriesPerVolume entries: no new one in a full journal.
        const std::string& GetEditJournalFull() const { return editJournalFull_; }
        // A blank journal read from the inventory became one of the player's journals.
        const std::string& GetEditStartedVolume() const { return editStartedVolume_; }
        // After a load: a journal made after the save, whose entries SkyrimNet kept, is back with the player.
        const std::string& GetJournalRestored() const { return journalRestored_; }
        const std::string& GetBlankJournalFailed() const { return blankJournalFailed_; }
        // An NPC starts or stops writing every day; {Name} is theirs.
        const std::string& GetDailyDiaryOn() const { return dailyDiaryOn_; }
        const std::string& GetDailyDiaryOff() const { return dailyDiaryOff_; }

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
        std::string blankJournalName_;  // the blank journal item's name
        std::string dailyDiaryOn_;
        std::string dailyDiaryOff_;
        std::string journalTitleFmt_;  // e.g. "{Name}'s Journal": the player's written volumes
        std::string volumeSuffixFmt_; // e.g. ", v{n}"
        std::string emptyVolumeText_;
        std::string templatesMissingText_;
        std::string oldFilesText_;
        std::string writingOffText_;
        std::string skyrimNetTooOldText_;
        std::string editSaveFailed_;
        std::string editDeletePrompt_;
        std::string editDelete_;
        std::string editKeep_;
        std::string editDeleteFailed_;
        std::string editNeedsSkyrimNet_;
        std::string editEmptiedHint_;
        std::string editJournalFull_;
        std::string editStartedVolume_;
        std::string journalRestored_;
        std::string blankJournalFailed_;
    };

}  // namespace SkyrimNetDiaries
