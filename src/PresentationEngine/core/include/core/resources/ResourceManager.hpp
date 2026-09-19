#pragma once

#include "core/common/Common.hpp"
#include "interfaces/IService.hpp"
#include "platform/IPlatform.hpp"
#include "core/task_scheduler/TaskScheduler.hpp"

#include <atomic>
#include <deque>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace bps {

// Snapshot of machine resource state (docs/specs/10 §3).
struct MachineTelemetry {
    uint64_t totalRamBytes = 0;
    uint64_t availableRamBytes = 0;
    uint64_t usedRamBytes = 0;
    double cpuUsagePct = 0.0;
    uint64_t diskFreeBytes = 0;
    uint64_t diskTotalBytes = 0;
    double networkRxBps = 0.0;
    double networkTxBps = 0.0;
    uint64_t threadCount = 0;      // live threads in this process
    int batteryPercent = -1;       // -1 = no battery
    bool onBattery = false;
    unsigned coreCount = 0;
    uint64_t gpuVramTotalBytes = 0;   // 0 = no GPU backend reported
    uint64_t gpuVramUsedBytes = 0;
    EngineTime sampledAt;
};

// Per-owner byte usage entry for cache/module monitoring (10 §3).
struct UsageEntry {
    std::string owner;
    uint64_t bytes = 0;
};

class ResourceManager final : public IService {
public:
    static ResourceManager& Instance();

    Result<void> Initialize() override;      // starts periodic sampling (scheduler heartbeat)
    Result<void> Shutdown() override;        // cancels the heartbeat

    MachineTelemetry Telemetry() const;    // last sampled snapshot (lock-free-ish copy)
    void Sample();                         // force a sample now

    PressureLevel MemoryPressure() const;
    PressureLevel CpuPressure() const;
    PressureLevel DiskPressure() const;
    PressureLevel GpuVramPressure() const;

    // Cache / module-usage monitoring (10 §3): a per-owner byte ledger. Asset
    // caches, module buffers, and plugin heaps report here so pressure rules
    // can see who consumes what (e.g. AssetManager -> RecordUsage("assets", ..)).
    Result<void> RecordUsage(std::string_view owner, uint64_t bytes);
    Result<void> ClearUsage(std::string_view owner);
    std::vector<UsageEntry> UsageSnapshot() const;
    uint64_t TotalUsageBytes() const;

    Result<void> SetMode(ResourceMode mode);
    ResourceMode Mode() const noexcept { return mode_.load(); }

    // Platform backend (PAL, SystemArchitecture §3.6): machine telemetry source.
    // Without a platform the manager reports core count only.
    void SetPlatform(std::shared_ptr<platform::IPlatform> p) {
        std::lock_guard<std::mutex> lock(mutex_);
        platform_ = std::move(p);
        prevPlatform_ = {};          // reset deltas: the old backend's counters
        lastNetSample_ = {};         // are meaningless across the swap
    }
    bool HasPlatform() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return platform_ != nullptr;
    }

    // Budget accounting for owners (10 §2): coarse byte claims.
    Result<void> Claim(std::string_view owner, size_t bytes);
    Result<void> ReleaseClaim(std::string_view owner);

    // Auto-decisions (10 §7 Smart rules): actions taken automatically on pressure
    // crossings (throttle background, tighten polling, publish pressure events).
    struct AutoDecision {
        EngineTime at;
        std::string action;
        std::string reason;
    };
    std::vector<AutoDecision> LastAutoActions(size_t max = 16) const;

    const char* ServiceName() const noexcept override { return "ResourceManager"; }
    HealthReport GetHealth() const override;
    Metrics MetricsSnapshot() const;

private:
    ResourceManager() = default;

    void SampleLocked();                     // requires mutex_
    static PressureLevel LevelFor(double ratio);
    std::chrono::milliseconds SamplePeriodFor(ResourceMode mode) const;

    std::atomic<ResourceMode> mode_{ResourceMode::Balanced};
    mutable std::mutex mutex_;
    MachineTelemetry telemetry_;                            // guarded by mutex_
    std::unordered_map<std::string, size_t> claims_;        // guarded by mutex_
    std::unordered_map<std::string, uint64_t> usage_;       // guarded by mutex_
    std::atomic<uint64_t> sampleErrors_{0};
    std::atomic<bool> initialized_{false};

    std::shared_ptr<platform::IPlatform> platform_;         // guarded by mutex_
    platform::Snapshot prevPlatform_;                       // guarded by mutex_
    EngineTime lastNetSample_{};                            // guarded by mutex_
    PressureLevel lastMemPressure_{PressureLevel::None};    // guarded by mutex_
    PressureLevel lastCpuPressure_{PressureLevel::None};    // guarded by mutex_
    std::deque<AutoDecision> decisions_;                    // guarded by mutex_
    static constexpr size_t kMaxDecisions = 64;
    TaskId heartbeatTask_ = 0;
};

} // namespace bps
