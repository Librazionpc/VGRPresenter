#include "platform/windows/WindowsPlatform.hpp"
#include "platform/windows/WinUtil.hpp"

#include <windows.h>
#include <iphlpapi.h>
#include <netioapi.h>
#include <tlhelp32.h>

#include <set>
#include <sstream>

namespace bps::platform {

std::string WindowsPlatform::OsVersion() const { return environment_.Current().osVersion; }

std::string WindowsPlatform::Arch() const { return environment_.Current().arch; }

SystemInfo WindowsPlatform::Info() {
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

Snapshot WindowsPlatform::Sample() {
    Snapshot s;

    // CPU: GetSystemTimes returns idle/kernel/user as FILETIME (kernel includes
    // idle). Total = idle + (kernel - idle) + user.
    FILETIME idle{}, kernel{}, user{};
    if (GetSystemTimes(&idle, &kernel, &user)) {
        auto asU64 = [](const FILETIME& f) {
            return (static_cast<uint64_t>(f.dwHighDateTime) << 32) | f.dwLowDateTime;
        };
        uint64_t i = asU64(idle), k = asU64(kernel), u = asU64(user);
        s.cpuTotalJiffies = i + (k - i) + u;
        s.cpuIdleJiffies = i;
    }

    // RAM.
    MEMORYSTATUSEX ms{};
    ms.dwLength = sizeof(ms);
    if (GlobalMemoryStatusEx(&ms)) {
        s.totalRamBytes = ms.ullTotalPhys;
        s.availableRamBytes = ms.ullAvailPhys;
    }

    // Disk (C: drive as the primary volume).
    ULARGE_INTEGER freeBytes{}, totalBytes{};
    if (GetDiskFreeSpaceExA("C:\\", &freeBytes, &totalBytes, nullptr)) {
        s.diskFreeBytes = freeBytes.QuadPart;
        s.diskTotalBytes = totalBytes.QuadPart;
    }

    // Network: cumulative octets from GetIfTable2 (Vista+).
    MIB_IF_TABLE2* table = nullptr;
    if (GetIfTable2(&table) == NO_ERROR) {
        for (ULONG i = 0; i < table->NumEntries; ++i) {
            s.networkRxBytes += table->Table[i].InOctets;
            s.networkTxBytes += table->Table[i].OutOctets;
        }
        FreeMibTable(table);
    }

    // Battery.
    SYSTEM_POWER_STATUS sp{};
    if (GetSystemPowerStatus(&sp)) {
        if (sp.BatteryFlag != 128 && sp.BatteryFlag != 255) {
            s.batteryPercent = sp.BatteryLifePercent <= 100 ? sp.BatteryLifePercent : -1;
            s.onBattery = (sp.ACLineStatus == 0);
        }
    }

    SYSTEM_INFO si{};
    GetNativeSystemInfo(&si);
    s.coreCount = si.dwNumberOfProcessors > 0 ? si.dwNumberOfProcessors : 1;

    // Live thread count for this process — Toolhelp32 is fully documented
    // (unlike NtQuerySystemInformation), just needed enumerating instead of
    // a single API call the way Linux's /proc/self/status one-liner reads.
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap != INVALID_HANDLE_VALUE) {
        THREADENTRY32 te{};
        te.dwSize = sizeof(te);
        const DWORD pid = GetCurrentProcessId();
        uint64_t count = 0;
        if (Thread32First(snap, &te)) {
            do {
                if (te.th32OwnerProcessID == pid) ++count;
            } while (Thread32Next(snap, &te));
        }
        s.threadCount = count;
        CloseHandle(snap);
    }

    // gpuVram: not available without DXGI, a documented follow-up (PAL.md §11).
    return s;
}

std::vector<OsEvent> WindowsPlatform::PollChanges() {
    std::vector<OsEvent> events;

    // Monitor hot-plug + resolution changes.
    std::string ids, res;
    for (const auto& m : monitor_.Enumerate()) {
        ids += m.id + ";";
        res += m.id + "=" + std::to_string(m.widthPx) + "x" + std::to_string(m.heightPx) + ";";
    }
    if (!lastMonitorIds_.empty() && ids != lastMonitorIds_) {
        std::set<std::string> prev, cur;
        auto split = [](const std::string& s, std::set<std::string>& out) {
            std::stringstream ss(s);
            std::string id;
            while (std::getline(ss, id, ';'))
                if (!id.empty()) out.insert(id);
        };
        split(lastMonitorIds_, prev);
        split(ids, cur);
        for (const auto& id : cur)
            if (!prev.count(id)) events.push_back({OsEventType::MonitorConnected, id});
        for (const auto& id : prev)
            if (!cur.count(id)) events.push_back({OsEventType::MonitorDisconnected, id});
    }
    lastMonitorIds_ = ids;
    if (!lastResolutions_.empty() && res != lastResolutions_)
        events.push_back({OsEventType::ResolutionChanged, "display configuration changed"});
    lastResolutions_ = res;

    // Power changes + battery-low crossing.
    PowerInfo p = power_.Current();
    if (lastPower_.has_value()) {
        const auto& prev = *lastPower_;
        if (prev.onBattery != p.onBattery || prev.charging != p.charging ||
            prev.batteryPercent != p.batteryPercent)
            events.push_back({OsEventType::PowerChanged,
                              std::string(p.onBattery ? "on battery" : "on AC") + " (" +
                                  std::to_string(p.batteryPercent) + "%)"});
        if (p.onBattery && p.batteryPercent >= 0 && p.batteryPercent <= 20 &&
            !(prev.onBattery && prev.batteryPercent <= 20))
            events.push_back({OsEventType::BatteryLow, std::to_string(p.batteryPercent) + "%"});
    }
    lastPower_ = p;

    // Audio device hot-plug (DoD §13): winmm IDs change when devices are
    // added/removed.
    std::string audioFp = audio_.Fingerprint();
    if (!lastAudioFp_.empty() && audioFp != lastAudioFp_) {
        events.push_back({OsEventType::DeviceConnected,
                          "audio device configuration changed"});
    }
    lastAudioFp_ = audioFp;

    return events;
}

} // namespace bps::platform
