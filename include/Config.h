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

#include "PCH.h"
#include <algorithm>
#include <fstream>
#include <sstream>
#include <string>
#include <unordered_map>

namespace SkyrimNetDiaries {

    class Config {
    public:
        static Config* GetSingleton() {
            static Config singleton;
            return &singleton;
        }

        // Load configuration from INI file
        bool Load(const std::filesystem::path& iniPath) {
            iniPath_ = iniPath;
            try {
                if (!std::filesystem::exists(iniPath)) {
                    SKSE::log::warn("Config file not found: {} - using defaults", iniPath.string());
                    return false;
                }

                std::ifstream file(iniPath);
                if (!file.is_open()) {
                    SKSE::log::error("Failed to open config file: {}", iniPath.string());
                    return false;
                }

                std::string currentSection;
                std::string line;
                while (std::getline(file, line)) {
                    // Trim whitespace
                    line.erase(0, line.find_first_not_of(" \t\r\n"));
                    line.erase(line.find_last_not_of(" \t\r\n") + 1);

                    // Skip empty lines and comments
                    if (line.empty() || line[0] == ';' || line[0] == '#') {
                        continue;
                    }

                    // Section header
                    if (line[0] == '[' && line.back() == ']') {
                        currentSection = line.substr(1, line.length() - 2);
                        continue;
                    }

                    // Key=Value pair
                    size_t equalsPos = line.find('=');
                    if (equalsPos != std::string::npos) {
                        std::string key = line.substr(0, equalsPos);
                        std::string value = line.substr(equalsPos + 1);
                        
                        // Trim key and value
                        key.erase(0, key.find_first_not_of(" \t"));
                        key.erase(key.find_last_not_of(" \t") + 1);
                        value.erase(0, value.find_first_not_of(" \t"));
                        value.erase(value.find_last_not_of(" \t") + 1);

                        // Remove inline comments from value
                        size_t commentPos = value.find(';');
                        if (commentPos != std::string::npos) {
                            value = value.substr(0, commentPos);
                            value.erase(value.find_last_not_of(" \t") + 1);
                        }

                        std::string fullKey = currentSection + "." + key;
                        settings_[fullKey] = value;
                    }
                }

                // Replace invalid or out-of-range values with what Get() would return,
                // so the warning is logged once here instead of on every read.
                for (const auto& s : kIntSettings) {
                    Set(s, Get(s));
                }

                SKSE::log::info("Loaded config from: {}", iniPath.string());
                LogSettings();
                return true;

            } catch (const std::exception& e) {
                SKSE::log::error("Exception loading config: {}", e.what());
                return false;
            }
        }

        // Every integer setting: INI section and key, default, and the range the value
        // is clamped to.  Clamping happens on read, so a hand-edited INI can't feed
        // EntriesPerVolume = 0 (an endless chunking loop) or negative sizes to the code.
        struct IntSetting { const char* section; const char* key; int defaultValue; int min; int max; };
        static constexpr IntSetting kDebugLog         { "General", "DebugLog",         0,  0, 1  };
        static constexpr IntSetting kShowDateHeaders  { "Diary",   "ShowDateHeaders",  1,  0, 1  };
        static constexpr IntSetting kEntriesPerVolume { "Diary",   "EntriesPerVolume", 10, 1, 50 };
        static constexpr IntSetting kFontSizeTitle    { "Fonts",   "TitleSize",        18, 8, 24 };
        static constexpr IntSetting kFontSizeDate     { "Fonts",   "DateSize",         16, 8, 24 };
        static constexpr IntSetting kFontSizeContent  { "Fonts",   "ContentSize",      14, 8, 24 };
        static constexpr IntSetting kFontSizeSmall    { "Fonts",   "SmallSize",        12, 8, 24 };
        // INI order.  Language (string) is written first in [General], FontFace last in [Fonts].
        static constexpr IntSetting kIntSettings[] = {
            kDebugLog, kShowDateHeaders, kEntriesPerVolume,
            kFontSizeTitle, kFontSizeDate, kFontSizeContent, kFontSizeSmall,
        };

        int Get(const IntSetting& s) const {
            return std::clamp(GetInt(s.section, s.key, s.defaultValue), s.min, s.max);
        }
        void Set(const IntSetting& s, int value) {
            settings_[std::string(s.section) + "." + s.key] = std::to_string(std::clamp(value, s.min, s.max));
        }

