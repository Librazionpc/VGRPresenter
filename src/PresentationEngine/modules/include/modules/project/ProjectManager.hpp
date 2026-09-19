#pragma once

// ProjectManager (docs/specs/15 §Project Manager): the workflow owner for
// projects — create, open, save, save-as, close, rename, duplicate, delete,
// archive. Multiple projects can be open; one is active. Metadata persists via
// the DatabaseManager (collection "projects"); the project file itself is
// written through the PAL filesystem (never std::filesystem).

#include "modules/project/Project.hpp"
#include "modules/project/ProjectRegistry.hpp"
#include "interfaces/IService.hpp"

#include <atomic>
#include <string>
#include <vector>

namespace bps::project {

class ProjectManager {
public:
    static ProjectManager& Instance();

    // --- Workflow ---
    Result<Project> Create(std::string_view name, std::string_view templateId = {});
    Result<Project> Open(std::string_view hostPath);
    Result<void> Save(std::string_view id, bool autosave = false);
    Result<Project> SaveAs(std::string_view id, std::string_view hostPath);
    Result<void> Close(std::string_view id);
    Result<Project> Rename(std::string_view id, std::string_view newName);
    Result<Project> Duplicate(std::string_view id);
    Result<void> Delete(std::string_view id);
    Result<void> Archive(std::string_view id, bool archived);
    // Update the project's CAMS asset references (registry + DB + file).
    Result<void> SetAssetReferences(std::string_view id,
                                    const std::vector<std::string>& uuids);

    // --- Queries ---
    std::vector<Project> OpenProjects() const;
    Result<Project> Active() const;
    Result<void> SetActive(std::string_view id);
    Result<Project> Get(std::string_view id) const;

    // --- Persistence ---
    // Restore previously open projects from the database (crash/restart).
    Result<void> LoadAll();
    Result<void> AutosaveAll();

    void SetDataDir(std::string_view dir);
    const std::string& DataDir() const noexcept { return dataDir_; }

private:
    ProjectManager() = default;
    Result<void> Persist(const Project& p);

    ProjectRegistry registry_;
    mutable std::mutex mutex_;
    std::string activeId_;
    std::string dataDir_;
    std::atomic<bool> initialized_{false};
};

} // namespace bps::project
