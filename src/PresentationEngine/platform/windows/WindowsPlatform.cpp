#include "platform/windows/WindowsPlatform.hpp"
#include "platform/windows/WinUtil.hpp"

#include <windows.h>
#include <iphlpapi.h>
#include <netioapi.h>
#include <pdh.h>
// PDH_MORE_DATA (the "buffer too small" result PdhGetFormattedCounterArrayW's
// size probe returns) lives in pdhmsg.h — pdh.h alone does not define it.
#include <pdhmsg.h>
#include <tlhelp32.h>

#include <mutex>
#include <set>
#include <sstream>
#include <vector>

namespace bps::platform {

namespace {

// ---------------------------------------------------------------------------
// Live health counters (PDH)
// ---------------------------------------------------------------------------
// The three "how is this machine doing right now" numbers the Settings health
// surface shows: GPU engine utilization, the CPU's actual speed as a percent of
// its nominal clock (% Processor Performance — below 100 means the part is
// being held under nominal, i.e. power/thermal throttling), and a best-effort
// package temperature from the ACPI thermal zones. PDH is Windows' documented
// performance-counter API; nothing here is required for the engine to run, so
// any failure just leaves the field at its -1 "not measured" default instead of
// failing the Sample() the ResourceManager also depends on.
class HealthCounters {
public:
    struct Values {
        double gpuPct = -1.0;
        double cpuPerfPct = -1.0;
        double cpuTempC = -1.0;
    };

    Values Read()
    {
        Values v;
        std::lock_guard<std::mutex> lock(mutex_);
        if (!Ensure())
            return v;
        if (PdhCollectQueryData(query_) != ERROR_SUCCESS)
            return v;
        if (gpu_) {
            // Several engine instances share one GPU; their sum can pass 100
            // when engines run concurrently. Left at -1 when no instance
            // reported (the honest "not measured").
            const double gpu = Sum(gpu_);
            if (gpu >= 0.0)
                v.gpuPct = Clamp(gpu, 0.0, 100.0);
        }
        if (perf_)
            v.cpuPerfPct = Single(perf_);
        if (temp_) {
            const double kelvin = Max(temp_);
            // ACPI reports Kelvin; anything outside a plausible range is noise.
            if (kelvin > 200.0 && kelvin < 400.0)
                v.cpuTempC = kelvin - 273.15;
        }
        return v;
    }

private:
    bool Ensure()
    {
        if (ready_)
            return true;
        if (failed_)
            return false;
        if (PdhOpenQueryW(nullptr, 0, &query_) != ERROR_SUCCESS) {
            failed_ = true;
            return false;
        }
        // English counter names, regardless of the UI language.
        gpu_ = Add(L"\\GPU Engine(*)\\Utilization Percentage");
        perf_ = Add(L"\\Processor Information(_Total)\\% Processor Performance");
        temp_ = Add(L"\\Thermal Zone Information(*)\\Temperature");
        if (!gpu_ && !perf_ && !temp_) {
            PdhCloseQuery(query_);
            query_ = nullptr;
            failed_ = true;
            return false;
        }
        PdhCollectQueryData(query_);   // baseline for the rate counters
        ready_ = true;
        return true;
    }

    PDH_HCOUNTER Add(const wchar_t *path)
    {
        PDH_HCOUNTER h = nullptr;
        return PdhAddEnglishCounterW(query_, path, 0, &h) == ERROR_SUCCESS ? h : nullptr;
    }

    static double Clamp(double v, double lo, double hi) { return v < lo ? lo : (v > hi ? hi : v); }

    // A wildcard counter's instances (GPU Engine: one per active engine;
    // thermal zones: one per zone).
    template <typename Fn>
    void ForEach(PDH_HCOUNTER counter, Fn fn)
    {
        DWORD size = 0;
        DWORD type = 0;
        if (PdhGetFormattedCounterArrayW(counter, PDH_FMT_DOUBLE, &size, &type, nullptr) != PDH_MORE_DATA || size == 0)
            return;
        std::vector<BYTE> buf(size);
        auto *items = reinterpret_cast<PDH_FMT_COUNTERVALUE_ITEM_W *>(buf.data());
        if (PdhGetFormattedCounterArrayW(counter, PDH_FMT_DOUBLE, &size, &type, items) != ERROR_SUCCESS)
            return;
        const size_t count = size / sizeof(PDH_FMT_COUNTERVALUE_ITEM_W);
        for (size_t i = 0; i < count; ++i)
            if (items[i].FmtValue.CStatus == ERROR_SUCCESS)
                fn(items[i].FmtValue.doubleValue);
    }

    double Sum(PDH_HCOUNTER counter)
    {
        double total = 0.0;
        bool any = false;
        ForEach(counter, [&](double x) { total += x; any = true; });
        return any ? total : -1.0;
    }

    double Max(PDH_HCOUNTER counter)
    {
        double best = -1.0;
        bool any = false;
        ForEach(counter, [&](double x) { if (!any || x > best) best = x; any = true; });
        return any ? best : -1.0;
    }

    // A counter with no instances (e.g. the _Total processor one).
    double Single(PDH_HCOUNTER counter)
    {
        PDH_FMT_COUNTERVALUE v{};
        if (PdhGetFormattedCounterValue(counter, PDH_FMT_DOUBLE, nullptr, &v) != ERROR_SUCCESS)
            return -1.0;
        return v.CStatus == ERROR_SUCCESS ? v.doubleValue : -1.0;
    }

    std::mutex mutex_;
    PDH_HQUERY query_ = nullptr;
    PDH_HCOUNTER gpu_ = nullptr;
    PDH_HCOUNTER perf_ = nullptr;
    PDH_HCOUNTER temp_ = nullptr;
    bool ready_ = false;
    bool failed_ = false;
};

HealthCounters &Health()
{
    static HealthCounters counters;
    return counters;
}

} // namespace

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

    // The main graphics adapter's own video memory, from DXGI (asked once; see WindowsGpu.cpp).
    s.gpuVramTotalBytes = win::PrimaryGpu().vramBytes;

    // Live health (GPU %, CPU speed vs nominal, package temperature) from PDH.
    const HealthCounters::Values health = Health().Read();
    s.gpuPct = health.gpuPct;
    s.cpuPerfPct = health.cpuPerfPct;
    s.cpuTempC = health.cpuTempC;
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

    // Video-capture hot-plug: same fingerprint convention as audio — the
    // MF device roster's ids change when a camera/capture card plugs in.
    std::string videoFp = video_.Fingerprint();
    if (!lastVideoFp_.empty() && videoFp != lastVideoFp_) {
        events.push_back({OsEventType::DeviceConnected,
                          "video device configuration changed"});
    }
    lastVideoFp_ = videoFp;

    return events;
}

} // namespace bps::platform
