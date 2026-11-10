#include "platform/IPlatform.hpp"

#if defined(__linux__)
#include "platform/linux/LinuxPlatform.hpp"
#elif defined(_WIN32)
#include "platform/windows/WindowsPlatform.hpp"
#elif defined(__APPLE__)
#include <TargetConditionals.h>
#if TARGET_OS_OSX
#define BPS_PLATFORM_MACOS 1
#include "platform/macos/MacOSPlatform.hpp"
#else
#error "VGR Presenter PAL supports macOS, not other Apple platforms"
#endif
#else
#error "VGR Presenter PAL supports only Windows, Linux, and macOS"
#endif

namespace bps::platform {

// The only platform-specific symbol the core links. Only the backend matching
// the active build is compiled (SystemArchitecture.md §3.6, PAL.md).
std::unique_ptr<IPlatform> CreatePlatform() {
#if defined(__linux__)
    return std::make_unique<LinuxPlatform>();
#elif defined(_WIN32)
    return std::make_unique<WindowsPlatform>();
#elif defined(BPS_PLATFORM_MACOS)
    return std::make_unique<MacOSPlatform>();
#endif
}

} // namespace bps::platform
