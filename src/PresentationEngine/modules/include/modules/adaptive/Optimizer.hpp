#pragma once

// RuntimeOptimizer (docs/specs/16 §4, §22–§25): the brain. Every subsystem
// asks it instead of deciding for itself:
//   Renderer → GetTextureBudget / ShouldUseGpuText / ShouldUseGpuBlur
//   Media    → ShouldUseHardwareDecoder / GetStreamingQuality
//   AI       → GetAIModelTier
//   Search   → GetSearchTier
//   Import   → GetBatchSize
//   Cache    → GetCacheBytes
//   Any      → GetRecommendedThreadCount
// Decisions derive from hardware + budgets + quality profile + pressure +
// user preference. Nothing outside this file hardcodes those numbers.

#include "modules/adaptive/Budgets.hpp"
#include "modules/adaptive/Hardware.hpp"
#include "modules/adaptive/PerformanceProfiler.hpp"
#include "modules/adaptive/Quality.hpp"

#include <mutex>

namespace bps::adaptive {

enum class GpuTask : int {
    Text, Decode, Blur, Transitions, Scaling, Compositing, Effects
};

class RuntimeOptimizer {
public:
    RuntimeOptimizer() = default;

    // Wire the inputs the optimizer reads. Called by the facade at start.
    // settingsMutex guards *settings (written by the facade's Recompute on the
    // heartbeat/event threads, read by any module thread here).
    void Bind(const HardwareInfo* hw, const CapabilityDetector* caps,
              const PerformanceProfiler* profiler, ResourceBudgetManager* budgets,
              UserModeManager* modes, QualitySettings* settings,
              std::mutex* settingsMutex) {
        hw_ = hw;
        caps_ = caps;
        profiler_ = profiler;
        budgets_ = budgets;
        modes_ = modes;
        settings_ = settings;
        settingsMutex_ = settingsMutex;
    }

    // Locked copy of the current quality settings (never blocks long).
    QualitySettings LockedSettings() const {
        if (!settingsMutex_ || !settings_) return {};
        std::lock_guard<std::mutex> lock(*settingsMutex_);
        return *settings_;
    }

    // --- The questions (docs/specs/16 §2 “Zero hardcoded decisions”) ---
    unsigned GetRecommendedThreadCount() const;
    uint64_t GetTextureBudget() const;
    uint64_t GetCacheBytes() const;
    uint64_t GetRenderCacheBytes() const;
    size_t GetBatchSize() const;                  // import/thumbnail batch
    bool ShouldUseHardwareDecoder() const;
    bool ShouldUseGpu(GpuTask task) const;
    int GetAIModelTier() const;                   // 0=none .. 3=cloud
    int GetSearchTier() const;                    // 0=simple .. 2=vector
    int GetStreamingQuality() const;              // 360..1080+
    unsigned GetThumbnailResolutionPct() const;   // 25..100
    bool ShouldPreloadFrequentlyUsed() const;
    bool BackgroundWorkAllowed() const;           // false under battery/thermal/presenting
    QualityLevel GetQualityLevel() const;

    // Reaction: the optimizer drives live adaptation where it owns the knobs.
    void Apply();

private:
    const HardwareInfo* hw_ = nullptr;
    const CapabilityDetector* caps_ = nullptr;
    const PerformanceProfiler* profiler_ = nullptr;
    ResourceBudgetManager* budgets_ = nullptr;
    UserModeManager* modes_ = nullptr;
    QualitySettings* settings_ = nullptr;
    std::mutex* settingsMutex_ = nullptr;
    mutable std::mutex mutex_;
};

} // namespace bps::adaptive
