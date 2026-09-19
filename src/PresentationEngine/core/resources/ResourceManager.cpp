#include "core/resources/ResourceManager.hpp"

#include "core/events/EventBus.hpp"
#include "core/events/Events.hpp"
#include "core/logging/Logger.hpp"

#include <algorithm>
#include <string>
#include <format>

namespace bps {

ResourceManager& ResourceManager::Instance() {
    static ResourceManager instance;
    return instance;
}

std::chrono::milliseconds ResourceManager::SamplePeriodFor(ResourceMode mode) const {
    switch (mode) {
        case ResourceMode::Strict:      return std::chrono::milliseconds(250);
        case ResourceMode::Performance: return std::chrono::milliseconds(500);
        case ResourceMode::Battery:     return std::chrono::milliseconds(5000);
        case ResourceMode::Developer:   return std::chrono::milliseconds(1000);
        default:                        return std::chrono::milliseconds(1000);
    }
}

Result<void> ResourceManager::Initialize() {
    if (initialized_.load()) return Ok();
    initialized_.store(true);
    Sample();
    auto res = TaskScheduler::Instance().ScheduleEvery([this] { Sample(); },
                                                       SamplePeriodFor(mode_.load()));
    if (!res.ok())
        return Error::Make(Err::InvalidState, "ResourceManager", "failed to schedule sampling");
    heartbeatTask_ = res.value();
    return Ok();
}

Result<void> ResourceManager::Shutdown() {
    if (!initialized_.load()) return Ok();
    if (heartbeatTask_ != 0) (void)TaskScheduler::Instance().Cancel(heartbeatTask_);
    heartbeatTask_ = 0;
    initialized_.store(false);
    return Ok();
}

PressureLevel ResourceManager::LevelFor(double ratio) {
    if (ratio >= 0.95) return PressureLevel::Critical;
    if (ratio >= 0.90) return PressureLevel::High;
    if (ratio >= 0.80) return PressureLevel::Medium;
    if (ratio >= 0.65) return PressureLevel::Low;
    return PressureLevel::None;
}

void ResourceManager::SampleLocked() {
    MachineTelemetry t;
    t.sampledAt = EngineClock::now();
    t.coreCount = std::thread::hardware_concurrency();
    if (!platform_) {
        telemetry_ = t;   // no platform backend: core count only
        return;
    }

    // All machine counters come from the platform backend (PAL §3.6); deltas
    // between consecutive snapshots give CPU % and network rates.
    platform::Snapshot s = platform_->Sample();
    t.totalRamBytes = s.totalRamBytes;
    t.availableRamBytes = s.availableRamBytes;
    t.usedRamBytes = s.totalRamBytes > s.availableRamBytes
                         ? s.totalRamBytes - s.availableRamBytes : 0;
    t.diskFreeBytes = s.diskFreeBytes;
    t.diskTotalBytes = s.diskTotalBytes;
    t.threadCount = s.threadCount;
    t.batteryPercent = s.batteryPercent;
    t.onBattery = s.onBattery;
    t.gpuVramTotalBytes = s.gpuVramTotalBytes;
    t.gpuVramUsedBytes = s.gpuVramUsedBytes;
    if (s.coreCount > 0) t.coreCount = s.coreCount;

    // CPU utilization = 1 - (idle delta / total delta) between samples.
    if (prevPlatform_.cpuTotalJiffies > 0 && s.cpuTotalJiffies > prevPlatform_.cpuTotalJiffies) {
        uint64_t idleDelta = s.cpuIdleJiffies > prevPlatform_.cpuIdleJiffies
                                 ? s.cpuIdleJiffies - prevPlatform_.cpuIdleJiffies : 0;
        uint64_t totalDelta = s.cpuTotalJiffies - prevPlatform_.cpuTotalJiffies;
        if (totalDelta > 0)
            t.cpuUsagePct = 100.0 * (1.0 - static_cast<double>(idleDelta) /
                                               static_cast<double>(totalDelta));
    }

    // Network rate = byte delta / time delta between samples.
    if (lastNetSample_.time_since_epoch().count() != 0) {
        double secs = std::chrono::duration<double>(t.sampledAt - lastNetSample_).count();
        if (secs > 0.0) {
            t.networkRxBps = s.networkRxBytes >= prevPlatform_.networkRxBytes
                                 ? static_cast<double>(s.networkRxBytes -
                                                       prevPlatform_.networkRxBytes) / secs
                                 : 0.0;
            t.networkTxBps = s.networkTxBytes >= prevPlatform_.networkTxBytes
                                 ? static_cast<double>(s.networkTxBytes -
                                                       prevPlatform_.networkTxBytes) / secs
                                 : 0.0;
        }
    }

    prevPlatform_ = s;
    lastNetSample_ = t.sampledAt;
    telemetry_ = t;
}

void ResourceManager::Sample() {
    std::lock_guard<std::mutex> lock(mutex_);
    try {
        SampleLocked();
    } catch (const std::exception& ex) {
        sampleErrors_.fetch_add(1);
    }

    // --- Auto-decisions (10 §7 Smart rules) ---
    const MachineTelemetry& t = telemetry_;
    PressureLevel mem = t.totalRamBytes > 0
                            ? LevelFor(static_cast<double>(t.usedRamBytes) / static_cast<double>(t.totalRamBytes))
                            : PressureLevel::None;
    PressureLevel cpu = LevelFor(t.cpuUsagePct / 100.0);

    if (mem >= PressureLevel::High && lastMemPressure_ < PressureLevel::High) {
        decisions_.push_back(AutoDecision{EngineClock::now(), "throttle-background",
                                          "memory pressure " + std::string(ToString(mem))});
        auto sticky = std::make_shared<events::ResourcePressureHigh>("memory", mem);
        EventBus::Instance().SetSticky(events::ResourcePressureHigh::kTopic, sticky);
        (void)EventBus::Instance().Publish(*sticky);
        (void)EventBus::Instance().Publish(
            events::ResourcePressureChanged{"memory", lastMemPressure_, mem});
    } else if (mem < PressureLevel::High && lastMemPressure_ >= PressureLevel::High) {
        (void)EventBus::Instance().Publish(
            events::ResourcePressureChanged{"memory", lastMemPressure_, mem});
    }
    if (cpu >= PressureLevel::High && lastCpuPressure_ < PressureLevel::High) {
        decisions_.push_back(AutoDecision{EngineClock::now(), "reduce-polling",
                                          "cpu pressure " + std::string(ToString(cpu))});
        (void)EventBus::Instance().Publish(events::ResourcePressureHigh{"cpu", cpu});
        (void)EventBus::Instance().Publish(
            events::ResourcePressureChanged{"cpu", lastCpuPressure_, cpu});
    } else if (cpu < PressureLevel::High && lastCpuPressure_ >= PressureLevel::High) {
        (void)EventBus::Instance().Publish(
            events::ResourcePressureChanged{"cpu", lastCpuPressure_, cpu});
    }
    lastMemPressure_ = mem;
    lastCpuPressure_ = cpu;
    while (decisions_.size() > kMaxDecisions) decisions_.pop_front();
}

std::vector<ResourceManager::AutoDecision> ResourceManager::LastAutoActions(size_t max) const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<AutoDecision> out;
    out.reserve(std::min(max, decisions_.size()));
    for (auto it = decisions_.rbegin(); it != decisions_.rend() && out.size() < max; ++it)
        out.push_back(*it);
    return out;
}

