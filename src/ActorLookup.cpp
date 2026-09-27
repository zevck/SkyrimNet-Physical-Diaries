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

#include "ActorLookup.h"
#include "Database.h"
#include <mutex>
#include <unordered_map>

namespace SkyrimNetDiaries {

    namespace {
        // Actor lookup cache - persists for entire game session.
        // Keyed by SkyrimNet UUID only: a UUID is unique and stable, so within a
        // session (the cache is wiped on each load) it maps to exactly one actor.
        // FormIDs are deliberately NOT used as a cache key — they are volatile and
        // reused across sessions, so a FormID key could cross-bind two actors that
        // happened to share a stored FormID snapshot.  Cleared each load via
        // ClearActorCache().
        // Stores FormIDs, not Actor pointers: a non-persistent actor can be deleted
        // mid-session (cell reset), and a cached pointer would then dangle.
        static std::mutex g_actorCacheMutex;
        static std::unordered_map<std::string, RE::FormID> g_actorCacheByUuid;
    }

    // ---------------------------------------------------------------------------
    // FindActorForBook: resolve the owning NPC for a diary volume.
    //
    // SkyrimNet's UUID is the only stable, unique identity for an actor; FormIDs
    // are volatile (reused across sessions for non-persistent refs, shifted by
    // ESL load-order changes).  Resolution therefore trusts the UUID and never
    // trusts a bare FormID:
    //   1. UUID → live FormID via SkyrimNet API (authoritative).
    //   2. Stored targetFormID, accepted ONLY if SkyrimNet confirms that FormID
    //      still maps back to our UUID (reverse-lookup back-check).  A reused or
    //      stale FormID fails the check and is rejected rather than resolving to
    //      the wrong actor.
    // A row with no UUID cannot be verified, so it is rejected outright.  The
    // cache is keyed by UUID only and never stores negative results (a transient
    // miss must retry on the next access, not stick for the whole session).
    // ---------------------------------------------------------------------------
    RE::Actor* FindActorForBook(RE::FormID targetFormID,
                                const std::string& actorName,
                                const std::string& bioTemplate,
                                const std::string& actorUuid) {
        if (bioTemplate == "player_special" || targetFormID == 0x14 || actorUuid == "player_special")
            return RE::PlayerCharacter::GetSingleton();

        // No UUID → no way to verify identity.  A bare FormID is volatile and
        // could resolve to the wrong actor, so refuse to guess.
        if (actorUuid.empty()) {
            SKSE::log::warn("[FindActorForBook] '{}' (stored FormID 0x{:X}) has no UUID — cannot verify identity, rejecting",
                            actorName, targetFormID);
            return nullptr;
        }

        // Cache hit by UUID (unique + stable for the session)
        {
            std::lock_guard<std::mutex> lock(g_actorCacheMutex);
            auto it = g_actorCacheByUuid.find(actorUuid);
            if (it != g_actorCacheByUuid.end()) {
                if (auto* cached = RE::TESForm::LookupByID<RE::Actor>(it->second); cached && !cached->IsDeleted()) {
                    return cached;
                }
                g_actorCacheByUuid.erase(it);  // gone since it was cached: resolve again
            }
        }

        RE::Actor* result = nullptr;

        // Tier 1: UUID → live FormID (authoritative).
        uint32_t liveFormId = SkyrimNetDiaries::Database::GetFormIDForUUID(actorUuid);
        if (liveFormId != 0) {
            result = RE::TESForm::LookupByID<RE::Actor>(liveFormId);
            if (result) {
                SKSE::log::debug("[FindActorForBook] Resolved '{}' via UUID {} → live FormID 0x{:X}",
                                 actorName, actorUuid, liveFormId);
            }
        }

        // Tier 2: stored FormID, accepted only if SkyrimNet still maps it back to
        // our UUID.  This is what makes a reused/stale FormID safe — if the slot
        // now belongs to a different actor (or nobody), the back-check fails and
        // we reject rather than hand back the wrong diary owner.
        if (!result && targetFormID != 0) {
            RE::Actor* candidate = RE::TESForm::LookupByID<RE::Actor>(targetFormID);
            if (candidate) {
                std::string ownerUuid = SkyrimNetDiaries::Database::GetUUIDFromFormID(candidate->GetFormID());
                if (ownerUuid == actorUuid) {
                    result = candidate;
                    SKSE::log::debug("[FindActorForBook] Resolved '{}' via stored FormID 0x{:X} (UUID back-check OK)",
                                     actorName, targetFormID);
                } else {
                    SKSE::log::warn("[FindActorForBook] Rejected stored FormID 0x{:X} for '{}': SkyrimNet now maps it to UUID '{}', expected '{}'",
                                    targetFormID, actorName, ownerUuid, actorUuid);
                }
            }
        }

        // Cache only positive results, keyed by UUID.  Never cache a miss.
        if (result) {
            std::lock_guard<std::mutex> lock(g_actorCacheMutex);
            g_actorCacheByUuid[actorUuid] = result->GetFormID();
        }
        return result;
    }

    void ClearActorLookupCache() {
        std::lock_guard<std::mutex> lock(g_actorCacheMutex);
        g_actorCacheByUuid.clear();
    }

    std::int32_t CountInInventory(RE::TESObjectREFR* ref, const RE::TESBoundObject* item) {
        if (!ref || !item) return 0;
        auto inv = ref->GetInventory([item](RE::TESBoundObject& a_obj) { return &a_obj == item; });
        const auto it = inv.find(const_cast<RE::TESBoundObject*>(item));
        return it != inv.end() ? std::max(it->second.first, 0) : 0;
    }

} // namespace SkyrimNetDiaries
