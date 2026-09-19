#include "modules/content/AssetCache.hpp"

namespace bps::content {

AssetCache::AssetCache(size_t capacityBytes) : capacityBytes_(capacityBytes) {}

void AssetCache::SetCapacity(size_t bytes) {
    std::lock_guard<std::mutex> lock(mutex_);
    capacityBytes_ = bytes;
    EvictTo(capacityBytes_);
}

Result<void> AssetCache::Put(const Uuid& uuid, std::shared_ptr<RuntimeAsset> asset) {
    if (!asset)
        return Error::Make(Err::InvalidArgument, "CAMS", "null asset");
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = map_.find(uuid);
    if (it != map_.end()) {
        bytes_ -= it->second->second->bytes.size();
        lru_.erase(it->second);
        map_.erase(it);
    }
    size_t size = asset->bytes.size();
    lru_.push_front({uuid, std::move(asset)});
    map_[uuid] = lru_.begin();
    bytes_ += size;
    EvictTo(capacityBytes_);
    return Ok();
}

std::shared_ptr<RuntimeAsset> AssetCache::Get(const Uuid& uuid) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = map_.find(uuid);
    if (it == map_.end()) {
        ++misses_;
        return nullptr;
    }
    ++hits_;
    lru_.splice(lru_.begin(), lru_, it->second);   // LRU touch
    return it->second->second;
}

Result<void> AssetCache::Remove(const Uuid& uuid) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = map_.find(uuid);
    if (it == map_.end()) return Ok();
    bytes_ -= it->second->second->bytes.size();
    lru_.erase(it->second);
    map_.erase(it);
    return Ok();
}

void AssetCache::EvictTo(size_t targetBytes) {
    // Rotations guard: if we pass every entry without evicting (all pinned or
    // referenced), stop instead of looping forever.
    size_t rotations = 0;
    while (bytes_ > targetBytes && !lru_.empty()) {
        if (rotations >= lru_.size()) break;   // every remaining entry is protected
        auto& back = lru_.back();
        auto& asset = back.second;
        if (asset->pinned || asset->refCount > 0) {
            // Pinned/referenced entries can't be evicted; move to front and
            // try the previous one.
            lru_.splice(lru_.begin(), lru_, std::prev(lru_.end()));
            ++rotations;
            continue;
        }
        bytes_ -= asset->bytes.size();
        map_.erase(back.first);
        lru_.pop_back();
        ++evictions_;
        rotations = 0;   // made progress; restart the protection scan
    }
}

size_t AssetCache::Shrink(double fraction) {
    std::lock_guard<std::mutex> lock(mutex_);
    size_t target = static_cast<size_t>(capacityBytes_ * fraction);
    size_t before = bytes_;
    EvictTo(target);
    return before > bytes_ ? before - bytes_ : 0;
}

size_t AssetCache::EvictAllUnpinned() {
    std::lock_guard<std::mutex> lock(mutex_);
    size_t freed = 0;
    for (auto it = lru_.begin(); it != lru_.end();) {
        if (!it->second->pinned && it->second->refCount == 0) {
            freed += it->second->bytes.size();
            map_.erase(it->first);
            it = lru_.erase(it);
            ++evictions_;
        } else {
            ++it;
        }
    }
    bytes_ -= freed;
    return freed;
}

void AssetCache::Clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    lru_.clear();
    map_.clear();
    bytes_ = 0;
}

CacheStats AssetCache::Stats() const {
    std::lock_guard<std::mutex> lock(mutex_);
    CacheStats s;
    s.entries = lru_.size();
    s.bytes = bytes_;
    s.capacityBytes = capacityBytes_;
    s.evictions = evictions_;
    s.hits = hits_;
    s.misses = misses_;
    return s;
}

bool AssetCache::Contains(const Uuid& uuid) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return map_.count(uuid) > 0;
}

} // namespace bps::content
