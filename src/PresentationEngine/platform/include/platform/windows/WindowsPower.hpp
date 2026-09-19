#pragma once

// Windows PAL backend for the power subsystem (GetSystemPowerStatus).

#include "platform/IPower.hpp"

namespace bps::platform {

class WindowsPower final : public IPower {
public:
    PowerInfo Current() const override;
};

} // namespace bps::platform
