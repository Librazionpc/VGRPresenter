#pragma once

// Project model (docs/specs/15 §Project Manager). A project is the unit of
// user work: it owns documents, references assets through CAMS, and carries
// project settings + versioning. The model is pure data (no managers) so it
// can be serialized independently.

#include "core/common/Common.hpp"
#include "core/config/Json.hpp"

#include <string>
#include <vector>

namespace bps::project {

struct ProjectSettings {
    std::string theme;
    std::string defaultTemplateId;
    std::string aspectRatio = "16:9";       // "16:9" / "4:3" / "custom"
    int autosaveIntervalSec = 60;
    bool compressAssets = false;
    bool generateThumbnails = true;
    json::Value extra;                       // module-specific settings
};

// One project (docs/specs/15). `path` is the host path of the project file
// (empty while unsaved); `assetUuids` lists the CAMS assets the project uses.
struct Project {
    std::string id;                          // UUID
    std::string name;
    std::string path;                        // host path of the .bpsproj file
    std::string description;
    Version schema{1, 0, 0, ""};
    bool archived = false;
    int64_t createdAtMs = 0;
    int64_t modifiedAtMs = 0;
    ProjectSettings settings;
    std::vector<std::string> openDocuments;  // document ids currently open
    std::vector<std::string> assetUuids;     // CAMS asset references
};

// --- Serialization (JSON documents persisted via the DatabaseManager) -------
json::Value ProjectToJson(const Project& p);
Result<Project> ProjectFromJson(const json::Value& v);

} // namespace bps::project
