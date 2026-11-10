#pragma once

#include "platform/PlatformAccessors.hpp"
#include "platform/PlatformServices.hpp"
#include "platform/common/PosixProcess.hpp"
#include "platform/common/PosixSocket.hpp"
#include "platform/common/Timer.hpp"
#include "platform/qt/QtClipboard.hpp"
#include "platform/qt/QtComposed.hpp"
#include "platform/qt/QtMediaDevices.hpp"

#include <optional>

namespace bps::platform {

class MacOSThreading final : public IThreading {
public:
    unsigned HardwareConcurrency() const override;
    Result<void> SetCurrentThreadName(std::string_view name) override;
    std::string CurrentThreadName() const override;
    uint64_t CurrentThreadId() const override;
    Result<void> SetThreadAffinity(std::thread& thread, unsigned cpu) override;
    Result<void> SetCurrentThreadPriority(int priority) override;
};

class MacOSMonitor final : public IMonitor {
public:
    std::vector<MonitorInfo> Enumerate() const override;
    Result<MonitorInfo> Primary() const override;
    std::string DefaultMonitorId() const override;
};

class MacOSNetwork final : public INetwork {
public:
    std::string Hostname() const override;
    std::vector<NetworkAdapterInfo> Adapters() const override;
    Result<std::string> IpAddress() const override;
    Result<bool> InternetAvailable() const override;
    Result<std::string> Gateway() const override;
    std::vector<std::string> DnsServers() const override;
    std::string Proxy() const override;
    FirewallAccess ProbeInboundAccess(const std::string&, const std::string&) const override;
    FirewallRequestOutcome RequestInboundAccess(const std::string&, const std::string&,
                                                const std::string&, std::string*) override;
};

class MacOSPower final : public IPower {
public:
    PowerInfo Current() const override;
};

class MacOSEnvironment final : public IEnvironment {
public:
    EnvironmentInfo Current() const override;
};

class MacOSInput final : public IInput {
public:
    std::vector<InputDeviceInfo> Enumerate() const override;
    bool HasKeyboard() const override;
    bool HasPointer() const override;
};

class MacOSDialogs final : public IDialogs {
public:
    Result<std::optional<std::string>> OpenFileDialog(
        std::string_view title, const std::vector<std::string>& filters) override;
    Result<std::optional<std::string>> SaveFileDialog(
        std::string_view title, std::string_view defaultName) override;
    Result<std::optional<std::string>> SelectFolderDialog(std::string_view title) override;
    Result<void> MessageDialog(std::string_view title, std::string_view message) override;
    Result<bool> QuestionDialog(std::string_view title, std::string_view message) override;
};

class MacOSNotifications final : public INotifications {
public:
    bool Supported() const override { return true; }
    Result<void> Show(std::string_view title, std::string_view body) override;
};

class MacOSPlatform final : public PlatformAccessors<MacOSPlatform> {
    friend class PlatformAccessors<MacOSPlatform>;
public:
    const char* Name() const noexcept override { return "MacOSPlatform"; }
    SystemInfo Info() override;
    std::string OsName() const override { return "macOS"; }
    std::string OsVersion() const override;
    std::string Arch() const override;
    PlatformCapabilities Capabilities() const override;
    Snapshot Sample() override;
    std::vector<OsEvent> PollChanges() override;

private:
    PlatformPaths paths_;
    MacOSThreading nativeThreading_;
    PlatformThreading threading_{nativeThreading_};
    PlatformFilesystem filesystem_;
    PlatformLibrary library_;
    PlatformLocale locale_;
    TimerImpl timer_;
    PosixProcess process_;
    MacOSMonitor monitor_;
    QtAudioDevices audio_;
    QtVideoDevices video_;
    MacOSNetwork nativeNetwork_;
    QtNetwork network_{nativeNetwork_};
    MacOSPower power_;
    QtClipboard clipboard_;   // QClipboard (same class on every OS)
    MacOSEnvironment nativeEnvironment_;
    QtEnvironment environment_{nativeEnvironment_};
    PosixSocketFactory sockets_;
    MacOSInput nativeInput_;
    QtInput input_{nativeInput_};
    MacOSDialogs dialogs_;
    MacOSNotifications nativeNotifications_;
    QtNotifications notifications_{nativeNotifications_};

    std::string lastMonitorIds_;
    std::string lastResolutions_;
    std::string lastNetworkFingerprint_;
    std::string lastAudioFingerprint_;
    std::string lastVideoFingerprint_;
    std::optional<PowerInfo> lastPower_;
    bool havePollBaseline_ = false;
};

} // namespace bps::platform
