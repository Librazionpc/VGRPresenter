#include "modules/adaptive/Budgets.hpp"

#include <algorithm>

namespace bps::adaptive {

namespace {
constexpr uint64_t kMb = 1024ull * 1024ull;
}

void ResourceBudgetManager::Recompute(uint64_t totalRamBytes, const QualitySettings& q,
                                      PressureLevel pressure) {
    std::lock_guard<std::mutex> lock(mutex_);
    // The quality settings already encode machine size; the budget manager
    // splits the cache/render pool and applies pressure discounts.
    const double pressureFactor =
        pressure >= PressureLevel::Critical ? 0.25 :
        pressure == PressureLevel::High      ? 0.5  :
        pressure == PressureLevel::Medium    ? 0.75 : 1.0;

    // AI is gated by tier: 0 = disabled, 1+ gets a share.
    const bool aiEnabled = q.aiModelTier > 0;
    const uint64_t cachePool = q.cacheBudgetBytes;
    const uint64_t renderPool = q.renderCacheBytes + q.textureBudgetBytes;

    Budget renderer;
    renderer.subsystem = Subsystem::kRenderer;
    renderer.maxBytes = static_cast<uint64_t>(renderPool * pressureFactor);
    renderer.threadLimit = q.workerThreads;

    Budget cache;
    cache.subsystem = Subsystem::kCache;
    cache.maxBytes = static_cast<uint64_t>(cachePool * pressureFactor);
    cache.threadLimit = std::max(1u, q.workerThreads / 2);

    Budget search;
    search.subsystem = Subsystem::kSearch;
    search.maxBytes = aiEnabled ? static_cast<uint64_t>(256 * kMb * pressureFactor)
                                : static_cast<uint64_t>(128 * kMb * pressureFactor);

    Budget video;
    video.subsystem = Subsystem::kVideo;
    video.maxBytes = static_cast<uint64_t>(std::max<uint64_t>(
        128 * kMb, totalRamBytes / 16) * pressureFactor);

    Budget ai;
    ai.subsystem = Subsystem::kAi;
    ai.maxBytes = aiEnabled ? static_cast<uint64_t>(totalRamBytes / 12 * pressureFactor) : 0;
    ai.enabled = aiEnabled;
    ai.threadLimit = aiEnabled ? std::max(1u, q.workerThreads / 4) : 0;

    Budget thumbnails;
    thumbnails.subsystem = Subsystem::kThumbnails;
    thumbnails.maxBytes = q.backgroundThumbnails
                              ? static_cast<uint64_t>(96 * kMb * pressureFactor)
                              : 0;
    thumbnails.enabled = q.backgroundThumbnails;
    thumbnails.threadLimit = q.backgroundThumbnails ? std::max(1u, q.workerThreads / 4) : 0;

    budgets_[Subsystem::kRenderer] = std::move(renderer);
    budgets_[Subsystem::kCache] = std::move(cache);
    budgets_[Subsystem::kSearch] = std::move(search);
    budgets_[Subsystem::kVideo] = std::move(video);
    budgets_[Subsystem::kAi] = std::move(ai);
    budgets_[Subsystem::kThumbnails] = std::move(thumbnails);
}

Budget ResourceBudgetManager::Get(std::string_view subsystem) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = budgets_.find(subsystem);
    if (it == budgets_.end()) return Budget{std::string(subsystem), 0, 0, true};
    return it->second;
}

std::vector<Budget> ResourceBudgetManager::All() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<Budget> out;
    out.reserve(budgets_.size());
    for (const auto& [name, b] : budgets_) {
        (void)name;
        out.push_back(b);
    }
    return out;
}

uint64_t ResourceBudgetManager::Total() const {
    std::lock_guard<std::mutex> lock(mutex_);
    uint64_t total = 0;
    for (const auto& [name, b] : budgets_) {
        (void)name;
        total += b.maxBytes;
    }
    return total;
}

} // namespace bps::adaptive
