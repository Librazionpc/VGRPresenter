#pragma once

#include "core/common/Common.hpp"
#include "core/config/Json.hpp"

#include <string>
#include <vector>

namespace bps::project {
struct StoredPreset {
    std::string id;                        // stable id ("p<n>", never reused)
    std::string name;
    std::string kind = "recording";        // "recording" | "streaming" | "audio"
    std::string configJson = "{}";         // the kind's fields, a JSON object
};

class PresetStore {
public:
    static PresetStore& Instance();

    // The whole roster, display order (empty when nothing was saved yet).
    std::vector<StoredPreset> Get() const;

    // Replaces the roster and persists it. Returns the persistence result so a
    // failed write surfaces (the in-memory store is still updated).
    Result<void> Save(const std::vector<StoredPreset>& presets) const;

private:
    PresetStore() = default;
};
} // namespace bps::project