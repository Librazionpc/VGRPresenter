#pragma once

// PAL monitor subsystem (Phase 2): display *devices* only — never rendering and
// never window creation (the frontend owns windows per the Phase 2 design
// note). Provides enumeration, geometry, refresh rate, DPI and hot-plug change
// detection via the platform's PollChanges().

#include "core/common/Common.hpp"

#include <string>
#include <vector>

namespace bps::platform {

struct MonitorInfo {
    std::string id;            // stable id, e.g. "eDP-1", "\\.\DISPLAY1"
    std::string name;          // friendly name (best effort)
    int x = 0;                 // virtual-desktop origin
    int y = 0;
    int widthPx = 0;
    int heightPx = 0;
    int refreshRateHz = 0;     // 0 = unknown
    int dpi = 96;
    int orientation = 0;       // degrees: 0 / 90 / 180 / 270 (best effort)
    bool primary = false;
    bool connected = true;
    bool hdrSupported = false; // best effort
};

class IMonitor {
public:
    virtual ~IMonitor() = default;

    // All monitors currently connected (empty vector on unsupported systems).
    virtual std::vector<MonitorInfo> Enumerate() const = 0;

    // The primary monitor, or an Error when none is found.
    virtual Result<MonitorInfo> Primary() const = 0;

    // Stable id of the primary monitor ("" when unknown).
    virtual std::string DefaultMonitorId() const = 0;
};

} // namespace bps::platform
