#pragma once

// PresetStore (Settings · Recording presets persistence): the kernel-side copy of
// the recording / streaming preset roster. A preset is one named, reusable
// encoder + destination configuration — either a RECORDING (container, encoder,
// bitrates, resolution, output folder) or a STREAMING one (platform, RTMP
// server + key, bitrates, resolution, encoder). Settings · Routing wires an
// output to a preset, and that output records / streams with THAT preset's
// configuration. Lives in the kernel's DatabaseManager document store
// ("presets"/"roster" in <dataDir>/kernel.json), the same shape as GroupStore
// and StyleStore.
//
// Storage is a JSON ARRAY OF OBJECTS (stable ids, order = display order):
//   [ { "id": "p1", "name": "Sunday MP4", "kind": "recording",
//       "config": "{\"container\":\"MP4\",...}" } ]
// `config` is the kind's field set as a compact JSON string — opaque here, owned
// by PresetListModel — so adding a field to a preset never touches the schema.
// A missing/corrupt document reads as an empty roster.

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
