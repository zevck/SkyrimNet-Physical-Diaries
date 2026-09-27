#pragma once

#include "PCH.h"
#include <unordered_set>

// Entry -> volume sync: turns SkyrimNet diary entries into volumes, keeps them
// current, and recovers after loads.  See docs/VOLUMES_AND_SYNC.md.
namespace SkyrimNetDiaries {

    // Brings one actor's volumes up to date with SkyrimNet: first-time creation,
    // updating the open volume, sealing a full one and overflowing into new
    // volumes.  Game thread only.
    void UpdateDiaryForActorInternal(RE::FormID formId);

    // kPostLoadGame (revert + KEEP): queues an update for every actor whose latest
    // volume is missing entries SkyrimNet still has.
    void QueueSealedVolumeRecovery(const std::unordered_set<std::string>& skipUuids = {});

    // kPostLoadGame: pages through all diary entries to find actors that have
    // none of our volumes yet, then creates theirs, one actor per task.
    void QueueBatchCatchUpScan(std::unordered_set<std::string> skipUuids = {});

    // MCM Reset: removes every tracked book from loaded inventories and clears
    // BookManager and DiaryDB tracking (never SkyrimNet's entries).  Returns the
    // number of actors cleared, or -1 on exception.
    int ResetAllDiariesInternal();

} // namespace SkyrimNetDiaries
