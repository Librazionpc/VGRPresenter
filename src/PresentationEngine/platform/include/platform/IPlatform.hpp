#pragma once

// Platform Abstraction Layer (PAL) — the only place OS-specific code lives
// (docs/architecture/SystemArchitecture.md §3.6 and docs/architecture/PAL.md).
// The core consumes this facade; platform/linux, platform/windows,
// platform/macos provide backends. No feature module may include OS headers or
// call OS APIs directly — everything goes through one of the subsystems below.

#include "core/common/Common.hpp"

#include "IFilesystem.hpp"
#include "IPaths.hpp"
#include "ITimer.hpp"
#include "IThreading.hpp"
#include "IProcess.hpp"
#include "ILibrary.hpp"
#include "IMonitor.hpp"
#include "IAudio.hpp"
#include "IVideo.hpp"
#include "INetwork.hpp"
#include "IPower.hpp"
#include "IClipboard.hpp"
#include "IEnvironment.hpp"
#include "ILocale.hpp"
#include "ISocket.hpp"
#include "IInput.hpp"
#include "IDialogs.hpp"
#include "INotifications.hpp"

#include <memory>
#include <string>
#include <vector>

namespace bps::platform {

// ---------------------------------------------------------------------------
// Telemetry snapshot — absolute machine counters sampled by the backend.
// Consumers (e.g. ResourceManager) compute deltas for rates (CPU %, bps).
// ---------------------------------------------------------------------------
struct Snapshot {
    uint64_t totalRamBytes = 0;
    uint64_t availableRamBytes = 0;
    uint64_t cpuTotalJiffies = 0;   // cumulative across all cores
    uint64_t cpuIdleJiffies = 0;
    uint64_t diskFreeBytes = 0;
    uint64_t diskTotalBytes = 0;
    uint64_t networkRxBytes = 0;    // cumulative
    uint64_t networkTxBytes = 0;    // cumulative
    uint64_t threadCount = 0;       // live threads in this process
    int batteryPercent = -1;        // -1 = no battery
    bool onBattery = false;
    unsigned coreCount = 0;
    uint64_t gpuVramTotalBytes = 0; // best effort; 0 = no GPU backend reported
    uint64_t gpuVramUsedBytes = 0;
};

// ---------------------------------------------------------------------------
// Rich system info (§Core Platform Class)
// ---------------------------------------------------------------------------
struct SystemInfo {
    std::string osName;      // "Linux", "Windows", "macOS"
    std::string osVersion;   // kernel / build version
    std::string arch;        // "x86_64", "aarch64", ...
    std::string buildNumber; // OS build number ("" when unknown)
    std::string hostname;
    std::string username;
    std::string language;    // e.g. "en-US"
    std::string timezone;    // e.g. "UTC", "America/New_York"
    unsigned coreCount = 0;
    uint64_t totalRamBytes = 0;
};

// ---------------------------------------------------------------------------
// OS events detected by PollChanges() — the Kernel drains these into the Event
// Bus (topics: platform.monitor_connected, platform.power_changed, ...).
// ---------------------------------------------------------------------------
enum class OsEventType : int {
    MonitorConnected,
    MonitorDisconnected,
    ResolutionChanged,
    PowerChanged,
    BatteryLow,
    Sleep,
    Wake,
    NetworkChanged,
    DeviceConnected,
    DeviceRemoved,
    LocaleChanged,
    ClipboardChanged,
};

inline const char* ToString(OsEventType t) {
    switch (t) {
        case OsEventType::MonitorConnected:    return "MonitorConnected";
        case OsEventType::MonitorDisconnected: return "MonitorDisconnected";
        case OsEventType::ResolutionChanged:   return "ResolutionChanged";
        case OsEventType::PowerChanged:        return "PowerChanged";
        case OsEventType::BatteryLow:          return "BatteryLow";
        case OsEventType::Sleep:               return "Sleep";
        case OsEventType::Wake:                return "Wake";
        case OsEventType::NetworkChanged:      return "NetworkChanged";
        case OsEventType::DeviceConnected:     return "DeviceConnected";
        case OsEventType::DeviceRemoved:       return "DeviceRemoved";
        case OsEventType::LocaleChanged:       return "LocaleChanged";
        case OsEventType::ClipboardChanged:    return "ClipboardChanged";
    }
    return "Unknown";
}

struct OsEvent {
    OsEventType type = OsEventType::MonitorConnected;
    std::string detail; // e.g. monitor id, "on battery", device name
};

// ---------------------------------------------------------------------------
// The PAL facade
// ---------------------------------------------------------------------------
class IPlatform {
public:
    virtual ~IPlatform() = default;
    virtual const char* Name() const noexcept = 0;

    // Core platform class (§Core Platform Class)
    virtual SystemInfo Info() = 0;
    virtual std::string OsName() const = 0;
    virtual std::string OsVersion() const = 0;
    virtual std::string Arch() const = 0;

    // Subsystem accessors — never null on a supported OS.
    virtual IFilesystem& Filesystem() = 0;
    virtual IPaths& Paths() = 0;
    virtual ITimer& Timer() = 0;
    virtual IThreading& Threading() = 0;
    virtual IProcess& Process() = 0;
    virtual ILibrary& Library() = 0;
    virtual IMonitor& Monitor() = 0;
    virtual IAudio& Audio() = 0;
    // Video-capture discovery (cameras/capture cards + their real mode
    // lists). Same discovery-only scope as Audio(); streaming/capture I/O
    // belongs to the future capture feature module.
    virtual IVideo& Video() = 0;
    virtual INetwork& Network() = 0;
    virtual IPower& Power() = 0;
    virtual IClipboard& Clipboard() = 0;
    virtual IEnvironment& Environment() = 0;
    virtual ILocale& Locale() = 0;

    // TCP socket transport used by the IPC layer (core/ipc) and future
    // transports. Never null on a supported OS.
    virtual ISocketFactory& Sockets() = 0;

    // Input device discovery, native dialogs, desktop notifications. Never
    // null on a supported OS (methods may return Unsupported on headless
    // hosts / when a provider tool is missing).
    virtual IInput& Input() = 0;
    virtual IDialogs& Dialogs() = 0;
    virtual INotifications& Notifications() = 0;

    // Telemetry for the ResourceManager (never throws; fills what it can).
    virtual Snapshot Sample() = 0;

    // OS events detected since the last poll. Called periodically by the
    // Kernel's platform watcher; returns {} where change detection is
    // unsupported.
    virtual std::vector<OsEvent> PollChanges() { return {}; }
};

// Platform factory — the only platform-specific symbol the core links.
std::unique_ptr<IPlatform> CreatePlatform();

} // namespace bps::platform
