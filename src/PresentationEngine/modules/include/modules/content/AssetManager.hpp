#pragma once

// AssetManager (docs/specs/13 §Asset Manager): runtime assets. Owns loaded
// assets, lifetime, references, and state; combines the AssetRegistry (who is
// loaded) with the AssetCache (what bytes are resident) and the AssetLoader
// (how bytes are read). Supports Load/Unload/Reload/Pin/Release/Prefetch and
// reference counting.

#include "modules/content/AssetCache.hpp"
#include "modules/content/AssetLoader.hpp"
#include "modules/content/AssetRegistry.hpp"

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>

namespace bps::content {

class AssetManager {
public:
    AssetManager();

    void SetLoader(AssetLoader loader);
    void SetCapacity(size_t bytes);

    // Load: cache hit returns immediately; miss reads through the loader.
    Result<std::shared_ptr<RuntimeAsset>> Load(const AssetMetadata& meta,
                                               ProgressFn progress = {});
    // Refresh: after a Save(), update the in-memory copy so cached reads and
    // held references see the new bytes (docs/specs/13 §Asset Manager).
    void Refresh(const Uuid& uuid, const std::vector<uint8_t>& bytes,
                 const AssetMetadata& meta);
    // Prefetch: load without retaining a reference (warms the cache).
    void Prefetch(const AssetMetadata& meta);
    Result<std::shared_ptr<RuntimeAsset>> Acquire(const Uuid& uuid);
    void Release(const Uuid& uuid);
    Result<void> Pin(const Uuid& uuid, bool pinned);
    Result<void> Unload(const Uuid& uuid);
    Result<void> UnloadAll();

    std::shared_ptr<RuntimeAsset> Peek(const Uuid& uuid) const;
    size_t LoadedCount() const;
    size_t CachedCount() const;
    CacheStats GetCacheStats() const;

    // ResourceManager integration: release memory on pressure.
    size_t ShrinkCache(double fraction);
    size_t EvictAllUnpinned();

    // Duplicate detection across the registry.
    std::optional<Uuid> FindByHash(std::string_view hash) const;

private:
    AssetRegistry registry_;
    AssetCache cache_;
    AssetLoader loader_{nullptr};
    std::mutex mutex_;
};

} // namespace bps::content
