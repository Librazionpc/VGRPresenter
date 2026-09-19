#pragma once

// Linux PAL backend for the power subsystem (/sys/class/power_supply).

#include "../IPower.hpp"

namespace bps::platform {

class LinuxPower final : public IPower {
public:
    PowerInfo Current() const override;
};

} // namespace bps::platform
