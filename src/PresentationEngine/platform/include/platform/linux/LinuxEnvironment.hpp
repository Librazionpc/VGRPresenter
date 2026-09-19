#pragma once

// Linux PAL backend for the environment subsystem (DoD §14): /proc, /sys and
// libc reads only — never used outside the PAL.

#include "../IEnvironment.hpp"

namespace bps::platform {

class LinuxEnvironment final : public IEnvironment {
public:
    EnvironmentInfo Current() const override;
};

} // namespace bps::platform
