#pragma once

#include "core/config/Json.hpp"

#include <cstddef>
#include <string>

namespace bps::project {
struct ShippedMergeReport {
    bool applied = false;        // false: no shipped file here, or it is already in
    std::string fingerprint;     // "" when nothing was applied
    size_t added = 0;            // shipped entries this install did not have
    size_t refreshed = 0;        // shipped entries whose definition changed
};

ShippedMergeReport ApplyShippedConfiguration();

// The same merge against an explicit file — the seam the tests use.
ShippedMergeReport ApplyShippedConfigurationFrom(const std::string& filePath);

json::Value MergeShippedEntries(const json::Value& stored, const json::Value& shipped,
                                size_t& added, size_t& refreshed);
} // namespace bps::project