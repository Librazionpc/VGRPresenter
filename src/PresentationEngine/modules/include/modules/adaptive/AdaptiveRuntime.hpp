#pragma once

// AdaptiveRuntime (docs/specs/16): the facade every subsystem consults instead
// of making isolated hardware/resource decisions. Owns the profilers, the
// capability detector, feature registry, quality/user modes, budgets, the
// optimizer brain, the pressure reflexes and the learning engine; runs a
// heartbeat (Collect → Analyze → Optimize) and publishes adaptive.* events.
// "The engine becomes self-optimizing rather than user-optimized."

#include "core/common/Common.hpp"
#include "core/events/EventBus.hpp"
#include "core/events/Events.hpp"
#include "core/task_scheduler/TaskScheduler.hpp"
#include "interfaces/IService.hpp"
#include "modules/adaptive/Budgets.hpp"
#include "modules/adaptive/Contracts.hpp"
#include "modules/adaptive/Hardware.hpp"
#include "modules/adaptive/Learning.hpp"
#include "modules/adaptive/Optimizer.hpp"
#include "modules/adaptive/PerformanceProfiler.hpp"
#include "modules/adaptive/Pressure.hpp"
#include "modules/adaptive/Quality.hpp"

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace bps::adaptive {

// Dashboard snapshot (docs/specs/16 §14).
struct RuntimeSnapshot {
    std::string mode;            // active user mode
    std::string layer;           // Automatic | Assisted | Expert
    std::string quality;         // active quality level
    std::string gpuName;
    bool gpuAvailable = false;
    unsigned cores = 0;
    uint64_t totalRamBytes = 0;
    uint64_t ramUsedBytes = 0;
    uint64_t vramUsedBytes = 0;
    uint64_t vramTotalBytes = 0;
    double cpuPct = 0.0;
    double gpuPct = 0.0;
    double frameTimeMs = 0.0;
    double temperatureC = -1.0;
    unsigned gpuCapPct = 100;    // the share of the GPU the user allows (Settings > Smart Config)
    unsigned cpuCapPct = 100;    // ...and of the CPU
    unsigned workerThreads = 0;
    size_t activeFeatures = 0;
    size_t suspendedFeatures = 0;
    uint64_t memoryBudgetBytes = 0;   // total budgeted across subsystems
    std::vector<Budget> budgets;
    std::vector<std::string> preloadCandidates;
    std::vector<Recommendation> recommendations;
};

class AdaptiveRuntime final : public IService {
public:
    static AdaptiveRuntime& Instance();

    // --- Lifecycle (engine-wide contract) ---
    Result<void> Initialize() override;
    Result<void> Start() override;
    Result<void> Stop() override;
    Result<void> Shutdown() override;
    Result<void> Reload() override;
    Result<void> Reset() override;
    HealthReport GetHealth() const override;
    Metrics MetricsSnapshot() const override;
    const char* ServiceName() const noexcept override { return "AdaptiveRuntime"; }

    // --- Hardware & capabilities ---
    const HardwareInfo& Hardware() const;
    bool Supports(Capability c) const;
    std::vector<Capability> Capabilities() const;

    // --- The questions every subsystem asks (docs/specs/16 §2) ---
    unsigned GetRecommendedThreadCount() const;
    uint64_t GetTextureBudget() const;
    uint64_t GetCacheBytes() const;
    uint64_t GetRenderCacheBytes() const;
    size_t GetBatchSize() const;
    bool ShouldUseHardwareDecoder() const;
    bool ShouldUseGpu(GpuTask task) const;
    int GetAIModelTier() const;
    int GetSearchTier() const;
    int GetStreamingQuality() const;
    unsigned GetThumbnailResolutionPct() const;
    bool ShouldPreloadFrequentlyUsed() const;
    bool BackgroundWorkAllowed() const;
    QualityLevel GetQualityLevel() const;

    // --- User control (modes / layers / preferences) ---
    Result<void> SetUserMode(UserMode mode);
    UserMode GetUserMode() const;
    Result<void> SetConfigLayer(ConfigLayer layer);
    ConfigLayer GetConfigLayer() const;
    Result<void> SetPreference(Preference p);
    Preference GetPreference() const;
    // The share of the machine the user lets the engine use (Settings > Smart Config's resource budgets), 10..100 each.
    // The CPU cap bounds the recommended worker count, the GPU cap scales the texture budget; everything that asks the
    // runtime (GetRecommendedThreadCount / GetTextureBudget) sees the capped answer. 100 = no cap (the default).
    Result<void> SetResourceCaps(unsigned gpuPct, unsigned cpuPct);
    unsigned GpuCapPct() const { return gpuCapPct_.load(); }
    unsigned CpuCapPct() const { return cpuCapPct_.load(); }

    // --- Features ---
    Result<void> RegisterFeature(const FeatureDef& def);
    Result<void> SetFeatureEnabled(std::string_view id, bool enabled);
    Result<void> SetFeatureSuspended(std::string_view id, bool suspended);
    Result<FeatureState> GetFeatureState(std::string_view id) const;
    std::vector<FeatureDef> Features() const;

    // --- Learning ---
    void RecordUsage(std::string_view kind, std::string_view id);
    std::vector<std::string> PreloadCandidates(std::string_view kind, int limit = 5) const;
    bool ShouldPreload(std::string_view kind, std::string_view id) const;
    Result<void> SaveLearning();
    Result<void> LoadLearning();

    // --- Reactions (called by pressure events / heartbeat) ---
    // Apply the current optimum to live systems: thread pool resize, cache
    // budgets, resource mode. Publishes adaptive.* events on change.
    Result<void> ApplyOptimization(std::string_view reason);

    // --- Dashboard ---
    RuntimeSnapshot Snapshot() const;

private:
    AdaptiveRuntime() = default;

    void WireEvents();
    void UnwireEvents();
    void OnPressure(const events::ResourcePressureChanged& e);
    void OnPowerChanged(const events::PowerChanged& e);
    void OnBatteryLow(const events::BatteryLow& e);
    void OnMonitorConnected(const events::MonitorConnected& e);
    void OnMonitorDisconnected(const events::MonitorDisconnected& e);
    void OnSleep(const events::SystemSleep& e);
    void OnWake(const events::SystemWake& e);
    void OnConfigReload(const events::ConfigHotReload& e);

    // Heartbeat: Collect metrics → Analyze → Optimize (docs/specs/16 §16).
    void Heartbeat();

    // Recompute budgets + quality settings from current state and publish
    // budget events when they change.
    void Recompute(uint64_t totalRamBytes, PressureLevel pressure);

    HardwareProfiler hardware_;
    // Stable snapshot the optimizer is bound to (refreshed on hardware change);
    // avoids racing Refresh() which mutates the profiler's internal view.
    HardwareInfo hwSnapshot_;
    CapabilityDetector caps_;
    PerformanceProfiler profiler_;
    FeatureManager features_;
    UserModeManager modes_;
    // settings_ is written by Recompute() (heartbeat/event threads) and read by
    // the optimizer (any module thread) — guarded by settingsMutex_.
    mutable std::mutex settingsMutex_;
    QualitySettings settings_;
    ResourceBudgetManager budgets_;
    // Serializes ApplyOptimization (heartbeat tick + event publishers).
    std::mutex applyMutex_;
    RuntimeOptimizer optimizer_;
    PowerManager power_;
    ThermalManager thermal_;
    MemoryPressureManager memPressure_;
    GPURuntime gpu_;
    UsageLearningEngine learning_;
    SmartCacheManager smartCache_;
    StartupOptimizer startup_;
    RecommendationEngine recommendations_;

    std::vector<Subscription> subscriptions_;
    std::atomic<TaskId> heartbeatTask_{0};
    std::atomic<uint64_t> errorCount_{0};
    std::atomic<uint64_t> optimizationCount_{0};
    std::atomic<bool> initialized_{false};
    std::atomic<bool> running_{false};
    std::atomic<QualityLevel> activeQuality_{QualityLevel::Balanced};
    std::atomic<unsigned> gpuCapPct_{100};
    std::atomic<unsigned> cpuCapPct_{100};
    std::atomic<PressureLevel> activePressure_{PressureLevel::None};
    // Previous PAL jiffies (CPU% computed as a delta across heartbeats).
    std::atomic<uint64_t> prevCpuTotal_{0};
    std::atomic<uint64_t> prevCpuIdle_{0};
};

} // namespace bps::adaptive
