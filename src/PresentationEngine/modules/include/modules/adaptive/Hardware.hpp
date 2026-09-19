#pragma once

// Hardware profiling + capability detection (docs/specs/16 §1–2). The engine
// decides by *capabilities*, never by device names: modules ask
// `Supports(HardwareVideoDecode)`, never `if (RTX ...)`. All raw OS data comes
// from the PAL (IPlatform Info/Snapshot/Monitor/Power) — this is the only place
// outside platform/ that reasons about hardware.

#include "core/common/Common.hpp"
#include "platform/IPlatform.hpp"
#include "platform/PlatformAccessor.hpp"

#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace bps::adaptive {

// ---------------------------------------------------------------------------
// Hardware view (gathered once at startup + refreshed on demand)
// ---------------------------------------------------------------------------
struct HardwareInfo {
    std::string osName;
    std::string osVersion;
    std::string arch;
    unsigned coreCount = 0;
    uint64_t totalRamBytes = 0;

    uint64_t gpuVramTotalBytes = 0;
    std::string gpuName;                 // best effort (for the dashboard only)
    bool gpuDetected = false;

    int displayCount = 0;
    int maxRefreshRateHz = 0;            // across all monitors (0 = unknown)
    bool hdrSupported = false;

    bool hasBattery = false;
    bool onBattery = false;
    int batteryPercent = -1;

    uint64_t diskTotalBytes = 0;
    uint64_t diskFreeBytes = 0;
    bool storageIsSsd = false;           // best effort; false = unknown/HDD

    double thermalCelsius = -1.0;        // -1 = not exposed
};

// ---------------------------------------------------------------------------
// Capabilities — the vocabulary modules ask about (docs/specs/16 §2)
// ---------------------------------------------------------------------------
enum class Capability : int {
    HardwareVideoDecode,
    HardwareVideoEncode,
    DirectX12,
    Vulkan,
    Hdr,
    Raytracing,
    ComputeShader,
    Avx2,
    Avx512,
    Sse4,
    Neon,
    Cuda,
    OpenCl,
    Count
};

inline const char* ToString(Capability c) {
    switch (c) {
        case Capability::HardwareVideoDecode: return "HardwareVideoDecode";
        case Capability::HardwareVideoEncode: return "HardwareVideoEncode";
        case Capability::DirectX12:           return "DirectX12";
        case Capability::Vulkan:              return "Vulkan";
        case Capability::Hdr:                 return "HDR";
        case Capability::Raytracing:          return "Raytracing";
        case Capability::ComputeShader:       return "ComputeShader";
        case Capability::Avx2:                return "AVX2";
        case Capability::Avx512:              return "AVX512";
        case Capability::Sse4:                return "SSE4";
        case Capability::Neon:                return "NEON";
        case Capability::Cuda:                return "CUDA";
        case Capability::OpenCl:              return "OpenCL";
        case Capability::Count:               return "Count";
    }
    return "Unknown";
}

// ---------------------------------------------------------------------------
// HardwareProfiler — gathers the hardware view from the PAL
// ---------------------------------------------------------------------------
class HardwareProfiler {
public:
    HardwareProfiler() = default;

    // Refresh the view from the PAL backend (best effort — never throws).
    void Refresh();

    const HardwareInfo& Info() const noexcept { return info_; }
    // Convenience accessors used by the optimizer.
    unsigned CoreCount() const noexcept { return info_.coreCount; }
    uint64_t TotalRamBytes() const noexcept { return info_.totalRamBytes; }
    uint64_t VramBytes() const noexcept { return info_.gpuVramTotalBytes; }
    int DisplayCount() const noexcept { return info_.displayCount; }

private:
    HardwareInfo info_;
    mutable std::mutex mutex_;
};

// ---------------------------------------------------------------------------
// CapabilityDetector — answers Supports(capability) from the hardware view
// ---------------------------------------------------------------------------
class CapabilityDetector {
public:
    CapabilityDetector() = default;

    void SetHardware(const HardwareInfo& h) {
        std::lock_guard<std::mutex> lock(mutex_);
        hw_ = h;
    }

    bool Supports(Capability c) const;

    std::vector<Capability> Supported() const;

private:
    bool CpuHas(Capability c) const;

    mutable std::mutex mutex_;
    HardwareInfo hw_;
};

} // namespace bps::adaptive
