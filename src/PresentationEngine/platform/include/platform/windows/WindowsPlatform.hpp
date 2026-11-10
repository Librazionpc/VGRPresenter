#pragma once

#include "platform/PlatformAccessors.hpp"
#include "platform/PlatformServices.hpp"
#include "platform/common/Timer.hpp"
#include "platform/windows/WindowsThreading.hpp"
#include "platform/windows/WindowsProcess.hpp"
#include "platform/windows/WindowsMonitor.hpp"
#include "platform/windows/WindowsAudio.hpp"
#include "platform/windows/WindowsVideo.hpp"
#include "platform/windows/WindowsNetwork.hpp"
#include "platform/windows/WindowsPower.hpp"
#include "platform/qt/QtClipboard.hpp"
#include "platform/qt/QtComposed.hpp"
#include "platform/windows/WindowsEnvironment.hpp"
#include "platform/windows/WindowsSocket.hpp"
#include "platform/windows/WindowsInput.hpp"
#include "platform/windows/WindowsDialogs.hpp"
#include "platform/windows/WindowsNotifications.hpp"

#include <chrono>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>

namespace bps::platform {
class WindowsPlatform final : public PlatformAccessors<WindowsPlatform> {
    friend class PlatformAccessors<WindowsPlatform>;

public:
    const char* Name() const noexcept override { return "WindowsPlatform"; }

    SystemInfo Info() override;
    std::string OsName() const override { return "Windows"; }
    std::string OsVersion() const override;
    std::string Arch() const override;
    PlatformCapabilities Capabilities() const override;

    Snapshot Sample() override;
    std::vector<OsEvent> PollChanges() override;

    uint64_t ThreadCount();
    static constexpr int kThreadCountRefreshMs = 5000;

    void DeviceFingerprints(std::string& audioFp, std::string& videoFp);
    static constexpr int kDeviceFingerprintRefreshMs = 5000;

private:
    // THE PLATFORM SERVICES (docs/platform/qt_first_platform.md): Qt serves
    // all five — ONE implementation on every OS. The native threading adapter
    // remains only for what Qt has no API for (naming the calling thread,
    // affinity, priority); no per-OS paths/locale/library class exists any more.
    PlatformPaths paths_;
    WindowsThreading nativeThreading_;
    PlatformThreading threading_{nativeThreading_};
    PlatformFilesystem filesystem_;
    PlatformLibrary library_;
    PlatformLocale locale_;
    TimerImpl timer_;
    WindowsProcess process_;
    WindowsMonitor monitor_;
    WindowsAudio audio_;
    WindowsVideo video_;
    WindowsNetwork nativeNetwork_;
    QtNetwork network_{nativeNetwork_};
    WindowsPower power_;
    QtClipboard clipboard_;   // QClipboard (same class on every OS)
    WindowsEnvironment nativeEnvironment_;
    QtEnvironment environment_{nativeEnvironment_};
    WindowsInput nativeInput_;
    QtInput input_{nativeInput_};
    WindowsDialogs dialogs_;
    WindowsNotifications nativeNotifications_;
    QtNotifications notifications_{nativeNotifications_};
    WindowsSocketFactory sockets_;

    std::string lastMonitorIds_;
    std::optional<PowerInfo> lastPower_;
    // The cached thread count (Sample() runs on more than one sampler thread).
    std::mutex threadCountMutex_;
    uint64_t threadCount_ = 0;
    bool threadCountRead_ = false;
    std::chrono::steady_clock::time_point threadCountAt_{};
    // The cached device fingerprints (PollChanges is drained by the Kernel's
    // scheduler thread; the lock is for a second caller, as with the thread count).
    std::mutex deviceFpMutex_;
    std::string audioFpCache_;
    std::string videoFpCache_;
    bool deviceFpRead_ = false;
    std::chrono::steady_clock::time_point deviceFpAt_{};
    std::string lastResolutions_;
    // The last fingerprints REPORTED to the caller (only a walked change moves
    // these), which is what makes a cached walk report nothing between refreshes.
    std::string lastAudioFp_;
    std::string lastVideoFp_;
};
} // namespace bps::platform