MachineTelemetry ResourceManager::Telemetry() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return telemetry_;
}

PressureLevel ResourceManager::MemoryPressure() const {
    MachineTelemetry t = Telemetry();
    if (t.totalRamBytes == 0) return PressureLevel::None;
    double used = static_cast<double>(t.usedRamBytes) / static_cast<double>(t.totalRamBytes);
    return LevelFor(used);
}

PressureLevel ResourceManager::CpuPressure() const {
    return LevelFor(Telemetry().cpuUsagePct / 100.0);
}

PressureLevel ResourceManager::DiskPressure() const {
    MachineTelemetry t = Telemetry();
    if (t.diskTotalBytes == 0) return PressureLevel::None;
    double used = 1.0 - static_cast<double>(t.diskFreeBytes) / static_cast<double>(t.diskTotalBytes);
    return LevelFor(used);
}

PressureLevel ResourceManager::GpuVramPressure() const {
    MachineTelemetry t = Telemetry();
    if (t.gpuVramTotalBytes == 0) return PressureLevel::None;
    double used = static_cast<double>(t.gpuVramUsedBytes) / static_cast<double>(t.gpuVramTotalBytes);
    return LevelFor(used);
}

Result<void> ResourceManager::RecordUsage(std::string_view owner, uint64_t bytes) {
    std::lock_guard<std::mutex> lock(mutex_);
    usage_[std::string(owner)] = bytes;
    return Ok();
}

