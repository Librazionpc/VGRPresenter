#pragma once

// macOS backend of the PAL. Structure is in place so `CreatePlatform()` links;
// the subsystem implementations are pending (require a macOS host to develop
// and verify — Conformance §15.24). Common backends (filesystem, timer) are
// already OS-agnostic and would be reused as-is.

#include "platform/IPlatform.hpp"
#include "platform/common/Filesystem.hpp"
#include "platform/common/Timer.hpp"

namespace bps::platform {

class MacOSPlatform final : public IPlatform {
public:
    const char* Name() const noexcept override { return "MacOSPlatform"; }

    SystemInfo Info() override;
    std::string OsName() const override { return "macOS"; }
    std::string OsVersion() const override;
    std::string Arch() const override;

    IFilesystem& Filesystem() override { return filesystem_; }
    IPaths& Paths() override { return paths_; }
    ITimer& Timer() override { return timer_; }
    IThreading& Threading() override { return threading_; }
    IProcess& Process() override { return process_; }
    ILibrary& Library() override { return library_; }
    IMonitor& Monitor() override { return monitor_; }
    IAudio& Audio() override { return audio_; }
    IVideo& Video() override { return video_; }
    INetwork& Network() override { return network_; }
    IPower& Power() override { return power_; }
    IClipboard& Clipboard() override { return clipboard_; }
    IEnvironment& Environment() override { return environment_; }
    ILocale& Locale() override { return locale_; }
    ISocketFactory& Sockets() override { return sockets_; }
    IInput& Input() override { return input_; }
    IDialogs& Dialogs() override { return dialogs_; }
    INotifications& Notifications() override { return notifications_; }

    Snapshot Sample() override;

private:
    // Every subsystem below returns Err::Unsupported until the macOS backend
    // is implemented (see PAL.md §11). Only the OS-agnostic common backends are
    // functional out of the box.
    FilesystemImpl filesystem_;
    TimerImpl timer_;

