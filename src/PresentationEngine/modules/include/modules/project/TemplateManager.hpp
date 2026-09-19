#pragma once

// TemplateManager (docs/specs/15 §Template Manager): reusable project
// templates — creation from an existing project, versioning, default
// template, and applying a template to seed a new project.

#include "modules/project/Project.hpp"
#include "interfaces/IService.hpp"

#include <mutex>
#include <string>
#include <vector>

namespace bps::project {

struct ProjectTemplate {
    std::string id;
    std::string name;
    std::string description;
    Version version{1, 0, 0, ""};
    std::string sourceProjectId;   // project the template was captured from
    bool isDefault = false;
    int64_t createdAtMs = 0;
    json::Value data;              // captured project (minus volatile fields)
};

class TemplateManager {
public:
    static TemplateManager& Instance();

    Result<std::string> Create(std::string_view name, std::string_view sourceProjectId,
                               std::string_view description = {});
    std::vector<ProjectTemplate> List() const;
    Result<ProjectTemplate> Get(std::string_view templateId) const;
    Result<void> Remove(std::string_view templateId);
    Result<void> SetDefault(std::string_view templateId);
    Result<ProjectTemplate> Default() const;

    // Create a new project seeded from the template.
    Result<Project> Apply(std::string_view templateId, std::string_view projectName);

    Result<void> Save();
    Result<void> Load();
    size_t Count() const;

private:
    TemplateManager() = default;

    mutable std::mutex mutex_;
    std::vector<ProjectTemplate> templates_;
};

} // namespace bps::project
