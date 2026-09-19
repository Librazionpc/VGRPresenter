#include "modules/adaptive/PerformanceProfiler.hpp"

#include <numeric>

namespace bps::adaptive {

void PerformanceProfiler::Push(std::deque<double>& q, double v) {
    if (q.size() >= kWindow) q.pop_front();
    q.push_back(v);
}

void PerformanceProfiler::RecordCpuPct(double pct) {
    std::lock_guard<std::mutex> lock(mutex_);
    Push(cpu_, pct);
    samples_.fetch_add(1);
}

void PerformanceProfiler::RecordGpuPct(double pct) {
    std::lock_guard<std::mutex> lock(mutex_);
    Push(gpu_, pct);
}

void PerformanceProfiler::RecordFrameTime(double ms) {
    std::lock_guard<std::mutex> lock(mutex_);
    Push(frameMs_, ms);
}

void PerformanceProfiler::RecordImportSpeed(double assetsPerSec) {
    std::lock_guard<std::mutex> lock(mutex_);
    Push(import_, assetsPerSec);
}

void PerformanceProfiler::RecordCacheHitRatio(double ratio) {
    std::lock_guard<std::mutex> lock(mutex_);
    Push(cacheHit_, ratio);
}

void PerformanceProfiler::RecordDecodeLatency(double ms) {
    std::lock_guard<std::mutex> lock(mutex_);
    Push(decodeMs_, ms);
}

void PerformanceProfiler::RecordTemperature(double celsius) {
    std::lock_guard<std::mutex> lock(mutex_);
    Push(temp_, celsius);
}

void PerformanceProfiler::RecordMemory(uint64_t used, uint64_t total) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (ramUsed_.size() >= kWindow) ramUsed_.pop_front();
    ramUsed_.push_back(used);
    ramTotal_ = total;
}

void PerformanceProfiler::RecordVram(uint64_t used, uint64_t total) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (vramUsed_.size() >= kWindow) vramUsed_.pop_front();
    vramUsed_.push_back(used);
    vramTotal_ = total;
}

double PerformanceProfiler::Average(const std::deque<double>& q) const {
    if (q.empty()) return 0.0;
    double sum = std::accumulate(q.begin(), q.end(), 0.0);
    return sum / static_cast<double>(q.size());
}

ProfilerSample PerformanceProfiler::Snapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    ProfilerSample s;
    s.cpuPct = Average(cpu_);
    s.gpuPct = Average(gpu_);
    s.frameTimeMs = Average(frameMs_);
    s.importSpeedPerSec = Average(import_);
    s.cacheHitRatio = Average(cacheHit_);
    s.videoDecodeLatencyMs = Average(decodeMs_);
    s.cpuTemperatureC = Average(temp_);
    s.ramTotalBytes = ramTotal_;
    s.vramTotalBytes = vramTotal_;
    if (!ramUsed_.empty()) s.ramUsedBytes = ramUsed_.back();
    if (!vramUsed_.empty()) s.vramUsedBytes = vramUsed_.back();
    return s;
}

void PerformanceProfiler::Reset() {
    std::lock_guard<std::mutex> lock(mutex_);
    cpu_.clear();
    gpu_.clear();
    frameMs_.clear();
    import_.clear();
    cacheHit_.clear();
    decodeMs_.clear();
    temp_.clear();
    ramUsed_.clear();
    vramUsed_.clear();
    samples_.store(0);
}

} // namespace bps::adaptive