    struct StubPaths final : IPaths {
        std::string Unsupported() const { return {}; }
        std::string ExecutableDir() const override { return Unsupported(); }
        std::string CurrentWorkingDir() const override { return Unsupported(); }
        std::string ConfigDir() const override { return Unsupported(); }
        std::string CacheDir() const override { return Unsupported(); }
        std::string PluginDir() const override { return Unsupported(); }
        std::string AssetDir() const override { return Unsupported(); }
        std::string LogDir() const override { return Unsupported(); }
        std::string TempDir() const override { return Unsupported(); }
        std::string DownloadsDir() const override { return Unsupported(); }
        std::string DocumentsDir() const override { return Unsupported(); }
        std::string DesktopDir() const override { return Unsupported(); }
        std::string UserDataDir() const override { return Unsupported(); }
        std::string AppDataDir() const override { return Unsupported(); }
    };
    struct StubThreading final : IThreading {
        unsigned HardwareConcurrency() const override { return 1; }
        Result<void> SetCurrentThreadName(std::string_view) override { return Ok(); }
        std::string CurrentThreadName() const override { return {}; }
        uint64_t CurrentThreadId() const override { return 0; }
        Result<void> SetThreadAffinity(std::thread&, unsigned) override {
            return Error::Make(Err::Unsupported, "Threading", "macOS backend pending");
        }
        Result<void> SetCurrentThreadPriority(int) override {
            return Error::Make(Err::Unsupported, "Threading", "macOS backend pending");
        }
    };
    struct StubProcess final : IProcess {
        Result<int> Start(std::string_view, std::vector<std::string>) override {
            return Error::Make(Err::Unsupported, "Process", "macOS backend pending");
        }
        Result<void> Wait(int, int) override {
            return Error::Make(Err::Unsupported, "Process", "macOS backend pending");
        }
        Result<int> ExitCode(int) const override {
            return Error::Make(Err::Unsupported, "Process", "macOS backend pending");
        }
        Result<void> Kill(int) override {
            return Error::Make(Err::Unsupported, "Process", "macOS backend pending");
        }
        Result<void> Terminate(int) override {
            return Error::Make(Err::Unsupported, "Process", "macOS backend pending");
        }
        Result<bool> IsRunning(int) const override { return false; }
        int CurrentProcessId() const override { return 0; }
        Result<int> Restart(int, std::string_view, std::vector<std::string>) override {
            return Error::Make(Err::Unsupported, "Process", "macOS backend pending");
        }
        Result<std::string> Environment(std::string_view) const override {
            return Error::Make(Err::Unsupported, "Process", "macOS backend pending");
        }
        Result<void> SetEnvironment(std::string_view, std::string_view) override {
            return Error::Make(Err::Unsupported, "Process", "macOS backend pending");
        }
    };
    struct StubLibrary final : ILibrary {
        Result<Handle> Load(std::string_view) override {
            return Error::Make(Err::Unsupported, "Library", "macOS backend pending");
        }
        Result<void> Unload(Handle) override {
            return Error::Make(Err::Unsupported, "Library", "macOS backend pending");
        }
        Result<void*> Symbol(Handle, std::string_view) override {
            return Error::Make(Err::Unsupported, "Library", "macOS backend pending");
        }
        bool IsLoaded(Handle) const override { return false; }
        std::string LastError() const override { return "macOS backend pending"; }
        Result<Handle> Reload(Handle, std::string_view) override {
            return Error::Make(Err::Unsupported, "Library", "macOS backend pending");
        }
    };
    struct StubMonitor final : IMonitor {
        std::vector<MonitorInfo> Enumerate() const override { return {}; }
        Result<MonitorInfo> Primary() const override {
            return Error::Make(Err::Unsupported, "Monitor", "macOS backend pending");
        }
        std::string DefaultMonitorId() const override { return {}; }
    };
    struct StubAudio final : IAudio {
        std::vector<AudioDeviceInfo> Enumerate() const override { return {}; }
        Result<AudioDeviceInfo> DefaultOutput() const override {
            return Error::Make(Err::Unsupported, "Audio", "macOS backend pending");
        }
        Result<AudioDeviceInfo> DefaultInput() const override {
            return Error::Make(Err::Unsupported, "Audio", "macOS backend pending");
        }
        std::string Fingerprint() const override { return {}; }
    };
    struct StubVideo final : IVideo {
        std::vector<VideoDeviceInfo> Enumerate() const override { return {}; }
        std::string Fingerprint() const override { return {}; }
    };
    struct StubNetwork final : INetwork {
        std::string Hostname() const override { return {}; }
        std::vector<NetworkAdapterInfo> Adapters() const override { return {}; }
        Result<std::string> IpAddress() const override {
            return Error::Make(Err::Unsupported, "Network", "macOS backend pending");
        }
        Result<bool> InternetAvailable() const override { return false; }
        Result<std::string> Gateway() const override {
            return Error::Make(Err::Unsupported, "Network", "macOS backend pending");
        }
        std::vector<std::string> DnsServers() const override { return {}; }
        std::string Proxy() const override { return {}; }
        FirewallAccess ProbeInboundAccess(const std::string&, const std::string&) const override {
            return FirewallAccess::Unavailable;
        }
        FirewallRequestOutcome RequestInboundAccess(const std::string&, const std::string&,
                                                     const std::string&, std::string*) override {
            return FirewallRequestOutcome::Unavailable;
        }
    };
    struct StubPower final : IPower {
        PowerInfo Current() const override { return {}; }
    };
    struct StubClipboard final : IClipboard {
        Result<std::string> ReadText() const override {
            return Error::Make(Err::Unsupported, "Clipboard", "macOS backend pending");
        }
        Result<void> WriteText(std::string_view) override {
            return Error::Make(Err::Unsupported, "Clipboard", "macOS backend pending");
        }
        Result<std::vector<std::string>> GetFiles() const override {
            return Error::Make(Err::Unsupported, "Clipboard", "macOS backend pending");
        }
        Result<void> SetFiles(const std::vector<std::string>&) override {
            return Error::Make(Err::Unsupported, "Clipboard", "macOS backend pending");
        }
    };
    struct StubEnvironment final : IEnvironment {
        EnvironmentInfo Current() const override { return {}; }
    };
    struct StubLocale final : ILocale {
        LocaleInfo Current() const override { return {}; }
        std::string Language() const override { return {}; }
        std::string Country() const override { return {}; }
    };
    struct StubInput final : IInput {
        std::vector<InputDeviceInfo> Enumerate() const override { return {}; }
        bool HasKeyboard() const override { return false; }
        bool HasPointer() const override { return false; }
    };
    struct StubDialogs final : IDialogs {
        Result<std::optional<std::string>> OpenFileDialog(std::string_view, const std::vector<std::string>&) override {
            return Error::Make(Err::Unsupported, "Dialogs", "macOS backend pending");
        }
        Result<std::optional<std::string>> SaveFileDialog(std::string_view, std::string_view) override {
            return Error::Make(Err::Unsupported, "Dialogs", "macOS backend pending");
        }
        Result<std::optional<std::string>> SelectFolderDialog(std::string_view) override {
            return Error::Make(Err::Unsupported, "Dialogs", "macOS backend pending");
        }
        Result<void> MessageDialog(std::string_view, std::string_view) override {
            return Error::Make(Err::Unsupported, "Dialogs", "macOS backend pending");
        }
        Result<bool> QuestionDialog(std::string_view, std::string_view) override {
            return Error::Make(Err::Unsupported, "Dialogs", "macOS backend pending");
        }
    };
    struct StubNotifications final : INotifications {
        bool Supported() const override { return false; }
        Result<void> Show(std::string_view, std::string_view) override {
            return Error::Make(Err::Unsupported, "Notifications", "macOS backend pending");
        }
    };
    struct StubSocket final : ISocketFactory {
        Result<std::unique_ptr<ISocket>> Connect(std::string, uint16_t) override {
            return Error::Make(Err::Unsupported, "Socket", "macOS backend pending");
        }
        Result<std::unique_ptr<ISocketListener>> Listen(uint16_t) override {
            return Error::Make(Err::Unsupported, "Socket", "macOS backend pending");
        }
    };

    StubPaths paths_;
    StubThreading threading_;
    StubProcess process_;
    StubLibrary library_;
    StubMonitor monitor_;
    StubAudio audio_;
    StubVideo video_;
    StubNetwork network_;
    StubPower power_;
    StubClipboard clipboard_;
    StubEnvironment environment_;
    StubLocale locale_;
    StubInput input_;
    StubDialogs dialogs_;
    StubNotifications notifications_;
    StubSocket sockets_;
};

} // namespace bps::platform
