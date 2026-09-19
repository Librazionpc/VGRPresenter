#include "modules/project/ProjectRegistry.hpp"

namespace bps::project {

Result<void> ProjectRegistry::Register(const Project& p) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (byId_.count(p.id))
        return Error::Make(Err::AlreadyExists, "ProjectRegistry", "project '" + p.id + "' exists");
    byId_[p.id] = p;
    byName_[p.name] = p.id;
    if (!p.path.empty()) byPath_[p.path] = p.id;
    return Ok();
}

Result<void> ProjectRegistry::Update(const Project& p) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = byId_.find(p.id);
    if (it == byId_.end())
        return Error::Make(Err::Project_NotFound, "ProjectRegistry", "project '" + p.id + "' not open");
    // Rebuild name/path indexes if they changed.
    if (it->second.name != p.name) byName_.erase(it->second.name);
    if (it->second.path != p.path) byPath_.erase(it->second.path);
    it->second = p;
    byName_[p.name] = p.id;
    if (!p.path.empty()) byPath_[p.path] = p.id;
    return Ok();
}

Result<void> ProjectRegistry::Unregister(std::string_view id) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = byId_.find(id);
    if (it == byId_.end())
        return Error::Make(Err::Project_NotFound, "ProjectRegistry", "project not registered");
    byName_.erase(it->second.name);
    if (!it->second.path.empty()) byPath_.erase(it->second.path);
    byId_.erase(it);
    return Ok();
}

Result<Project> ProjectRegistry::Find(std::string_view id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = byId_.find(id);
    if (it == byId_.end())
        return Error::Make(Err::Project_NotFound, "ProjectRegistry", "project '" + std::string(id) + "' not open");
    return Result<Project>{it->second};
}

Result<Project> ProjectRegistry::FindByName(std::string_view name) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = byName_.find(name);
    if (it == byName_.end())
        return Error::Make(Err::Project_NotFound, "ProjectRegistry", "no project named '" + std::string(name) + "'");
    return Result<Project>{byId_.at(it->second)};
}

Result<Project> ProjectRegistry::FindByPath(std::string_view path) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = byPath_.find(path);
    if (it == byPath_.end())
        return Error::Make(Err::Project_NotFound, "ProjectRegistry", "no project at '" + std::string(path) + "'");
    return Result<Project>{byId_.at(it->second)};
}

std::vector<Project> ProjectRegistry::All() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<Project> out;
    out.reserve(byId_.size());
    for (const auto& [id, p] : byId_) {
        (void)id;
        out.push_back(p);
    }
    return out;
}

size_t ProjectRegistry::Count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return byId_.size();
}

} // namespace bps::project
