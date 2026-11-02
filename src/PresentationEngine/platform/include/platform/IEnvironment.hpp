#pragma once

#include "core/common/Common.hpp"

#include <string>

namespace bps::platform {
struct EnvironmentInfo {
    std::string osName;      // "Ubuntu 24.04 LTS", "Windows 11", ...
    std::string osVersion;   // kernel / build release
    std::string arch;        // "x86_64", "aarch64"
    std::string buildNumber; // OS build id ("" when unknown)
    std::string cpuModel;    // e.g. "AMD Ryzen 7 7840U" (best effort)
    std::string gpuName;     // e.g. "NVIDIA GeForce RTX 4070" (best effort)
    uint64_t totalRamBytes = 0;
    unsigned coreCount = 0;
    std::string username;
    std::string hostname;
    std::string locale;      // "en-US"
    std::string timezone;    // "UTC", "America/New_York"
};

class IEnvironment {
public:
    virtual ~IEnvironment() = default;
    virtual EnvironmentInfo Current() const = 0;
};
} // namespace bps::platform