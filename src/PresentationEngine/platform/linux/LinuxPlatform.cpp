#include "platform/linux/LinuxPlatform.hpp"
#include "platform/OsTag.hpp"

#include <pwd.h>
#include <sys/statvfs.h>
#include <sys/utsname.h>
#include <unistd.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include <thread>

namespace bps::platform {

std::string LinuxPlatform::OsName() const {
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

std::string LinuxPlatform::OsVersion() const {
    struct utsname u {};
    if (uname(&u) == 0) return u.release;
    return "unknown";
}

std::string LinuxPlatform::Arch() const {
    struct utsname u {};
    if (uname(&u) == 0) return CanonicalArch(u.machine);   // canonical naming
    return "unknown";
}

SystemInfo LinuxPlatform::Info() {
    // Single source of truth for environment reads is the IEnvironment backend;
    // Info() is a thin projection so the two cannot drift (PAL DoD §25).
    const EnvironmentInfo e = environment_.Current();
    SystemInfo info;
    info.osName = e.osName;
    info.osVersion = e.osVersion;
    info.arch = e.arch;
    info.buildNumber = e.buildNumber;
    info.hostname = e.hostname;
    info.username = e.username;
    info.language = e.locale;
    info.timezone = e.timezone;
    info.coreCount = e.coreCount;
    info.totalRamBytes = e.totalRamBytes;
    return info;
}

Snapshot LinuxPlatform::Sample() {
    Snapshot s;
    s.coreCount = std::thread::hardware_concurrency();

    // --- RAM (/proc/meminfo) ---
    std::ifstream meminfo("/proc/meminfo");
    std::string line;
    while (std::getline(meminfo, line)) {
        if (line.rfind("MemTotal:", 0) == 0)
            s.totalRamBytes = std::stoull(line.substr(9)) * 1024ull;
        else if (line.rfind("MemAvailable:", 0) == 0)
            s.availableRamBytes = std::stoull(line.substr(13)) * 1024ull;
    }

    // --- CPU: cumulative jiffies (/proc/stat); consumers compute deltas ---
    std::ifstream stat("/proc/stat");
    if (std::getline(stat, line) && line.rfind("cpu ", 0) == 0) {
        uint64_t user = 0, nice = 0, system = 0, idle = 0, iowait = 0, irq = 0, softirq = 0, steal = 0;
        std::istringstream is(line);
        std::string tag;
        is >> tag >> user >> nice >> system >> idle >> iowait >> irq >> softirq >> steal;
        s.cpuTotalJiffies = user + nice + system + idle + iowait + irq + softirq + steal;
        s.cpuIdleJiffies = idle + iowait;
    }

    // --- Disk (/) via statvfs ---
    struct statvfs vfs {};
    if (statvfs("/", &vfs) == 0) {
        s.diskFreeBytes = static_cast<uint64_t>(vfs.f_bavail) * vfs.f_frsize;
        s.diskTotalBytes = static_cast<uint64_t>(vfs.f_blocks) * vfs.f_frsize;
    }

    // --- Network: cumulative rx/tx bytes (/proc/net/dev) ---
    uint64_t rx = 0, tx = 0;
    std::ifstream dev("/proc/net/dev");
    std::string l;
    bool first = true;
    while (std::getline(dev, l)) {
        if (first) { first = false; continue; }   // header
        size_t colon = l.find(':');
        if (colon == std::string::npos) continue;
        std::istringstream is(l.substr(colon + 1));
        uint64_t r = 0, t = 0, drop = 0;
        is >> r >> drop;                          // rx bytes, rx packets
        for (int i = 0; i < 6; ++i) { uint64_t v; is >> v; }
        is >> t;                                  // tx bytes
        rx += r;
        tx += t;
    }
    s.networkRxBytes = rx;
    s.networkTxBytes = tx;

    // --- Thread count (/proc/self/status) ---
    std::ifstream status("/proc/self/status");
    while (std::getline(status, line)) {
        if (line.rfind("Threads:", 0) == 0) {
            s.threadCount = std::stoull(line.substr(8));
            break;
        }
    }

    // --- Battery (best effort; absent on desktops) ---
    for (int i = 0; i < 4; ++i) {
        const std::string base = "/sys/class/power_supply/BAT" + std::to_string(i);
        std::ifstream cap(base + "/capacity");
        if (!cap.is_open()) continue;
        cap >> s.batteryPercent;
        std::ifstream st(base + "/status");
        std::string statusStr;
        st >> statusStr;
        s.onBattery = (statusStr == "Discharging");
        break;
    }

    // --- GPU / VRAM (10 §3, best effort) ---
    for (int card = 0; card < 8; ++card) {
        const std::string base = "/sys/class/drm/card" + std::to_string(card) + "/device";
        std::ifstream total(base + "/mem_info_vram_total");
        if (!total.is_open()) continue;
        std::ifstream used(base + "/mem_info_vram_used");
        uint64_t t = 0, u = 0;
        total >> t;
        if (used.is_open()) used >> u;
        s.gpuVramTotalBytes += t;
        s.gpuVramUsedBytes += u;
    }
    if (s.gpuVramTotalBytes == 0) {
        std::error_code ec;
        for (const auto& entry : std::filesystem::directory_iterator("/proc/driver/nvidia/gpus", ec)) {
            std::ifstream info(entry.path() / "information");
            std::string l;
            while (std::getline(info, l)) {
                if (l.rfind("Video Memory", 0) != 0) continue;
                size_t colon = l.find(':');
                if (colon == std::string::npos) break;
                std::istringstream is(l.substr(colon + 1));
                uint64_t mb = 0;
                is >> mb;   // "8192 MiB" -> 8192
                s.gpuVramTotalBytes += mb * 1024ull * 1024ull;
                break;
            }
        }
    }

    return s;
}

std::vector<OsEvent> LinuxPlatform::PollChanges() {
    std::vector<OsEvent> events;

    // --- Monitor hot-plug detection ---
    std::set<std::string> ids;
    for (const auto& m : monitor_.Enumerate()) ids.insert(m.id);
    std::string joined;
    for (const auto& id : ids) joined += id + ";";
    if (!lastMonitorIds_.empty() && joined != lastMonitorIds_) {
        std::set<std::string> prev;
        std::stringstream ss(lastMonitorIds_);
        std::string id;
        while (std::getline(ss, id, ';'))
            if (!id.empty()) prev.insert(id);
        for (const auto& id : ids)
            if (!prev.count(id)) events.push_back({OsEventType::MonitorConnected, id});
        for (const auto& id : prev)
            if (!ids.count(id)) events.push_back({OsEventType::MonitorDisconnected, id});
    }
    lastMonitorIds_ = joined;

    // --- Power state changes ---
    PowerInfo p = power_.Current();
    if (lastPower_.has_value()) {
        const auto& prev = *lastPower_;
        if (prev.onBattery != p.onBattery || prev.charging != p.charging ||
            prev.batteryPercent != p.batteryPercent) {
            events.push_back({OsEventType::PowerChanged,
                              std::string(p.onBattery ? "on battery" : "on AC") +
                                  " (" + std::to_string(p.batteryPercent) + "%)"});
        }
        // Battery-low crossing (<= 20% while discharging) — emitted once per
        // crossing, not continuously.
        if (p.onBattery && p.batteryPercent >= 0 && p.batteryPercent <= 20 &&
            !(prev.onBattery && prev.batteryPercent <= 20)) {
            events.push_back({OsEventType::BatteryLow,
                              std::to_string(p.batteryPercent) + "%"});
        }
    }
    lastPower_ = p;

    // --- Monitor resolution changes (hot-plug is detected above) ---
    std::string resJoined;
    for (const auto& m : monitor_.Enumerate())
        resJoined += m.id + "=" + std::to_string(m.widthPx) + "x" + std::to_string(m.heightPx) + ";";
    if (!lastResolutions_.empty() && resJoined != lastResolutions_) {
        for (const auto& m : monitor_.Enumerate())
            if (resJoined.find(m.id + "=") != std::string::npos &&
                lastResolutions_.find(m.id + "=" + std::to_string(m.widthPx) + "x" +
                                      std::to_string(m.heightPx)) == std::string::npos)
                events.push_back({OsEventType::ResolutionChanged,
                                  m.id + " " + std::to_string(m.widthPx) + "x" +
                                      std::to_string(m.heightPx)});
    }
    lastResolutions_ = resJoined;

    // --- Audio device hot-plug (DoD §13) ---
    std::string audioFp = audio_.Fingerprint();
    if (!lastAudioFp_.empty() && audioFp != lastAudioFp_) {
        events.push_back({OsEventType::DeviceConnected,
                          "audio device configuration changed"});
    }
    lastAudioFp_ = audioFp;

    return events;
}

} // namespace bps::platform
