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
#include "VolumeSync.h"

namespace SkyrimNetDiaries::Serialization {

    namespace {
        // The co-save carries no data: DiaryDB is the source of truth, and the save
        // folder is detected from SkyrimNet.log.  The callbacks are still needed for
        // their timing.  Older saves hold 'SNDB', 'SNDF' and 'SNDC' records; with no
        // load callback SKSE skips them.
        constexpr std::uint32_t kSerializationId = 'SNDB';

        void SaveCallback([[maybe_unused]] SKSE::SerializationInterface* a_intfc) {
            try {
                // A new game never gets a post-load sync, so its first save opens
                // DiaryDB here and flushes the volumes created so far.  Not during a
                // load's post-load wait: SkyrimNet.log may still name the previous
                // save's folder then, and the sync opens the right DB itself.
                auto* db = DiaryDB::GetSingleton();
                if (!db->IsOpen() && IsPostLoadSyncReady()) {
                    if (SaveFolder::Get().empty()) {
                        SaveFolder::DetectFromLog();
                    }
                    if (!SaveFolder::Get().empty()) {
                        db->Open(SaveFolder::Get());
                    }
                }
                if (db->IsOpen()) {
                    BookManager::GetSingleton()->FlushToDB();
                }
            } catch (const std::exception& e) {
                SKSE::log::error("SaveCallback exception: {}", e.what());
            } catch (...) {
                SKSE::log::error("SaveCallback: unknown exception");
            }
        }

        void RevertCallback([[maybe_unused]] SKSE::SerializationInterface* a_intfc) {
            // A new game or a load: clear in-memory volumes (DiaryDB on disk stays).
            BookManager::GetSingleton()->Revert();
            SaveFolder::Clear();
            SKSE::log::info("Reverted all diary data and caches");
        }

    } // namespace

    void Register() {
        auto serialization = SKSE::GetSerializationInterface();
        serialization->SetUniqueID(kSerializationId);
        serialization->SetSaveCallback(SaveCallback);
        serialization->SetRevertCallback(RevertCallback);
    }

} // namespace SkyrimNetDiaries::Serialization
