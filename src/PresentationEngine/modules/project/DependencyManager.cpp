#include "modules/project/DependencyManager.hpp"

#include "modules/content/ContentManager.hpp"

#include <algorithm>
#include <cstring>

namespace bps::project {

DependencyManager& DependencyManager::Instance() {
    static DependencyManager instance;
    return instance;
}

Result<void> DependencyManager::Add(std::string_view projectId, std::string_view parent,
                                    std::string_view child, std::string_view kind) {
    if (child.empty())
        return Error::Make(Err::InvalidArgument, "Dependency", "child must not be empty");
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& e : edges_)
        if (e.projectId == projectId && e.parent == parent && e.child == child) return Ok();
    edges_.push_back(DependencyEdge{std::string(projectId), std::string(parent),
                                    std::string(child), std::string(kind)});
    return Ok();
}

Result<void> DependencyManager::Remove(std::string_view projectId, std::string_view parent,
                                       std::string_view child) {
    std::lock_guard<std::mutex> lock(mutex_);
    edges_.erase(std::remove_if(edges_.begin(), edges_.end(),
                                [&](const DependencyEdge& e) {
                                    return e.projectId == projectId && e.parent == parent &&
                                           e.child == child;
                                }),
                 edges_.end());
    return Ok();
}

Result<void> DependencyManager::ClearProject(std::string_view projectId) {
    std::lock_guard<std::mutex> lock(mutex_);
    edges_.erase(std::remove_if(edges_.begin(), edges_.end(),
                                [&](const DependencyEdge& e) { return e.projectId == projectId; }),
                 edges_.end());
    return Ok();
}

std::vector<DependencyEdge> DependencyManager::Dependencies(std::string_view projectId) const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<DependencyEdge> out;
    for (const auto& e : edges_)
        if (e.projectId == projectId) out.push_back(e);
    return out;
}

std::vector<DependencyIssue> DependencyManager::Validate(std::string_view projectId) const {
    std::vector<DependencyEdge> edges = Dependencies(projectId);
    auto& content = content::ContentManager::Instance();
    std::vector<DependencyIssue> issues;
    for (const auto& e : edges) {
        bool present = content.GetMetadata(content::Uuid::FromString(e.child)).ok();
        // Non-CAMS children (e.g. a bare filename) are checked against the
        // library path index instead.
        if (!present && e.child.contains('/')) present = content.FindByPath(e.child).has_value();
        if (!present)
            issues.push_back(DependencyIssue{e.projectId, e.parent, e.child, e.kind, "missing"});
    }
    return issues;
}

Result<void> DependencyManager::SyncProject(std::string_view projectId,
                                            const std::vector<std::string>& assetUuids) {
    std::lock_guard<std::mutex> lock(mutex_);
    edges_.erase(std::remove_if(edges_.begin(), edges_.end(),
                                [&](const DependencyEdge& e) {
                                    return e.projectId == projectId && e.parent == projectId;
                                }),
                 edges_.end());
    for (const auto& uuid : assetUuids)
        edges_.push_back(DependencyEdge{std::string(projectId), std::string(projectId), uuid,
                                        "asset"});
    return Ok();
}

size_t DependencyManager::EdgeCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return edges_.size();
}

} // namespace bps::project
