#include "modules/adaptive/Pressure.hpp"

#include <algorithm>

namespace bps::adaptive {

void PowerManager::Update(bool hasBattery, bool onBattery, int percent) {
    std::lock_guard<std::mutex> lock(mutex_);
    state_.hasBattery = hasBattery;
    state_.onBattery = onBattery;
    state_.batteryPercent = percent;
}

void ThermalManager::Update(double celsius) {
    std::lock_guard<std::mutex> lock(mutex_);
    tempC_ = celsius;
}

void MemoryPressureManager::Update(PressureLevel level) { level_.store(level); }

uint64_t MemoryPressureManager::SuggestedReclaimBytes(uint64_t cacheBytes) const {
    switch (level_.load()) {
        case PressureLevel::Critical: return cacheBytes;          // reclaim all cache
        case PressureLevel::High:     return (cacheBytes * 3) / 4;
        case PressureLevel::Medium:   return cacheBytes / 2;
        case PressureLevel::Low:      return cacheBytes / 4;
        case PressureLevel::None:
        default:                      return 0;
    }
}

} // namespace bps::adaptive
