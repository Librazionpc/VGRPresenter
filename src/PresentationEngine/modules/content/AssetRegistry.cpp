#include "modules/content/AssetRegistry.hpp"

namespace bps::content {

Result<std::shared_ptr<RuntimeAsset>> AssetRegistry::Acquire(const Uuid& uuid) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = assets_.find(uuid);
    if (it == assets_.end())
        return Error::Make(Err::Content_NotFound, "CAMS", "asset not loaded in registry");
    it->second->refCount++;
    return Result<std::shared_ptr<RuntimeAsset>>{it->second};
}

void AssetRegistry::Release(const Uuid& uuid) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = assets_.find(uuid);
    if (it != assets_.end() && it->second->refCount > 0) it->second->refCount--;
}

Result<void> AssetRegistry::SetPinned(const Uuid& uuid, bool pinned) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = assets_.find(uuid);
    if (it == assets_.end())
        return Error::Make(Err::Content_NotFound, "CAMS", "asset not loaded in registry");
    it->second->pinned = pinned;
    return Ok();
}

bool AssetRegistry::IsPinned(const Uuid& uuid) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = assets_.find(uuid);
    return it != assets_.end() && it->second->pinned;
}

Result<void> AssetRegistry::Register(const RuntimeAsset& asset) {
    if (asset.meta.uuid == Uuid{})
        return Error::Make(Err::InvalidArgument, "CAMS", "asset uuid must be set");
    std::lock_guard<std::mutex> lock(mutex_);
    auto [it, inserted] = assets_.try_emplace(asset.meta.uuid, std::make_shared<RuntimeAsset>(asset));
    (void)it;
    if (inserted && !asset.meta.hash.empty())
        byHash_.try_emplace(asset.meta.hash, asset.meta.uuid);
    return Ok();
}

Result<void> AssetRegistry::Unregister(const Uuid& uuid) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = assets_.find(uuid);
    if (it == assets_.end())
        return Error::Make(Err::Content_NotFound, "CAMS", "asset not loaded in registry");
    if (it->second->refCount > 0)
        return Error::Make(Err::InvalidState, "CAMS", "asset still referenced");
    if (it->second->pinned)
        return Error::Make(Err::InvalidState, "CAMS", "asset is pinned");
    if (!it->second->meta.hash.empty()) byHash_.erase(it->second->meta.hash);
    assets_.erase(it);
    return Ok();
}

std::shared_ptr<RuntimeAsset> AssetRegistry::Find(const Uuid& uuid) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = assets_.find(uuid);
    return it == assets_.end() ? nullptr : it->second;
}

size_t AssetRegistry::Count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return assets_.size();
}

std::vector<Uuid> AssetRegistry::LoadedUuids() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<Uuid> out;
    out.reserve(assets_.size());
    for (const auto& [u, a] : assets_) {
        (void)a;
        out.push_back(u);
    }
    return out;
}

Result<void> AssetRegistry::AddAlias(std::string alias, const Uuid& uuid) {
    if (alias.empty())
        return Error::Make(Err::InvalidArgument, "CAMS", "alias must not be empty");
    std::lock_guard<std::mutex> lock(mutex_);
    aliases_[std::move(alias)] = uuid;
    return Ok();
}

Result<void> AssetRegistry::RemoveAlias(std::string_view alias) {
    std::lock_guard<std::mutex> lock(mutex_);
    aliases_.erase(std::string(alias));
    return Ok();
}

std::optional<Uuid> AssetRegistry::ResolveAlias(std::string_view alias) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = aliases_.find(std::string(alias));
    if (it == aliases_.end()) return std::nullopt;
    return it->second;
}

std::optional<Uuid> AssetRegistry::FindByHash(std::string_view hash) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = byHash_.find(std::string(hash));
    if (it == byHash_.end()) return std::nullopt;
    return it->second;
}

void AssetRegistry::Clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    assets_.clear();
    aliases_.clear();
    byHash_.clear();
}

} // namespace bps::content
