#include "platform/linux/LinuxEnvironment.hpp"
#include "platform/OsTag.hpp"

#include <pwd.h>
#include <sys/utsname.h>
#include <unistd.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <thread>

namespace bps::platform {

namespace {
std::string FirstLine(std::string_view path) {
    std::ifstream in{std::string(path)};
    std::string line;
    if (!std::getline(in, line)) return {};
    return line;
}

std::string PrettyOsName() {
    std::ifstream os("/etc/os-release");
    std::string line;
    while (std::getline(os, line)) {
        if (line.rfind("PRETTY_NAME=", 0) != 0) continue;
        std::string v = line.substr(12);
        if (v.size() >= 2 && v.front() == '"' && v.back() == '"') v = v.substr(1, v.size() - 2);
        return v;
    }
    return "Linux";
}

std::string CpuModel() {
    std::ifstream cpuinfo("/proc/cpuinfo");
    std::string line;
    while (std::getline(cpuinfo, line)) {
        if (line.rfind("model name", 0) != 0) continue;
        size_t colon = line.find(':');
        if (colon == std::string::npos) continue;
        std::string v = line.substr(colon + 1);
        // strip leading whitespace
        size_t start = v.find_first_not_of(" \t");
        if (start == std::string::npos) return {};
        return v.substr(start);
    }
    return {};
}

std::string GpuName() {
    // NVIDIA exposes a "Model:" line per GPU.
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator("/proc/driver/nvidia/gpus", ec)) {
        std::ifstream info(entry.path() / "information");
        std::string line;
        while (std::getline(info, line)) {
            if (line.rfind("Model", 0) != 0) continue;
            size_t colon = line.find(':');
            if (colon == std::string::npos) break;
            std::string v = line.substr(colon + 1);
            size_t start = v.find_first_not_of(" \t");
            return start == std::string::npos ? v : v.substr(start);
        }
    }
    // AMD/Intel: map the DRM device vendor id to a family name (best effort).
    for (int card = 0; card < 8; ++card) {
        const std::string base = "/sys/class/drm/card" + std::to_string(card) + "/device";
        std::string vendor = FirstLine(base + "/vendor");
        if (vendor == "0x1002") return "AMD Radeon (amdgpu)";
        if (vendor == "0x10de") return "NVIDIA GPU";
        if (vendor == "0x8086") return "Intel Graphics";
    }
    return {};
}

std::string LangToTag(const char* lang) {
    if (!lang || !*lang) return {};
    std::string l = lang;
    size_t dot = l.find('.');
    if (dot != std::string::npos) l = l.substr(0, dot);
    if (l.size() >= 5 && l[2] == '_') l[2] = '-';
    return l;
}

std::string Timezone() {
    std::error_code ec;
    auto target = std::filesystem::read_symlink("/etc/localtime", ec);
    if (!ec) {
        std::string t = target.string();
        size_t pos = t.find("zoneinfo/");
        if (pos != std::string::npos) return t.substr(pos + 9);
    }
    if (const char* tz = std::getenv("TZ"); tz) return tz;
    return {};
}

uint64_t TotalRamBytes() {
    std::ifstream meminfo("/proc/meminfo");
    std::string line;
    while (std::getline(meminfo, line))
        if (line.rfind("MemTotal:", 0) == 0)
            return std::stoull(line.substr(9)) * 1024ull;
    return 0;
}
} // namespace

EnvironmentInfo LinuxEnvironment::Current() const {
    EnvironmentInfo info;
    struct utsname u {};
    if (uname(&u) == 0) {
        info.osVersion = u.release;
        info.arch = CanonicalArch(u.machine);   // "aarch64" -> "arm64", "armv7l" -> "arm", ...
        info.buildNumber = u.version;
    }
    info.osName = PrettyOsName();
    info.cpuModel = CpuModel();
    info.gpuName = GpuName();
    info.totalRamBytes = TotalRamBytes();
    info.coreCount = std::thread::hardware_concurrency();

    char host[256] = {0};
    if (gethostname(host, sizeof(host) - 1) == 0) info.hostname = host;

    if (const char* user = std::getenv("USER"); user) {
        info.username = user;
    } else if (struct passwd* pw = getpwuid(getuid()); pw) {
        info.username = pw->pw_name;
    }

    info.locale = LangToTag(std::getenv("LANG"));
    info.timezone = Timezone();
    return info;
}

} // namespace bps::platform
