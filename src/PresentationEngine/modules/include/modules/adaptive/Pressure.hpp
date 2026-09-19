#pragma once

// Pressure managers (docs/specs/16 §14–§17): the runtime's reflexes. Battery,
// thermal and memory pressure throttle background work and shrink caches —
// never the live presentation. The GPURuntime answers GPU-vs-CPU questions.

#include "core/common/Common.hpp"

#include <atomic>
#include <mutex>
#include <string>

namespace bps::adaptive {

// --- PowerManager (docs/specs/16 §14) ---------------------------------------
// Battery state → reduce FPS, background tasks, thumbnails, animations, AI.
struct PowerState {
    bool hasBattery = false;
    bool onBattery = false;
    int batteryPercent = -1;
};

class PowerManager {
public:
    PowerManager() = default;

    void Update(bool hasBattery, bool onBattery, int percent);
    PowerState State() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return state_;
    }
    // True when the engine should throttle background work for battery life.
    bool ShouldThrottle() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return state_.onBattery && (state_.batteryPercent < 0 || state_.batteryPercent <= 50);
    }

private:
    mutable std::mutex mutex_;
    PowerState state_;
};

// --- ThermalManager (docs/specs/16 §15) -------------------------------------
// CPU temperature → reduce background indexing, AI, thumbnails, cache rebuilds.
class ThermalManager {
public:
    ThermalManager() = default;

    void Update(double celsius);   // -1 = unknown
    double Temperature() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return tempC_;
    }
    // True when throttling is advised (≥ 85 °C) or unknown-but-suspicious.
    bool ShouldThrottle() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return tempC_ >= 85.0;
    }

private:
    mutable std::mutex mutex_;
    double tempC_ = -1.0;
};

// --- MemoryPressureManager (docs/specs/16 §16) -------------------------------
// RAM pressure → unload unused textures, AI/search/thumbnail caches, shrink
// render cache. The reaction itself is delegated (CAMS ShrinkCache + resource
// manager); this manager records the level and the decision.
class MemoryPressureManager {
public:
    MemoryPressureManager() = default;

    void Update(PressureLevel level);
    PressureLevel Level() const { return level_.load(); }
    // Bytes this manager suggests reclaiming (0 = none).
    uint64_t SuggestedReclaimBytes(uint64_t cacheBytes) const;

private:
    std::atomic<PressureLevel> level_{PressureLevel::None};
};

// --- GPURuntime (docs/specs/16 §17) ------------------------------------------
// GPU available → GPU text/decode/blur/scale/composition; no GPU → CPU fallback.
// Rendered decisions come from the optimizer; this is the thin policy holder.
class GPURuntime {
public:
    GPURuntime() = default;

    void SetGpuAvailable(bool available) { gpuAvailable_.store(available); }
    bool GpuAvailable() const { return gpuAvailable_.load(); }

private:
    std::atomic<bool> gpuAvailable_{false};
};

} // namespace bps::adaptive
