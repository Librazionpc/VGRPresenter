#pragma once

#include "../IPlatform.hpp"
#include "../common/Filesystem.hpp"
#include "../common/Timer.hpp"
#include "LinuxPaths.hpp"
#include "LinuxThreading.hpp"
#include "LinuxProcess.hpp"
#include "LinuxLibrary.hpp"
#include "LinuxMonitor.hpp"
#include "LinuxAudio.hpp"
#include "LinuxVideo.hpp"
#include "LinuxNetwork.hpp"
#include "LinuxPower.hpp"
#include "LinuxClipboard.hpp"
#include "LinuxEnvironment.hpp"
#include "LinuxLocale.hpp"
#include "LinuxInput.hpp"
#include "LinuxDialogs.hpp"
#include "LinuxNotifications.hpp"
#include "../common/PosixSocket.hpp"

#include <optional>
#include <string>

namespace bps::platform {

// Linux backend of the PAL (docs/architecture/PAL.md): the only place on this
// platform that reads /proc, /sys, and calls libc/pthread directly.
class LinuxPlatform final : public IPlatform {
public:
    const char* Name() const noexcept override { return "LinuxPlatform"; }

    SystemInfo Info() override;
    std::string OsName() const override;
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
    LinuxPaths paths_;
    TimerImpl timer_;
    LinuxThreading threading_;
    LinuxProcess process_;
    LinuxLibrary library_;
    LinuxMonitor monitor_;
    LinuxAudio audio_;
    LinuxVideo video_;
    LinuxNetwork network_;
    LinuxPower power_;
    LinuxClipboard clipboard_;
    LinuxEnvironment environment_;
    LinuxLocale locale_;
    LinuxInput input_;
    LinuxDialogs dialogs_;
    LinuxNotifications notifications_;
    PosixSocketFactory sockets_;

    std::string lastMonitorIds_;              // for hot-plug change detection
    std::optional<PowerInfo> lastPower_;      // for power-change events
    std::string lastResolutions_;             // for resolution-change events
    std::string lastAudioFp_;                 // for audio hot-plug events
    std::string lastVideoFp_;                 // for video hot-plug events
};

} // namespace bps::platform
