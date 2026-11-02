#pragma once

#include "../PlatformAccessors.hpp"
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
class LinuxPlatform final : public PlatformAccessors<LinuxPlatform> {
    friend class PlatformAccessors<LinuxPlatform>;

public:
    const char* Name() const noexcept override { return "LinuxPlatform"; }

    SystemInfo Info() override;
    std::string OsName() const override;
    std::string OsVersion() const override;
    std::string Arch() const override;

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
