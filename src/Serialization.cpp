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
#include "BookCreation.h"
#include "BookManager.h"
#include "DiaryDB.h"
#include "DynamicForms.h"
#include "NpcDiaries.h"
#include "SaveFolder.h"
#include "VolumeSync.h"

namespace SkyrimNetDiaries::Serialization {

    namespace {
        // One record: what each book form in the save is (DynamicForms).  Older saves' records are skipped
        // (docs/DATABASE.md#co-save-records).
        constexpr std::uint32_t kSerializationId = 'SNDB';
        constexpr std::uint32_t kBookFormsRecord = 'SNBF';
        constexpr std::uint32_t kNpcDiariesRecord = 'SNND';  // the game day NPC diaries last ran

        void SaveCallback(SKSE::SerializationInterface* a_intfc) {
            try {
                // First, so a DiaryDB failure below can't lose it: without its record a
                // book in the save loads as an empty shell.
                DynamicForms::Save(a_intfc, kBookFormsRecord);
                NpcDiaries::Save(a_intfc, kNpcDiariesRecord);

                // A new game's first save opens DiaryDB here (no post-load sync).  Not during a load's post-load wait:
                // SkyrimNet.log may still name the previous save's folder then.
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
            DynamicForms::Revert();
            NpcDiaries::Revert();
            SaveFolder::Clear();
            SKSE::log::info("Reverted all diary data and caches");
        }

        // Runs inside the load, after the engine has recreated this save's book forms and resolved inventories:
        // fills the books in at once; the post-load sync matches them against DiaryDB later.
        void LoadCallback(SKSE::SerializationInterface* a_intfc) {
            try {
                std::uint32_t type = 0, version = 0, length = 0;
                while (a_intfc->GetNextRecordInfo(type, version, length)) {
                    if (type == kBookFormsRecord) DynamicForms::Load(a_intfc, version);
                    else if (type == kNpcDiariesRecord) NpcDiaries::Load(a_intfc, version);
                }
                ConfigureLoadedBooks();
            } catch (const std::exception& e) {
                SKSE::log::error("LoadCallback exception: {}", e.what());
            } catch (...) {
                SKSE::log::error("LoadCallback: unknown exception");
            }
        }

    } // namespace

    void Register() {
        auto serialization = SKSE::GetSerializationInterface();
        serialization->SetUniqueID(kSerializationId);
        serialization->SetSaveCallback(SaveCallback);
        serialization->SetRevertCallback(RevertCallback);
        serialization->SetLoadCallback(LoadCallback);
    }

} // namespace SkyrimNetDiaries::Serialization
