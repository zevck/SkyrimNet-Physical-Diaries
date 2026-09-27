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

#include "Serialization.h"
#include "BookManager.h"
#include "DiaryDB.h"
#include "SaveFolder.h"

namespace SkyrimNetDiaries::Serialization {

    namespace {
        constexpr std::uint32_t kSerializationVersion = 3;
        constexpr std::uint32_t kSerializationTypeBooks = 'SNDB'; // SkyrimNet Diary Books
        // Retired 'SNDC' records may still exist in older saves; LoadCallback skips them.
        constexpr std::uint32_t kSerializationTypeFolder = 'SNDF'; // SkyrimNet Diary Folder (save-specific database name)

        void SaveCallback(SKSE::SerializationInterface* a_intfc) {
            // Ensure the DiaryDB is open before writing the sentinel.
            // On a fresh/new game kPostLoadGame never fires, so the DB may not have been
            // opened yet.  Detect the save folder now and flush all in-memory books so
            // nothing is lost when the save is later reloaded.
            auto* db = DiaryDB::GetSingleton();
            if (!db->IsOpen()) {
                if (SaveFolder::Get().empty()) {
                    SaveFolder::DetectFromLog();
                }
                if (!SaveFolder::Get().empty()) {
                    db->Open(SaveFolder::Get());
                }
            }
            if (db->IsOpen()) {
                BookManager::GetSingleton()->FlushToDB();

                // Update last_known_game_time for all actors to capture current game time
                // This is critical for backwards time travel detection when loading older saves
                auto calendar = RE::Calendar::GetSingleton();
                if (calendar) {
                    double currentTime = calendar->GetCurrentGameTime() * 86400.0;
                    auto actorTemplates = db->LoadActorTemplates();
                    int updatedCount = 0;
                    for (const auto& [uuid, templateName] : actorTemplates) {
                        db->UpdateLastKnownGameTime(uuid, currentTime);
                        updatedCount++;
                    }
                    SKSE::log::debug("[SaveCallback] Updated last_known_game_time to {:.2f} for {} actors", currentTime, updatedCount);
                }
            }

            // Save book data
            if (!a_intfc->OpenRecord(kSerializationTypeBooks, kSerializationVersion)) {
                SKSE::log::error("Failed to open book serialization record");
                return;
            }
            BookManager::GetSingleton()->Save(a_intfc);

            // Save current save folder name
            if (!SaveFolder::Get().empty()) {
                if (!a_intfc->OpenRecord(kSerializationTypeFolder, kSerializationVersion)) {
                    SKSE::log::error("Failed to open folder serialization record");
                    return;
                }

                std::uint32_t folderLen = static_cast<std::uint32_t>(SaveFolder::Get().length());
                if (!a_intfc->WriteRecordData(&folderLen, sizeof(folderLen))) {
                    SKSE::log::error("Failed to write folder name length");
                    return;
                }
                if (!a_intfc->WriteRecordData(SaveFolder::Get().c_str(), folderLen)) {
                    SKSE::log::error("Failed to write folder name");
                    return;
                }

                SKSE::log::debug("Saved current save folder: {}", SaveFolder::Get());
            }
        }

        void LoadCallback(SKSE::SerializationInterface* a_intfc) {
            // Clear save folder cache - will be restored from serialized data below (or detected on first diary event)
            SaveFolder::Clear();

            std::uint32_t type;
            std::uint32_t version;
            std::uint32_t length;

            while (a_intfc->GetNextRecordInfo(type, version, length)) {
                if (version > kSerializationVersion) {
                    SKSE::log::error("Serialization version too new for type {}: expected <={}, got {}", type, kSerializationVersion, version);
                    continue;
                }

                if (type == kSerializationTypeBooks) {
                    BookManager::GetSingleton()->Load(a_intfc, version);
                }
                else if (type == kSerializationTypeFolder) {
                    // Load save folder name
                    std::uint32_t folderLen;
                    if (!a_intfc->ReadRecordData(&folderLen, sizeof(folderLen))) {
                        SKSE::log::error("Failed to read folder name length");
                        continue;
                    }

                    std::string folder(folderLen, '\0');
                    if (!a_intfc->ReadRecordData(folder.data(), folderLen)) {
                        SKSE::log::error("Failed to read folder name");
                        continue;
                    }
                    SaveFolder::Set(folder);

                    SKSE::log::debug("Restored save folder from co-save: {}", folder);
                }
            }
        }

        void RevertCallback([[maybe_unused]] SKSE::SerializationInterface* a_intfc) {
            // Called when starting a new game - clear all books and tracking
            BookManager::GetSingleton()->Revert();
            SaveFolder::Clear();
            SKSE::log::info("Reverted all diary data and caches (new game)");
        }

    } // namespace

    void Register() {
        auto serialization = SKSE::GetSerializationInterface();
        serialization->SetUniqueID(kSerializationTypeBooks);
        serialization->SetSaveCallback(SaveCallback);
        serialization->SetLoadCallback(LoadCallback);
        serialization->SetRevertCallback(RevertCallback);
    }

} // namespace SkyrimNetDiaries::Serialization
