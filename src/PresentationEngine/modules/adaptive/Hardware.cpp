#include "modules/adaptive/Hardware.hpp"

#include <algorithm>
#include <cstring>

#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
#define BPS_X86 1
#endif
#if defined(__aarch64__) || defined(_M_ARM64) || defined(__arm__) || defined(_M_ARM)
#define BPS_ARM 1
#endif

namespace bps::adaptive {

void HardwareProfiler::Refresh() {
    // Gather from the PAL FIRST, with no lock held. Info/Sample/Monitor/Power/
    // Environment are OS calls — a monitor enumeration can block on a sleeping
    // panel or a hung display driver — and mutex_ is shared with every reader
    // (Info/CoreCount/TotalRamBytes), so it must never be held across them.
    HardwareInfo info;
    auto& platform = platform::PlatformAccessor::Get();
    auto sys = platform.Info();
    info.osName = sys.osName;
    info.osVersion = sys.osVersion;
    info.arch = sys.arch;
    info.coreCount = sys.coreCount > 0 ? sys.coreCount
                                       : static_cast<unsigned>(std::thread::hardware_concurrency());
    info.totalRamBytes = sys.totalRamBytes;

    // Rich telemetry (Snapshot) + monitors + power, all best effort.
    auto snap = platform.Sample();
    info.gpuVramTotalBytes = snap.gpuVramTotalBytes;
    info.gpuDetected = snap.gpuVramTotalBytes > 0;
    info.diskTotalBytes = snap.diskTotalBytes;
    info.diskFreeBytes = snap.diskFreeBytes;
    info.onBattery = snap.onBattery;
    info.batteryCharging = snap.batteryCharging;
    info.batteryPercent = snap.batteryPercent;
    info.hasBattery = snap.batteryPercent >= 0;
    // Package temperature, where the backend exposes one (Windows PDH thermal
    // zones). -1 keeps "not exposed", which is what a desktop/server is.
    info.thermalCelsius = snap.cpuTempC;

    try {
        auto monitors = platform.Monitor().Enumerate();
        info.displayCount = static_cast<int>(monitors.size());
        info.maxRefreshRateHz = 0;
        info.hdrSupported = false;
        for (const auto& m : monitors) {
            if (m.connected) {
                info.maxRefreshRateHz = std::max(info.maxRefreshRateHz, m.refreshRateHz);
                if (m.hdrSupported) info.hdrSupported = true;
            }
        }
    } catch (...) {
        info.displayCount = 0;   // PAL backend may be minimal
    }

    try {
        auto power = platform.Power().Current();
        info.hasBattery = power.hasBattery;
        info.onBattery = power.onBattery;
        info.batteryCharging = power.charging;
        if (power.batteryPercent >= 0) info.batteryPercent = power.batteryPercent;
    } catch (...) {
    }

    // GPU name: only for the dashboard; never used for decisions.
    try {
        info.gpuName = platform.Environment().Current().gpuName;
    } catch (...) {
    }

    // Publish the finished view: the lock covers the assignment only.
    std::lock_guard<std::mutex> lock(mutex_);
    info_ = info;
}

namespace {
// CPUID-based x86 feature detection (no external dependency, header-local).
#if defined(BPS_X86)
#include <cpuid.h>
struct CpuIdRegs {
    unsigned a = 0, b = 0, c = 0, d = 0;
};
CpuIdRegs CpuId(unsigned leaf, unsigned sub = 0) {
    CpuIdRegs r;
    __cpuid_count(leaf, sub, r.a, r.b, r.c, r.d);
    return r;
}
#endif
} // namespace

bool CapabilityDetector::CpuHas(Capability c) const {
#if defined(BPS_X86)
    switch (c) {
        case Capability::Sse4: {
            auto r = CpuId(1);
            return (r.c & (1u << 19)) != 0 || (r.c & (1u << 20)) != 0;   // SSE4.1/4.2
        }
        case Capability::Avx2: {
            auto r = CpuId(7, 0);
            return (r.b & (1u << 5)) != 0;
        }
        case Capability::Avx512: {
            auto r = CpuId(7, 0);
            return (r.b & (1u << 16)) != 0;   // AVX512F
        }
        default:
            return false;
    }
#elif defined(BPS_ARM)
    return c == Capability::Neon;   // NEON is baseline on ARMv8+
#else
    (void)c;
    return false;
#endif
}

bool CapabilityDetector::Supports(Capability c) const {
    std::lock_guard<std::mutex> lock(mutex_);
    switch (c) {
        case Capability::Sse4:
        case Capability::Avx2:
        case Capability::Avx512:
        case Capability::Neon:
            return CpuHas(c);
        case Capability::Hdr:
            return hw_.hdrSupported;
        case Capability::HardwareVideoDecode:
        case Capability::HardwareVideoEncode:
            // GPU present + enough VRAM ⇒ hardware media pipelines are viable.
            return hw_.gpuDetected && hw_.gpuVramTotalBytes >= (256ull << 20);
        case Capability::Cuda:
        case Capability::OpenCl:
        case Capability::ComputeShader:
            return hw_.gpuDetected;
        case Capability::Vulkan:
            // Best-effort: a detected GPU is treated as Vulkan-capable on
            // desktop OSes; renderers may verify at device creation.
            return hw_.gpuDetected &&
                   (hw_.osName == "Linux" || hw_.osName == "Windows");
        case Capability::DirectX12:
            return hw_.gpuDetected && hw_.osName == "Windows";
        case Capability::Raytracing:
            return hw_.gpuDetected && hw_.gpuVramTotalBytes >= (6ull << 30);
        default:
            return false;
    }
}

std::vector<Capability> CapabilityDetector::Supported() const {
    std::vector<Capability> out;
    for (int i = 0; i < static_cast<int>(Capability::Count); ++i) {
        if (Supports(static_cast<Capability>(i))) out.push_back(static_cast<Capability>(i));
    }
    return out;
}

} // namespace bps::adaptive
