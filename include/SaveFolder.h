#pragma once

#include "PCH.h"

// The loaded character's SkyrimNet save folder ("SkyrimNet-<id>").  DiaryDB is
// keyed by it.  See docs/DATABASE.md.
namespace SkyrimNetDiaries::SaveFolder {

    const std::string& Get();
    void Set(std::string folder);
    void Clear();

    // Parses SkyrimNet.log for the last "Using save ID: " line, checks that the
    // matching SkyrimNet .db exists, and caches the result.  Returns the cached
    // value if one is already set, "" on failure.
    std::string DetectFromLog();

} // namespace SkyrimNetDiaries::SaveFolder
