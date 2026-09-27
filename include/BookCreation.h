#pragma once

#include "PCH.h"

// DPF book creation: the serial create queue, DPFCreateCallback and the FormID
// claim table.  BookManager::CreateDiaryBook and SanitizeLoadedBookForms are also
// defined in BookCreation.cpp.  See docs/BOOK_FORMS.md.
namespace SkyrimNetDiaries {

    // Records that formId belongs to actorUuid, so a recycled DPF FormID can't be
    // handed to a second actor.
    void ClaimBookFormId(RE::FormID formId, const std::string& actorUuid);

    // Claims are per-session; cleared from BookManager::ClearActorCache on each load.
    void ClearBookFormIdClaims();

} // namespace SkyrimNetDiaries
