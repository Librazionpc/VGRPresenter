#include "platform/IPlatform.hpp"

#if defined(__linux__)
#include "platform/linux/LinuxPlatform.hpp"
#elif defined(_WIN32)
#include "platform/windows/WindowsPlatform.hpp"
#elif defined(__APPLE__)
#include "platform/macos/MacOSPlatform.hpp"
#endif

namespace bps::platform {

// The only platform-specific symbol the core links. Only the backend matching
// the active build is compiled (SystemArchitecture.md §3.6, PAL.md).
std::unique_ptr<IPlatform> CreatePlatform() {
#if defined(__linux__)
    return std::make_unique<LinuxPlatform>();
#elif defined(_WIN32)
    return std::make_unique<WindowsPlatform>();
#elif defined(__APPLE__)
    return std::make_unique<MacOSPlatform>();
#else
    return nullptr;   // platform not yet available on this OS
#endif
}

} // namespace bps::platform
