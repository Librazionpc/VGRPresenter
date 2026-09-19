#pragma once

// DependencyManager (docs/specs/15 §Dependency Manager): tracks the graph
// Project → Presentation → Slides → Images/Videos/Fonts/Audio and detects
// missing or broken dependencies. Validation reports which referenced assets
// are no longer present in CAMS, so the UI can warn instantly when a file
// disappears (also consumed by the ReferenceManager for cleanup).

#include "interfaces/IService.hpp"

#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace bps::project {

// One edge in the dependency graph: `parent` (e.g. a presentation uuid) needs
// `child` (e.g. an image uuid) of `kind`.
struct DependencyEdge {
    std::string projectId;
    std::string parent;
    std::string child;
    std::string kind;      // "image" | "video" | "audio" | "font" | "presentation" | ...
};

struct DependencyIssue {
    std::string projectId;
    std::string parent;
    std::string child;
    std::string kind;
    std::string reason;    // "missing" | "broken"
};

class DependencyManager {
public:
    static DependencyManager& Instance();

    Result<void> Add(std::string_view projectId, std::string_view parent,
                     std::string_view child, std::string_view kind);
    Result<void> Remove(std::string_view projectId, std::string_view parent,
                        std::string_view child);
    Result<void> ClearProject(std::string_view projectId);

    std::vector<DependencyEdge> Dependencies(std::string_view projectId) const;

    // Validate the project graph against CAMS: returns missing/broken edges.
    std::vector<DependencyIssue> Validate(std::string_view projectId) const;

    // CAMS integration: register all of a project's assetUuids as top-level
    // dependencies of the project itself.
    Result<void> SyncProject(std::string_view projectId,
                             const std::vector<std::string>& assetUuids);

    size_t EdgeCount() const;

private:
    DependencyManager() = default;

    mutable std::mutex mutex_;
    std::vector<DependencyEdge> edges_;
};

} // namespace bps::project
