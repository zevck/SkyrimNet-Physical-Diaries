#include "SaveFolder.h"
#include <fstream>

namespace SkyrimNetDiaries::SaveFolder {

    namespace {
        // Cache the current save's folder name (e.g., "SkyrimNet-1772379483115-796523")
        std::string g_currentSaveFolder;
    }

    const std::string& Get() { return g_currentSaveFolder; }
    void Set(std::string folder) { g_currentSaveFolder = std::move(folder); }
    void Clear() { g_currentSaveFolder.clear(); }

    // Parse SkyrimNet.log to detect the current save folder (more reliable than filesystem scan).
    // Finds the most recent "Using save ID: " entry, verifies the .db file exists, and caches
    // the result in g_currentSaveFolder. Returns "" on failure.
    std::string DetectFromLog() {
        if (!g_currentSaveFolder.empty()) {
            return g_currentSaveFolder;
        }

        try {
            auto logDir = SKSE::log::log_directory();
            if (!logDir) {
                SKSE::log::warn("DetectSaveFolderFromLog: could not get SKSE log directory");
                return "";
            }

            auto logPath = *logDir / "SkyrimNet.log";
            if (!std::filesystem::exists(logPath)) {
                SKSE::log::warn("DetectSaveFolderFromLog: SkyrimNet.log not found at {}", logPath.string());
                return "";
            }

            std::ifstream file(logPath);
            if (!file.is_open()) {
                SKSE::log::warn("DetectSaveFolderFromLog: could not open {}", logPath.string());
                return "";
            }

            // Scan all lines, keeping the LAST "Using save ID: " occurrence
            static const std::string marker = "Using save ID: ";
            std::string lastSaveId;
            std::string line;
            while (std::getline(file, line)) {
                auto pos = line.find(marker);
                if (pos != std::string::npos) {
                    lastSaveId = line.substr(pos + marker.length());
                    // Trim trailing whitespace / CR
                    while (!lastSaveId.empty() &&
                           (lastSaveId.back() == '\r' || lastSaveId.back() == '\n' || lastSaveId.back() == ' ')) {
                        lastSaveId.pop_back();
                    }
                }
            }

            if (lastSaveId.empty()) {
                SKSE::log::warn("DetectSaveFolderFromLog: no 'Using save ID' line found in log");
                return "";
            }

            // Verify the corresponding .db file actually exists
            std::string folderName = "SkyrimNet-" + lastSaveId;
            auto dbPath = std::filesystem::current_path() / "Data" / "SKSE" / "Plugins" / "SkyrimNet" / "data" / (folderName + ".db");
            if (!std::filesystem::exists(dbPath)) {
                SKSE::log::warn("DetectSaveFolderFromLog: save ID '{}' found in log but DB not found at {}",
                                lastSaveId, dbPath.string());
                return "";
            }

            g_currentSaveFolder = folderName;
            SKSE::log::info("DetectSaveFolderFromLog: detected save folder '{}'", g_currentSaveFolder);
            return g_currentSaveFolder;

        } catch (const std::exception& e) {
            SKSE::log::error("DetectSaveFolderFromLog exception: {}", e.what());
            return "";
        }
    }

} // namespace SkyrimNetDiaries::SaveFolder
