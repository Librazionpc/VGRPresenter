#pragma once

// PAL power subsystem (Phase 2): battery / AC state. The ResourceManager uses
// this to drive the Battery resource mode and low-power decisions.

#include <cstdint>

namespace bps::platform {

struct PowerInfo {
    bool hasBattery = false;
    int batteryPercent = -1; // -1 = unknown
    bool onBattery = false;
    bool charging = false;
};

class IPower {
public:
    virtual ~IPower() = default;
    virtual PowerInfo Current() const = 0;
};

} // namespace bps::platform
