#include "platform/linux/LinuxPaths.hpp"

#include <cstdlib>
#include <string>
#include <unistd.h>
#include <limits.h>

namespace bps::platform {

namespace {

std::string Home() {
    const char* h = std::getenv("HOME");
    return h ? h : "/tmp";
}

std::string Xdg(std::string_view env, std::string_view fallback) {
    const char* v = std::getenv(std::string(env).c_str());
    if (v && *v) return v;
    return std::string(fallback);
}

std::string ParentOf(std::string_view path) {
    size_t pos = path.find_last_of('/');
    if (pos == std::string::npos || pos == 0) return "/";
    return std::string(path.substr(0, pos));
}
} // namespace

std::string LinuxPaths::ExecutableDir() const {
    char buf[PATH_MAX] = {0};
    ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n > 0) return ParentOf(std::string_view(buf, static_cast<size_t>(n)));
    return ".";
}

std::string LinuxPaths::CurrentWorkingDir() const {
    char buf[PATH_MAX] = {0};
    return getcwd(buf, sizeof(buf)) ? buf : ".";
}

std::string LinuxPaths::ConfigDir() const {
    return Xdg("XDG_CONFIG_HOME", Home() + "/.config") + "/bps";
}

std::string LinuxPaths::CacheDir() const {
    return Xdg("XDG_CACHE_HOME", Home() + "/.cache") + "/bps";
}

std::string LinuxPaths::PluginDir() const {
    return ExecutableDir() + "/../lib/bps/plugins";
}

std::string LinuxPaths::AssetDir() const {
    return ExecutableDir() + "/../share/bps/assets";
}

std::string LinuxPaths::LogDir() const {
    return Xdg("XDG_STATE_HOME", Home() + "/.local/state") + "/bps/logs";
}

std::string LinuxPaths::TempDir() const {
    const char* t = std::getenv("TMPDIR");
    return (t && *t) ? t : "/tmp";
}

std::string LinuxPaths::DownloadsDir() const { return Home() + "/Downloads"; }
std::string LinuxPaths::DocumentsDir() const { return Home() + "/Documents"; }
std::string LinuxPaths::DesktopDir() const { return Home() + "/Desktop"; }

std::string LinuxPaths::UserDataDir() const {
    return Xdg("XDG_DATA_HOME", Home() + "/.local/share") + "/bps";
}

std::string LinuxPaths::AppDataDir() const {
    // XDG has no separate "roaming app data" concept; reuse the data home.
    return UserDataDir();
}

} // namespace bps::platform
