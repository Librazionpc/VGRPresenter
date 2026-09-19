#pragma once

// AssetRegistry (docs/specs/13 §Asset Registry): maps UUID → loaded runtime
// asset, tracks reference counts and pins, provides alias lookup and
// duplicate detection (by content hash).

#include "modules/content/RuntimeAsset.hpp"
#include "core/common/Common.hpp"

#include <memory>
#include <optional>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace bps::content {

class AssetRegistry {
public:
    AssetRegistry() = default;

    Result<std::shared_ptr<RuntimeAsset>> Acquire(const Uuid& uuid);   // refCount++
    void Release(const Uuid& uuid);                                     // refCount--
    Result<void> SetPinned(const Uuid& uuid, bool pinned);
    bool IsPinned(const Uuid& uuid) const;

    Result<void> Register(const RuntimeAsset& asset);
    Result<void> Unregister(const Uuid& uuid);
    std::shared_ptr<RuntimeAsset> Find(const Uuid& uuid) const;
    size_t Count() const;
    std::vector<Uuid> LoadedUuids() const;

    // Aliases: a secondary name that resolves to a UUID.
    Result<void> AddAlias(std::string alias, const Uuid& uuid);
    Result<void> RemoveAlias(std::string_view alias);
    std::optional<Uuid> ResolveAlias(std::string_view alias) const;

    // Duplicate detection: first-registered uuid for a content hash.
    std::optional<Uuid> FindByHash(std::string_view hash) const;

    void Clear();

private:
    mutable std::mutex mutex_;
    std::unordered_map<Uuid, std::shared_ptr<RuntimeAsset>> assets_;
    std::unordered_map<std::string, Uuid> aliases_;     // alias -> uuid
    std::unordered_map<std::string, Uuid> byHash_;      // hash -> uuid (first)
};

} // namespace bps::content
