#pragma once

// Windows PAL backend for the monitor subsystem (EnumDisplayMonitors +
// GetMonitorInfoEx + EnumDisplaySettingsEx; orientation via QueryDisplayConfig).

#include "platform/IMonitor.hpp"

namespace bps::platform {

class WindowsMonitor final : public IMonitor {
public:
    std::vector<MonitorInfo> Enumerate() const override;
    Result<MonitorInfo> Primary() const override;
    std::string DefaultMonitorId() const override;
};

} // namespace bps::platform
