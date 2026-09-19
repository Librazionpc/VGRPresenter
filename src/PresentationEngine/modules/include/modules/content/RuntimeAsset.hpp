#pragma once

// RuntimeAsset: the in-memory representation of a loaded asset (docs/specs/13
// §Asset Manager). Bytes live here once loaded; reference counting + pinning
// are tracked by the AssetRegistry.

#include "modules/content/AssetMetadata.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace bps::content {

struct RuntimeAsset {
    AssetMetadata meta;
    std::vector<uint8_t> bytes;
    std::string text;                    // decoded text form (Text/Json/Data assets)
    std::string thumbnail;               // small/thumb bytes (best-effort)
    int64_t loadedAtMs = 0;
    int refCount = 0;
    bool pinned = false;
    bool dirty = false;                  // modified in memory, not yet saved
};

} // namespace bps::content
