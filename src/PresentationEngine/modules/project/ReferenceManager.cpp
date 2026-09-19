#include "modules/project/ReferenceManager.hpp"

#include "modules/content/ContentManager.hpp"
#include "modules/project/ProjectManager.hpp"

#include <algorithm>
#include <map>
#include <set>

namespace bps::project {

ReferenceManager& ReferenceManager::Instance() {
    static ReferenceManager instance;
    return instance;
}

ReferenceReport ReferenceManager::Analyze(std::string_view projectId) const {
    ReferenceReport report;
    auto& content = content::ContentManager::Instance();
    auto p = ProjectManager::Instance().Get(projectId);

    // Project's referenced uuids.
    std::set<std::string> referenced;
    if (p.ok())
        for (const auto& u : p.value().assetUuids) referenced.insert(u);

    // All library assets.
    std::map<std::string, std::string> hashToUuid;   // hash → uuid (first seen)
    for (const auto& meta : content.List()) {
        std::string uuid = meta.uuid.ToString();
        if (referenced.count(uuid)) continue;   // used → never "unused"
        report.unused.push_back(uuid);
        // Duplicate detection: same checksum/hash as another asset.
        std::string key = meta.hash.empty() ? meta.checksum : meta.hash;
        if (!key.empty()) {
            if (hashToUuid.count(key))
                report.duplicates.push_back(uuid);
            else
                hashToUuid[key] = uuid;
        }
    }
    // Broken: referenced uuids with no library metadata.
    for (const auto& u : referenced)
        if (!content.GetMetadata(content::Uuid::FromString(u)).ok()) report.broken.push_back(u);
    analyzes_.fetch_add(1);
    return report;
}

ReferenceReport ReferenceManager::AnalyzeAll() const {
    ReferenceReport report;
    std::set<std::string> referencedByAnyone;
    std::map<std::string, int> usage;   // uuid → project count
    for (const auto& p : ProjectManager::Instance().OpenProjects()) {
        for (const auto& u : p.assetUuids) {
            usage[u]++;
            referencedByAnyone.insert(u);
        }
    }
    auto& content = content::ContentManager::Instance();
    for (const auto& meta : content.List()) {
        std::string uuid = meta.uuid.ToString();
        if (!referencedByAnyone.count(uuid)) report.unused.push_back(uuid);
    }
    for (const auto& [uuid, count] : usage)
        if (count >= 2) report.shared.push_back(uuid);
    for (const auto& u : referencedByAnyone)
        if (!content.GetMetadata(content::Uuid::FromString(u)).ok()) report.broken.push_back(u);
    analyzes_.fetch_add(1);
    return report;
}

Result<void> ReferenceManager::RemoveUnused(std::string_view projectId,
                                            const std::vector<std::string>& onlyUuids) {
    ReferenceReport r = Analyze(projectId);
    auto& content = content::ContentManager::Instance();
    for (const auto& uuid : r.unused) {
        if (!onlyUuids.empty() &&
            std::find(onlyUuids.begin(), onlyUuids.end(), uuid) == onlyUuids.end())
            continue;
        (void)content.Delete(content::Uuid::FromString(uuid));
    }
    return Ok();
}

Result<void> ReferenceManager::ClearDuplicates(std::string_view projectId) {
    ReferenceReport r = Analyze(projectId);
    auto& content = content::ContentManager::Instance();
    for (const auto& uuid : r.duplicates)
        (void)content.Delete(content::Uuid::FromString(uuid));
    return Ok();
}

} // namespace bps::project
