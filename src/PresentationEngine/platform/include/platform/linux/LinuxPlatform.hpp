#pragma once

#include "../PlatformAccessors.hpp"
#include "../PlatformServices.hpp"
#include "../common/Timer.hpp"
#include "LinuxThreading.hpp"
#include "LinuxProcess.hpp"
#include "LinuxMonitor.hpp"
#include "platform/qt/QtMediaDevices.hpp"
#include "LinuxVideo.hpp"
#include "LinuxNetwork.hpp"
#include "LinuxPower.hpp"
#include "platform/qt/QtClipboard.hpp"
#include "platform/qt/QtComposed.hpp"
#include "LinuxEnvironment.hpp"
#include "LinuxInput.hpp"
#include "LinuxDialogs.hpp"
#include "platform/qt/QtDialogs.hpp"
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
    PlatformCapabilities Capabilities() const override;

    Snapshot Sample() override;
    std::vector<OsEvent> PollChanges() override;

private:
    // THE PLATFORM SERVICES (docs/platform/qt_first_platform.md): Qt serves
    // all five — ONE implementation on every OS. The native threading adapter
    // remains only for what Qt has no API for (naming, affinity, priority).
    PlatformPaths paths_;
    LinuxThreading nativeThreading_;
    PlatformThreading threading_{nativeThreading_};
    PlatformFilesystem filesystem_;
    PlatformLibrary library_;
    PlatformLocale locale_;
    TimerImpl timer_;
    LinuxProcess process_;
    LinuxMonitor monitor_;
    QtAudioDevices audio_;
    LinuxVideo video_;
    LinuxNetwork nativeNetwork_;
    QtNetwork network_{nativeNetwork_};
    LinuxPower power_;
    QtClipboard clipboard_;   // QClipboard on the GUI thread (was xclip / wl-paste command lines)
    LinuxEnvironment nativeEnvironment_;
    QtEnvironment environment_{nativeEnvironment_};
    LinuxInput nativeInput_;
    QtInput input_{nativeInput_};
    // File pickers: Qt's platform-theme dialogs first (nothing to install), zenity/kdialog as the fallback.
    LinuxDialogs nativeDialogs_;
    QtDialogs dialogs_{&nativeDialogs_, [this] { return nativeDialogs_.Supported(); }};
    LinuxNotifications nativeNotifications_;
    QtNotifications notifications_{nativeNotifications_};
    PosixSocketFactory sockets_;

    std::string lastMonitorIds_;              // for hot-plug change detection
    std::optional<PowerInfo> lastPower_;      // for power-change events
    std::string lastResolutions_;             // for resolution-change events
    std::string lastAudioFp_;                 // for audio hot-plug events
    std::string lastVideoFp_;                 // for video hot-plug events
};

} // namespace bps::platform
