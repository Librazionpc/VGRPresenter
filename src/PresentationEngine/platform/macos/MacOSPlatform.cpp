#include "platform/macos/MacOSPlatform.hpp"
#include "platform/OsTag.hpp"

namespace bps::platform {

std::string MacOSPlatform::OsVersion() const { return "unknown"; }
std::string MacOSPlatform::Arch() const { return CompileArch(); }   // canonical, like the other backends

SystemInfo MacOSPlatform::Info() {
    SystemInfo info;
    info.osName = "macOS";
    info.arch = CompileArch();
    return info;
}

Snapshot MacOSPlatform::Sample() {
    Snapshot s;
    s.coreCount = 1;
    return s;
}

} // namespace bps::platform
