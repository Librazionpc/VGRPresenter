#pragma once

// PerformanceProfiler (docs/specs/16 §3): continuously measures CPU usage, GPU
// usage, memory, VRAM, disk, frame time, import speed, cache hit ratio and
// video decode latency. Rolling-window averages; the AdaptiveRuntime heartbeat
// calls Record* from subsystems (or computes from the PAL Snapshot) and the
// RuntimeOptimizer reads Snapshot() to decide.

#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <mutex>

namespace bps::adaptive {

struct ProfilerSample {
    double cpuPct = 0.0;
    double gpuPct = 0.0;
    uint64_t ramUsedBytes = 0;
    uint64_t ramTotalBytes = 0;
    uint64_t vramUsedBytes = 0;
    uint64_t vramTotalBytes = 0;
    double frameTimeMs = 0.0;
    double importSpeedPerSec = 0.0;   // assets per second
    double cacheHitRatio = 0.0;       // [0..1]
    double videoDecodeLatencyMs = 0.0;
    double cpuTemperatureC = -1.0;
};

class PerformanceProfiler {
public:
    PerformanceProfiler() = default;

    // --- Recording (called by subsystems + the heartbeat) ---
    void RecordCpuPct(double pct);
    void RecordGpuPct(double pct);
    void RecordFrameTime(double ms);
    void RecordImportSpeed(double assetsPerSec);
    void RecordCacheHitRatio(double ratio);
    void RecordDecodeLatency(double ms);
    void RecordTemperature(double celsius);
    void RecordMemory(uint64_t used, uint64_t total);
    void RecordVram(uint64_t used, uint64_t total);

    // --- Aggregates (windowed averages) ---
    ProfilerSample Snapshot() const;
    void Reset();

    size_t SampleCount() const noexcept { return samples_.load(); }

private:
    double Average(const std::deque<double>& q) const;
    void Push(std::deque<double>& q, double v);

    static constexpr size_t kWindow = 32;

    mutable std::mutex mutex_;
    std::deque<double> cpu_, gpu_, frameMs_, import_, cacheHit_, decodeMs_, temp_;
    std::deque<uint64_t> ramUsed_, vramUsed_;
    uint64_t ramTotal_ = 0, vramTotal_ = 0;
    std::atomic<size_t> samples_{0};
};

} // namespace bps::adaptive