Result<void> ResourceManager::ClearUsage(std::string_view owner) {
    std::lock_guard<std::mutex> lock(mutex_);
    usage_.erase(std::string(owner));
    return Ok();
}

std::vector<UsageEntry> ResourceManager::UsageSnapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<UsageEntry> out;
    out.reserve(usage_.size());
    for (const auto& [owner, bytes] : usage_) out.push_back(UsageEntry{owner, bytes});
    return out;
}

uint64_t ResourceManager::TotalUsageBytes() const {
    std::lock_guard<std::mutex> lock(mutex_);
    uint64_t total = 0;
    for (const auto& [owner, bytes] : usage_) total += bytes;
    return total;
}

Result<void> ResourceManager::SetMode(ResourceMode mode) {
    ResourceMode prev = mode_.exchange(mode);
    if (prev == mode) return Ok();
    if (initialized_.load() && heartbeatTask_ != 0) {
        auto res = TaskScheduler::Instance().Reschedule(heartbeatTask_, SamplePeriodFor(mode));
        if (!res.ok()) return res;
    }
    Logger::Instance().Info("Resource mode -> " + std::string(ToString(mode)), "Resources");
    return Ok();
}

Result<void> ResourceManager::Claim(std::string_view owner, size_t bytes) {
    std::lock_guard<std::mutex> lock(mutex_);
    claims_[std::string(owner)] += bytes;
    return Ok();
}

Result<void> ResourceManager::ReleaseClaim(std::string_view owner) {
    std::lock_guard<std::mutex> lock(mutex_);
    claims_.erase(std::string(owner));
    return Ok();
}

HealthReport ResourceManager::GetHealth() const {
    HealthReport r;
    MachineTelemetry t = Telemetry();
    PressureLevel mem = MemoryPressure();
    r.errorCount = sampleErrors_.load();
    r.state = (mem >= PressureLevel::High || sampleErrors_.load() > 0)
                  ? HealthState::Degraded
                  : HealthState::Healthy;
    r.detail = std::format("mode={} mem_used={}MB/{}MB cpu={}% threads={} disk_free={}MB",
                           ToString(mode_.load()), t.usedRamBytes / (1024 * 1024),
                           t.totalRamBytes / (1024 * 1024), static_cast<int>(t.cpuUsagePct),
                           t.threadCount, t.diskFreeBytes / (1024 * 1024));
    if (t.gpuVramTotalBytes > 0)
        r.detail += std::format(" vram={}MB/{}MB", t.gpuVramUsedBytes / (1024 * 1024),
                                t.gpuVramTotalBytes / (1024 * 1024));
    uint64_t usageBytes = TotalUsageBytes();
    if (usageBytes > 0)
        r.detail += std::format(" usage={}MB", usageBytes / (1024 * 1024));
    return r;
}

Metrics ResourceManager::MetricsSnapshot() const {
    Metrics m;
    MachineTelemetry t = Telemetry();
    m.cpuPct = t.cpuUsagePct;
    m.ramBytes = t.usedRamBytes;
    m.health = GetHealth().state;
    return m;
}

} // namespace bps
