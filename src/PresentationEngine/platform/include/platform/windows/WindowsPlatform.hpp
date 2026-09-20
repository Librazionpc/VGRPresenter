#pragma once

// Windows backend of the PAL (docs/architecture/PAL.md): the only place on
// Windows that calls Win32 directly. Uses the common backends for filesystem
// (std::filesystem) and timer (std::chrono).

#include "platform/IPlatform.hpp"
#include "platform/common/Filesystem.hpp"
#include "platform/common/Timer.hpp"
#include "platform/windows/WindowsPaths.hpp"
#include "platform/windows/WindowsThreading.hpp"
#include "platform/windows/WindowsProcess.hpp"
#include "platform/windows/WindowsLibrary.hpp"
#include "platform/windows/WindowsMonitor.hpp"
#include "platform/windows/WindowsAudio.hpp"
#include "platform/windows/WindowsVideo.hpp"
#include "platform/windows/WindowsNetwork.hpp"
#include "platform/windows/WindowsPower.hpp"
#include "platform/windows/WindowsClipboard.hpp"
#include "platform/windows/WindowsEnvironment.hpp"
#include "platform/windows/WindowsLocale.hpp"
#include "platform/windows/WindowsSocket.hpp"
#include "platform/windows/WindowsInput.hpp"
#include "platform/windows/WindowsDialogs.hpp"
#include "platform/windows/WindowsNotifications.hpp"

#include <optional>
#include <string>

namespace bps::platform {

class WindowsPlatform final : public IPlatform {
public:
    const char* Name() const noexcept override { return "WindowsPlatform"; }

    SystemInfo Info() override;
    std::string OsName() const override { return "Windows"; }
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
    std::vector<OsEvent> PollChanges() override;

private:
    FilesystemImpl filesystem_;
    WindowsPaths paths_;
    TimerImpl timer_;
    WindowsThreading threading_;
    WindowsProcess process_;
    WindowsLibrary library_;
    WindowsMonitor monitor_;
    WindowsAudio audio_;
    WindowsVideo video_;
    WindowsNetwork network_;
    WindowsPower power_;
    WindowsClipboard clipboard_;
    WindowsEnvironment environment_;
    WindowsLocale locale_;
    WindowsInput input_;
    WindowsDialogs dialogs_;
    WindowsNotifications notifications_;
    WindowsSocketFactory sockets_;

    std::string lastMonitorIds_;
    std::optional<PowerInfo> lastPower_;
    std::string lastResolutions_;
    std::string lastAudioFp_;
    std::string lastVideoFp_;
};

} // namespace bps::platform
