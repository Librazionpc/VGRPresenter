#include "core/assets/AssetManager.hpp"

#include "core/events/EventBus.hpp"
#include "core/events/Events.hpp"

#include <algorithm>
#include <utility>
#include <format>

namespace bps {

AssetManager& AssetManager::Instance() {
    static AssetManager instance;
    return instance;
}

Result<void> AssetManager::Initialize() {
    if (initialized_.exchange(true)) return Ok();
    pressureSub_ = EventBus::Instance().Subscribe<events::ResourcePressureHigh>(
        [this](const events::ResourcePressureHigh& e) { OnPressure(e); });
    return Ok();
}

Result<void> AssetManager::Shutdown() {
    if (!initialized_.exchange(false)) return Ok();
    (void)EventBus::Instance().Unsubscribe(pressureSub_);
    std::lock_guard<std::mutex> lock(mutex_);
    assets_.clear();
    return Ok();
}

Result<AssetInfo> AssetManager::Register(std::string id, AssetType type, uint64_t sizeBytes,
                                         std::string source) {
    if (id.empty())
        return Error::Make(Err::InvalidArgument, "AssetManager", "asset id must not be empty");
    std::lock_guard<std::mutex> lock(mutex_);
    AssetInfo a;
    a.id = std::move(id);
    a.type = type;
    a.sizeBytes = sizeBytes;
    a.source = std::move(source);
    a.state = AssetState::Registered;
    std::string key = a.id;   // copy before the move: a is moved-from afterwards
    assets_[key] = std::move(a);
    return assets_.at(key);
}

Result<void> AssetManager::Load(std::string_view id) {
    AssetState state = AssetState::Unknown;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = assets_.find(std::string(id));
        if (it == assets_.end())
            return Error::Make(Err::NotFound, "AssetManager",
                               "unknown asset '" + std::string(id) + "'");
        it->second.state = AssetState::Loaded;
        it->second.loadedAt = EngineClock::now();
        it->second.lastUsedAt = it->second.loadedAt;
        state = it->second.state;
    }
    (void)EventBus::Instance().Publish(
        events::AssetStateChanged{std::string(id), static_cast<int>(state)});
    return Ok();
}

Result<void> AssetManager::Unload(std::string_view id) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = assets_.find(std::string(id));
    if (it == assets_.end())
        return Error::Make(Err::NotFound, "AssetManager",
                           "unknown asset '" + std::string(id) + "'");
    if (it->second.state == AssetState::Loaded) it->second.state = AssetState::Unloaded;
    return Ok();
}

Result<void> AssetManager::UnloadAll() {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& [id, a] : assets_) {
        (void)id;
        a.state = AssetState::Unloaded;
    }
    return Ok();
}

Result<void> AssetManager::Prefetch(const std::vector<std::string>& ids) {
    for (const auto& id : ids)
        if (auto r = Load(id); !r.ok()) return r;
    return Ok();
}

Result<AssetInfo> AssetManager::Get(std::string_view id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = assets_.find(std::string(id));
    if (it == assets_.end())
        return Error::Make(Err::NotFound, "AssetManager",
                           "unknown asset '" + std::string(id) + "'");
    return it->second;
}

size_t AssetManager::ShrinkTo(size_t targetBytes) {
    std::vector<std::pair<EngineTime, std::string>> lru;
    size_t freed = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        size_t cached = 0;
        for (const auto& [id, a] : assets_)
            if (a.state == AssetState::Loaded) {
                (void)id;
                cached += a.sizeBytes;
                lru.emplace_back(a.lastUsedAt, a.id);
            }
        if (cached <= targetBytes) return 0;
        std::sort(lru.begin(), lru.end());
        for (const auto& [at, id] : lru) {
            (void)at;
            if (cached <= targetBytes) break;
            auto& a = assets_.at(id);
            a.state = AssetState::Unloaded;
            cached -= a.sizeBytes;
            freed += a.sizeBytes;
        }
    }
    return freed;
}

void AssetManager::OnPressure(const events::ResourcePressureHigh& e) {
    if (e.level < PressureLevel::High) return;
    // Only memory pressure justifies dropping cached assets â evicting them because the
    // CPU is busy frees nothing that matters and just makes the next load slower.
    if (e.resource != "memory") return;
    size_t target = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        size_t cached = 0;
        for (const auto& [id, a] : assets_)
            if (a.state == AssetState::Loaded) {
                (void)id;
                cached += a.sizeBytes;
            }
        target = cached / 2;
    }
    (void)ShrinkTo(target);
}

std::vector<AssetInfo> AssetManager::Snapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<AssetInfo> out;
    out.reserve(assets_.size());
    for (const auto& [id, a] : assets_) {
        (void)id;
        out.push_back(a);
    }
    return out;
}

size_t AssetManager::CachedBytes() const {
    std::lock_guard<std::mutex> lock(mutex_);
    size_t total = 0;
    for (const auto& [id, a] : assets_)
        if (a.state == AssetState::Loaded) {
            (void)id;
            total += a.sizeBytes;
        }
    return total;
}

HealthReport AssetManager::GetHealth() const {
    HealthReport r;
    r.state = initialized_.load() ? HealthState::Healthy : HealthState::Degraded;
    r.detail = std::format("assets={} cached={}B", Snapshot().size(), CachedBytes());
    return r;
}

} // namespace bps
