#pragma once

// Global PAL accessor (Phase 2). The Kernel installs the backend at boot
// (PlatformAccessor::Install(CreatePlatform())); until then (and in unit tests
// that do not install one) a lazily-created default backend is used so engine
// systems can route through the PAL unconditionally.
//
// Managers that historically called OS APIs directly (PluginManager,
// ModuleManager, DatabaseManager, ThreadPool) resolve the platform through this
// accessor instead of threading a dependency through every constructor.

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