        // Convenience getters for diary-specific settings
        std::string GetLanguageOverride() const { return GetString("General", "Language", ""); }
        bool GetDebugLog() const { return Get(kDebugLog) != 0; }
        bool GetShowDateHeaders() const { return Get(kShowDateHeaders) != 0; }
        int GetEntriesPerVolume() const { return Get(kEntriesPerVolume); }
        int GetFontSizeTitle() const { return Get(kFontSizeTitle); }
        int GetFontSizeDate() const { return Get(kFontSizeDate); }
        int GetFontSizeContent() const { return Get(kFontSizeContent); }
        int GetFontSizeSmall() const { return Get(kFontSizeSmall); }
        std::string GetFontFace() const { return GetString("Fonts", "FontFace", "$HandwrittenFont"); }

        // Convenience setters (in memory until Save())
        void SetDebugLog(bool v) { Set(kDebugLog, v ? 1 : 0); }
        void SetShowDateHeaders(bool v) { Set(kShowDateHeaders, v ? 1 : 0); }
        void SetEntriesPerVolume(int v) { Set(kEntriesPerVolume, v); }
        void SetFontSizeTitle(int v)    { Set(kFontSizeTitle, v); }
        void SetFontSizeDate(int v)     { Set(kFontSizeDate, v); }
        void SetFontSizeContent(int v)  { Set(kFontSizeContent, v); }
        void SetFontSizeSmall(int v)    { Set(kFontSizeSmall, v); }
        void SetFontFace(const std::string& v) { settings_["Fonts.FontFace"] = v; }

        // Persist current settings back to the INI file loaded via Load()
        bool Save() const {
            if (iniPath_.empty()) {
                SKSE::log::error("Config::Save() called before Load() - iniPath unknown");
                return false;
            }
            return SaveToPath(iniPath_);
        }

        bool SaveToPath(const std::filesystem::path& path) const {
            try {
                // Build the whole file first, so a failure can't leave it half-written.
                std::ostringstream out;
                std::string currentSection;
                for (const auto& s : kIntSettings) {
                    if (s.section != currentSection) {
                        if (!currentSection.empty()) out << "\n";
                        out << "[" << s.section << "]\n";
                        currentSection = s.section;
                        if (currentSection == "General") {
                            const std::string lang = GetLanguageOverride();
                            if (!lang.empty()) out << "Language = " << lang << "\n";
                        }
                    }
                    out << s.key << " = " << Get(s) << "\n";
                }
                out << "FontFace = " << GetFontFace() << "\n";  // [Fonts] is the last section

                std::filesystem::create_directories(path.parent_path());
                std::ofstream file(path, std::ios::trunc);
                if (!file.is_open()) {
                    SKSE::log::error("Config::Save() - failed to open: {}", path.string());
                    return false;
                }
                file << out.str();

                SKSE::log::info("Config saved to: {}", path.string());
                return true;

            } catch (const std::exception& e) {
                SKSE::log::error("Exception saving config: {}", e.what());
                return false;
            }
        }

    private:
        Config() = default;
        ~Config() = default;
        Config(const Config&) = delete;
        Config& operator=(const Config&) = delete;

        // Raw INI value; Get() clamps it.
        int GetInt(const std::string& section, const std::string& key, int defaultValue) const {
            std::string fullKey = section + "." + key;
            auto it = settings_.find(fullKey);
            if (it != settings_.end()) {
                try {
                    return std::stoi(it->second);
                } catch (...) {
                    SKSE::log::warn("Invalid integer value for {}: '{}' - using default {}",
                                  fullKey, it->second, defaultValue);
                }
            }
            return defaultValue;
        }

        std::string GetString(const std::string& section, const std::string& key, const std::string& defaultValue) const {
            std::string fullKey = section + "." + key;
            auto it = settings_.find(fullKey);
            return (it != settings_.end()) ? it->second : defaultValue;
        }

        void LogSettings() const {
            SKSE::log::info("Config Settings:");
            for (const auto& s : kIntSettings) {
                SKSE::log::info("  {}.{}: {}", s.section, s.key, Get(s));
            }
        }

        std::unordered_map<std::string, std::string> settings_;
        std::filesystem::path iniPath_;
    };

}  // namespace SkyrimNetDiaries
