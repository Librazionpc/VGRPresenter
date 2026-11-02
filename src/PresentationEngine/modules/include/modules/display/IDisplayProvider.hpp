#pragma once

#include "core/common/Common.hpp"
#include "modules/display/DisplayTypes.hpp"

#include <string>
#include <vector>

namespace bps::display {
struct DisplayProviderCapabilities {
    size_t maxOutputs = 0;          // 0 = unlimited
    bool supportsHotPlug = false;   // Probe() detects connect/disconnect
    bool supportsVirtualOutputs = false;
    std::vector<ScalingMode> scalingModes;
    std::vector<ColorProfile> colorProfiles;
};

class IDisplayProvider {
public:
    virtual ~IDisplayProvider() = default;

    virtual const char* Name() const noexcept = 0;
    virtual const char* Version() const noexcept = 0;

    // All display devices this provider currently offers (enumerated once).
    virtual std::vector<DisplayDevice> Enumerate() const = 0;

    // Re-probe the provider (hot-plug refresh). Returns the set of devices
    // whose state changed since the last call (added/removed/geometry change).
    virtual Result<std::vector<DisplayDevice>> Probe() = 0;

    virtual DisplayProviderCapabilities Capabilities() const noexcept = 0;
};
} // namespace bps::display