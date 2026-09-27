#pragma once

#include "PCH.h"

// Answers the SNPD_QUERY_* SKSE messages other plugins send to read diary text.
// Message layouts: include/SkyrimNetPhysicalDiariesAPI.h.  See docs/PAPYRUS_AND_API.md.
namespace SkyrimNetDiaries::InterPluginAPI {

    // Returns true if msg was an SNPD query (answered in place), false otherwise.
    bool HandleMessage(SKSE::MessagingInterface::Message* msg);

} // namespace SkyrimNetDiaries::InterPluginAPI
