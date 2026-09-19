#pragma once

#include "core/common/Common.hpp"
#include "core/events/EventBus.hpp"
#include "core/events/Events.hpp"
#include "interfaces/IService.hpp"

#include <atomic>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace bps {

enum class AssetType : int { Unknown = 0, Texture, Image, Audio, Video, Font, Data };
enum class AssetState : int { Unknown = 0, Registered, Loading, Loaded, Failed, Unloaded };

struct AssetInfo {
    std::string id;
    AssetType type = AssetType::Unknown;
    uint64_t sizeBytes = 0;
    AssetState state = AssetState::Unknown;
    std::string source;
    EngineTime loadedAt;
    EngineTime lastUsedAt;
};

// Central asset registry/cache (core/assets/). The engine core does not decode
// media — it tracks, prefetches and unloads assets for feature modules, and
// reacts to memory pressure automatically (ResourceManager pressure events).
class AssetManager final : public IService {
public:
    static AssetManager& Instance();

    Result<void> Initialize() override;   // subscribes to resource pressure events
    Result<void> Shutdown() override;

    Result<AssetInfo> Register(std::string id, AssetType type, uint64_t sizeBytes,
                               std::string source = {});
    Result<void> Load(std::string_view id);          // synchronous simulated load
    Result<void> Unload(std::string_view id);
    Result<void> UnloadAll();
    Result<void> Prefetch(const std::vector<std::string>& ids);
    Result<AssetInfo> Get(std::string_view id) const;

    // LRU trim under pressure: drops least-recently-used Loaded assets until the
    // retained byte count is <= targetBytes. Returns bytes freed.
    size_t ShrinkTo(size_t targetBytes);

    std::vector<AssetInfo> Snapshot() const;
    size_t CachedBytes() const;

    const char* ServiceName() const noexcept override { return "AssetManager"; }
    HealthReport GetHealth() const override;

private:
    AssetManager() = default;
    void OnPressure(const events::ResourcePressureHigh& e);

    mutable std::mutex mutex_;
    std::unordered_map<std::string, AssetInfo> assets_;   // guarded by mutex_
    Subscription pressureSub_;
    std::atomic<bool> initialized_{false};
};

} // namespace bps
