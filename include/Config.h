#pragma once

#include "PCH.h"
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

                SKSE::log::info("Loaded config from: {}", iniPath.string());
                LogSettings();
                return true;

            } catch (const std::exception& e) {
                SKSE::log::error("Exception loading config: {}", e.what());
                return false;
            }
        }

        // Get integer value
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

        // Get string value
        std::string GetString(const std::string& section, const std::string& key, const std::string& defaultValue) const {
            std::string fullKey = section + "." + key;
            auto it = settings_.find(fullKey);
            return (it != settings_.end()) ? it->second : defaultValue;
        }

        // Convenience getters for diary-specific settings
        int GetEntriesPerVolume() const { return GetInt("Diary", "EntriesPerVolume", 10); }
        int GetFontSizeTitle() const { return GetInt("Fonts", "TitleSize", 18); }
        int GetFontSizeDate() const { return GetInt("Fonts", "DateSize", 16); }
        int GetFontSizeContent() const { return GetInt("Fonts", "ContentSize", 14); }
        int GetFontSizeSmall() const { return GetInt("Fonts", "SmallSize", 12); }

    private:
        Config() = default;
        ~Config() = default;
        Config(const Config&) = delete;
        Config& operator=(const Config&) = delete;

        void LogSettings() const {
            SKSE::log::info("Config Settings:");
            SKSE::log::info("  EntriesPerVolume: {}", GetEntriesPerVolume());
            SKSE::log::info("  FontSizeTitle: {}", GetFontSizeTitle());
            SKSE::log::info("  FontSizeDate: {}", GetFontSizeDate());
            SKSE::log::info("  FontSizeContent: {}", GetFontSizeContent());
            SKSE::log::info("  FontSizeSmall: {}", GetFontSizeSmall());
        }

        std::unordered_map<std::string, std::string> settings_;
    };

}  // namespace SkyrimNetDiaries
