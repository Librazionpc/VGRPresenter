#pragma once

#include "IPlatform.hpp"

#include <memory>

namespace bps::platform {
class PlatformAccessor {
public:
    // Returns the active backend; lazily creates it on first use. Never null
    // on a supported OS (throws std::logic_error when no backend exists).
    static IPlatform& Get();

    // Raw shared_ptr (null before Install on unsupported builds).
    static std::shared_ptr<IPlatform> Shared();

    // Install/replace the backend. Called by the Kernel at boot and by tests.
    static void Install(std::shared_ptr<IPlatform> platform);

    static bool Installed();
};
} // namespace bps::platform