#pragma once

#include "modules/project/Project.hpp"

#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace bps::project {
class ProjectRegistry {
public:
    ProjectRegistry() = default;

    Result<void> Register(const Project& p);      // Err::AlreadyExists on dup id
    Result<void> Update(const Project& p);        // Err::Project_NotFound when missing
    Result<void> Unregister(std::string_view id);
    Result<Project> Find(std::string_view id) const;
    Result<Project> FindByName(std::string_view name) const;
    Result<Project> FindByPath(std::string_view path) const;
    std::vector<Project> All() const;
    size_t Count() const;

private:
    mutable std::mutex mutex_;
    std::map<std::string, Project, std::less<>> byId_;        // id → project
    std::map<std::string, std::string, std::less<>> byName_;  // name → id
    std::map<std::string, std::string, std::less<>> byPath_;  // path → id (when saved)
};
} // namespace bps::project