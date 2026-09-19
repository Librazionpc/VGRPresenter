#include "modules/content/AssetManager.hpp"

namespace bps::content {

AssetManager::AssetManager() = default;

void AssetManager::SetLoader(AssetLoader loader) {
    std::lock_guard<std::mutex> lock(mutex_);
    loader_ = std::move(loader);
}

void AssetManager::SetCapacity(size_t bytes) { cache_.SetCapacity(bytes); }

Result<std::shared_ptr<RuntimeAsset>> AssetManager::Load(const AssetMetadata& meta,
                                                         ProgressFn progress) {
    if (auto hit = cache_.Get(meta.uuid); hit) {
        (void)registry_.Register(*hit);   // keep registry in sync (idempotent)
        return Result<std::shared_ptr<RuntimeAsset>>{hit};
    }
    // Read the payload OUTSIDE the registry/cache mutex: disk I/O must not
    // stall unrelated loads/unloads.
    if (!loader_.HasReadFn())
        return Error::Make(Err::InvalidState, "CAMS", "asset loader not configured");
    auto bytes = loader_.Load(meta, std::move(progress));
    if (!bytes.ok()) return bytes.error();

    RuntimeAsset asset;
    asset.meta = meta;
    asset.bytes = std::move(bytes.value());
    asset.loadedAtMs = static_cast<int64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());
    if (meta.type == AssetType::Text || meta.type == AssetType::Json ||
        meta.type == AssetType::Data || meta.type == AssetType::Song)
        asset.text.assign(asset.bytes.begin(), asset.bytes.end());

    // Double-check after the I/O: a concurrent Load may have populated the
    // cache while we were reading.
    if (auto cached = cache_.Get(meta.uuid); cached) {
        (void)registry_.Register(*cached);
        return Result<std::shared_ptr<RuntimeAsset>>{cached};
    }
    auto shared = std::make_shared<RuntimeAsset>(std::move(asset));
    auto putR = cache_.Put(meta.uuid, shared);
    if (!putR.ok()) return putR.error();
    if (auto r = registry_.Register(*shared); !r.ok()) return r.error();
    return Result<std::shared_ptr<RuntimeAsset>>{shared};
}

void AssetManager::Refresh(const Uuid& uuid, const std::vector<uint8_t>& bytes,
                           const AssetMetadata& meta) {
    // Update the registry copy if loaded, and the cache copy if resident.
    if (auto asset = registry_.Find(uuid); asset) {
        asset->meta = meta;
        asset->bytes = bytes;
        asset->text.assign(bytes.begin(), bytes.end());
        asset->dirty = false;
    }
    if (auto cached = cache_.Get(uuid); cached) {
        cached->meta = meta;
        cached->bytes = bytes;
        cached->text.assign(bytes.begin(), bytes.end());
        cached->dirty = false;
    }
}

void AssetManager::Prefetch(const AssetMetadata& meta) {
    if (cache_.Contains(meta.uuid)) return;
    auto r = Load(meta);
    if (r.ok()) {
        // Prefetched entries are cache-resident but not referenced; release
        // the implicit reference so they stay evictable.
        registry_.Release(meta.uuid);
    }
}

Result<std::shared_ptr<RuntimeAsset>> AssetManager::Acquire(const Uuid& uuid) {
    return registry_.Acquire(uuid);
}

void AssetManager::Release(const Uuid& uuid) { registry_.Release(uuid); }

Result<void> AssetManager::Pin(const Uuid& uuid, bool pinned) {
    return registry_.SetPinned(uuid, pinned);
}

Result<void> AssetManager::Unload(const Uuid& uuid) {
    if (auto r = registry_.Unregister(uuid); !r.ok()) return r;
    (void)cache_.Remove(uuid);
    return Ok();
}

Result<void> AssetManager::UnloadAll() {
    registry_.Clear();
    cache_.Clear();
    return Ok();
}

std::shared_ptr<RuntimeAsset> AssetManager::Peek(const Uuid& uuid) const {
    return registry_.Find(uuid);
}

size_t AssetManager::LoadedCount() const { return registry_.Count(); }
size_t AssetManager::CachedCount() const { return cache_.Stats().entries; }
CacheStats AssetManager::GetCacheStats() const { return cache_.Stats(); }

size_t AssetManager::ShrinkCache(double fraction) { return cache_.Shrink(fraction); }
size_t AssetManager::EvictAllUnpinned() { return cache_.EvictAllUnpinned(); }

std::optional<Uuid> AssetManager::FindByHash(std::string_view hash) const {
    return registry_.FindByHash(hash);
}

} // namespace bps::content
