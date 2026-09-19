#include "modules/content/AssetDatabase.hpp"

#include <algorithm>

namespace bps::content {

namespace {

std::string ExtensionOf(std::string_view path) {
    auto dot = path.find_last_of('.');
    if (dot == std::string_view::npos) return {};
    std::string ext;
    for (size_t i = dot + 1; i < path.size(); ++i)
        ext += static_cast<char>(std::tolower(static_cast<unsigned char>(path[i])));
    return ext;
}

std::vector<AssetMetadata> Resolve(const std::set<std::string>& ids,
                                   const std::unordered_map<Uuid, AssetMetadata>& byUuid) {
    std::vector<AssetMetadata> out;
    out.reserve(ids.size());
    for (const auto& id : ids) {
        Uuid u = Uuid::FromString(id);
        auto it = byUuid.find(u);
        if (it != byUuid.end()) out.push_back(it->second);
    }
    return out;
}

} // namespace

Result<void> AssetDatabase::Upsert(const AssetMetadata& meta) {
    if (meta.uuid == Uuid{})
        return Error::Make(Err::InvalidArgument, "CAMS", "asset uuid must be set");
    std::lock_guard<std::mutex> lock(mutex_);
    std::string id = meta.uuid.ToString();
    auto [it, inserted] = byUuid_.insert_or_assign(meta.uuid, meta);
    (void)it;

    if (!inserted) {
        // Rebuild indexes for this uuid from the fresh record.
        for (auto& [t, set] : byType_) set.erase(id);
        for (auto& [t, set] : byTag_) set.erase(id);
        for (auto& [t, set] : byCategory_) set.erase(id);
        for (auto& [t, set] : byExt_) set.erase(id);
        favorites_.erase(id);
        for (auto it2 = pathToUuid_.begin(); it2 != pathToUuid_.end();) {
            if (it2->second == id) it2 = pathToUuid_.erase(it2);
            else ++it2;
        }
    }
    if (!meta.path.empty()) pathToUuid_[meta.path] = id;
    byType_[meta.type].insert(id);
    for (const auto& tag : meta.tags) byTag_[tag].insert(id);
    if (!meta.category.empty()) byCategory_[meta.category].insert(id);
    byExt_[ExtensionOf(meta.path)].insert(id);
    if (meta.favorite) favorites_.insert(id);
    return Ok();
}

Result<void> AssetDatabase::Remove(const Uuid& uuid) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = byUuid_.find(uuid);
    if (it == byUuid_.end())
        return Error::Make(Err::NotFound, "CAMS", "asset not in database");
    std::string id = uuid.ToString();
    for (auto& [t, set] : byType_) set.erase(id);
    for (auto& [t, set] : byTag_) set.erase(id);
    for (auto& [t, set] : byCategory_) set.erase(id);
    for (auto& [t, set] : byExt_) set.erase(id);
    favorites_.erase(id);
    for (auto it2 = pathToUuid_.begin(); it2 != pathToUuid_.end();) {
        if (it2->second == id) it2 = pathToUuid_.erase(it2);
        else ++it2;
    }
    byUuid_.erase(it);
    return Ok();
}

Result<AssetMetadata> AssetDatabase::Get(const Uuid& uuid) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = byUuid_.find(uuid);
    if (it == byUuid_.end())
        return Error::Make(Err::NotFound, "CAMS", "asset not in database");
    return Result<AssetMetadata>{it->second};
}

bool AssetDatabase::Contains(const Uuid& uuid) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return byUuid_.count(uuid) > 0;
}

size_t AssetDatabase::Count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return byUuid_.size();
}

std::vector<AssetMetadata> AssetDatabase::All() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<AssetMetadata> out;
    out.reserve(byUuid_.size());
    for (const auto& [u, m] : byUuid_) out.push_back(m);
    return out;
}

std::vector<AssetMetadata> AssetDatabase::ByType(AssetType type) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = byType_.find(type);
    return it == byType_.end() ? std::vector<AssetMetadata>{} : Resolve(it->second, byUuid_);
}

std::vector<AssetMetadata> AssetDatabase::ByTag(std::string_view tag) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = byTag_.find(std::string(tag));
    return it == byTag_.end() ? std::vector<AssetMetadata>{} : Resolve(it->second, byUuid_);
}

std::vector<AssetMetadata> AssetDatabase::ByCategory(std::string_view category) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = byCategory_.find(std::string(category));
    return it == byCategory_.end() ? std::vector<AssetMetadata>{} : Resolve(it->second, byUuid_);
}

std::vector<AssetMetadata> AssetDatabase::ByExtension(std::string_view ext) const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::string key;
    for (char c : ext) key += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    auto it = byExt_.find(key);
    return it == byExt_.end() ? std::vector<AssetMetadata>{} : Resolve(it->second, byUuid_);
}

std::vector<AssetMetadata> AssetDatabase::Favorites() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return Resolve(favorites_, byUuid_);
}

std::optional<AssetMetadata> AssetDatabase::FindByPath(std::string_view path) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = pathToUuid_.find(std::string(path));
    if (it == pathToUuid_.end()) return std::nullopt;
    Uuid u = Uuid::FromString(it->second);
    auto mit = byUuid_.find(u);
    if (mit == byUuid_.end()) return std::nullopt;
    return mit->second;
}

void AssetDatabase::Clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    byUuid_.clear();
    pathToUuid_.clear();
    byType_.clear();
    byTag_.clear();
    byCategory_.clear();
    byExt_.clear();
    favorites_.clear();
}

} // namespace bps::content
