#pragma once

#include "PCH.h"

namespace SkyrimNetDiaries {

    // Resolves the NPC that owns a diary volume: the player special case, then
    // SkyrimNet UUID -> live FormID, then the stored FormID only if SkyrimNet maps
    // it back to the same UUID.  Hits are cached by UUID for the session.
    // See docs/BOOK_FORMS.md.
    RE::Actor* FindActorForBook(RE::FormID targetFormID,
                                const std::string& actorName,
                                const std::string& bioTemplate,
                                const std::string& actorUuid = "");

    // Clears the UUID -> actor cache.  Called on each load.
    void ClearActorLookupCache();

} // namespace SkyrimNetDiaries
