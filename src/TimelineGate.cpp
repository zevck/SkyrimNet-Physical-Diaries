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

#include "TimelineGate.h"
#include "Database.h"
#include <atomic>

namespace SkyrimNetDiaries::TimelineGate {

    namespace {
        // PublicGetTimelineState values.
        enum class Timeline : int { kNone = 0, kPending = 1, kKept = 2, kCleared = 3 };
        std::atomic<int> g_state{ -1 };  // last value read; -1 = not read this load
    }

    void Reset() {
        g_state.store(-1);
    }

    std::string_view Outcome() {
        switch (static_cast<Timeline>(g_state.load())) {
        case Timeline::kPending: return "unanswered";
        case Timeline::kKept:    return "Keep";
        case Timeline::kCleared: return "Clear";
        default:                 return "no prompt";
        }
    }

    bool IsSettled() {
        // Pending from kPreLoadGame while the prompt is open and while a Clear is still deleting.  No state (an
        // API without it can't get here: InitializeAPI requires it): nothing to wait for.
        const auto state = Database::GetTimelineState().value_or(static_cast<int>(Timeline::kNone));
        if (g_state.exchange(state) != state && state == static_cast<int>(Timeline::kPending)) {
            SKSE::log::info("[TimelineGate] Waiting for SkyrimNet's keep/clear check");
        }
        return state != static_cast<int>(Timeline::kPending);
    }

} // namespace SkyrimNetDiaries::TimelineGate
