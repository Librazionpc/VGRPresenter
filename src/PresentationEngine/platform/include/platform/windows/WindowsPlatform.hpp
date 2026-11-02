#pragma once

#include "platform/PlatformAccessors.hpp"
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

    Snapshot Sample() override;
    std::vector<OsEvent> PollChanges() override;

    uint64_t ThreadCount();
    static constexpr int kThreadCountRefreshMs = 5000;

    void DeviceFingerprints(std::string& audioFp, std::string& videoFp);
    static constexpr int kDeviceFingerprintRefreshMs = 5000;

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