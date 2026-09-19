#pragma once

// AssetDatabase (docs/specs/13 §Asset Database): metadata-only store. Never
// holds file contents — just AssetMetadata records plus fast indexes.

#include "modules/content/AssetMetadata.hpp"
#include "core/common/Common.hpp"

#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

namespace bps::content {

class AssetDatabase {
public:
    AssetDatabase() = default;

    Result<void> Upsert(const AssetMetadata& meta);
    Result<void> Remove(const Uuid& uuid);
    Result<AssetMetadata> Get(const Uuid& uuid) const;
    bool Contains(const Uuid& uuid) const;
    size_t Count() const;

    std::vector<AssetMetadata> All() const;
    std::vector<AssetMetadata> ByType(AssetType type) const;
    std::vector<AssetMetadata> ByTag(std::string_view tag) const;
    std::vector<AssetMetadata> ByCategory(std::string_view category) const;
    std::vector<AssetMetadata> ByExtension(std::string_view extLower) const;
    std::vector<AssetMetadata> Favorites() const;

    // Lookup by VFS path (one-to-one: a path maps to at most one asset).
    std::optional<AssetMetadata> FindByPath(std::string_view path) const;

    void Clear();

private:
    mutable std::mutex mutex_;
    std::unordered_map<Uuid, AssetMetadata> byUuid_;
    std::map<std::string, std::string> pathToUuid_;     // path -> uuid string
    std::map<AssetType, std::set<std::string>> byType_;      // type -> uuid strings
    std::map<std::string, std::set<std::string>> byTag_;     // tag -> uuid strings
    std::map<std::string, std::set<std::string>> byCategory_; // category -> uuid strings
    std::map<std::string, std::set<std::string>> byExt_;      // ext -> uuid strings
    std::set<std::string> favorites_;                         // uuid strings
};

} // namespace bps::content
