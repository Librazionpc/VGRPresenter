#pragma once

// Linux PAL backend for the monitor subsystem. Enumerates DRM connectors via
// sysfs (/sys/class/drm) — no rendering, no windows.

#include "../IMonitor.hpp"

namespace bps::platform {

class LinuxMonitor final : public IMonitor {
public:
    std::vector<MonitorInfo> Enumerate() const override;
    Result<MonitorInfo> Primary() const override;
    std::string DefaultMonitorId() const override;
};

} // namespace bps::platform
