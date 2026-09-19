#pragma once

// AssetCache (docs/specs/13 §Asset Cache): LRU memory cache with capacity,
// pinning, priority hints, and size/time eviction. Eviction skips pinned and
// referenced entries. `Shrink(percent)` is called by ContentManager when the
// ResourceManager reports memory pressure.

#include "modules/content/RuntimeAsset.hpp"
#include "core/common/Common.hpp"

#include <chrono>
#include <cstdint>
#include <list>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

namespace bps::content {

struct CacheStats {
    size_t entries = 0;
    size_t bytes = 0;
    size_t capacityBytes = 0;
    size_t evictions = 0;
    size_t hits = 0;
    size_t misses = 0;
};

class AssetCache {
public:
    explicit AssetCache(size_t capacityBytes = 64u * 1024u * 1024u);

    void SetCapacity(size_t bytes);
    size_t Capacity() const { return capacityBytes_; }

    // Insert (or refresh) an entry. Returns bytes dropped by eviction.
    Result<void> Put(const Uuid& uuid, std::shared_ptr<RuntimeAsset> asset);
    std::shared_ptr<RuntimeAsset> Get(const Uuid& uuid);   // LRU touch on hit
    Result<void> Remove(const Uuid& uuid);

    // Evict least-recently-used unpinned, unreferenced entries until the cache
    // is at `fraction` of capacity (e.g. 0.5 after memory pressure).
    size_t Shrink(double fraction);
    size_t EvictAllUnpinned();
    void Clear();

    CacheStats Stats() const;
    bool Contains(const Uuid& uuid) const;

private:
    void EvictTo(size_t targetBytes);   // requires mutex_

    mutable std::mutex mutex_;
    size_t capacityBytes_;
    size_t bytes_ = 0;
    size_t evictions_ = 0;
    size_t hits_ = 0;
    size_t misses_ = 0;
    std::list<std::pair<Uuid, std::shared_ptr<RuntimeAsset>>> lru_;
    std::unordered_map<Uuid, std::list<std::pair<Uuid, std::shared_ptr<RuntimeAsset>>>::iterator> map_;
};

} // namespace bps::content
