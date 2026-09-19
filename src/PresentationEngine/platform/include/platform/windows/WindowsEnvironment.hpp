#pragma once

// Windows PAL backend for the environment subsystem (registry + Win32 system
// info reads).

#include "platform/IEnvironment.hpp"

namespace bps::platform {

class WindowsEnvironment final : public IEnvironment {
public:
    EnvironmentInfo Current() const override;
};

} // namespace bps::platform
