#pragma once

#include "core/common/Common.hpp"
#include "modules/display/IDisplayProvider.hpp"

#include <atomic>
#include <chrono>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace bps::display {
// A rendering frame (RGBA8) forwarded to the NDI provider for output.
struct RenderFrameView {
    int width = 0;
    int height = 0;
    const uint32_t* pixels = nullptr;   // Color::Pack format (0xAABBGGRR)
};

inline constexpr float kMaxOutputFrameRateHz = 240.0f;
inline constexpr float kDefaultOutputFrameRateHz = 30.0f;

inline float AcceptedOutputFrameRate(float requestedHz)
{
    return (requestedHz > 0.0f && requestedHz <= kMaxOutputFrameRateHz)
        ? requestedHz : kDefaultOutputFrameRateHz;
}

// --- Null provider: no displays. The engine still works (18 §Display Independence).
class NullDisplayProvider final : public IDisplayProvider {
public:
    const char* Name() const noexcept override { return "Null"; }
    const char* Version() const noexcept override { return "1.0"; }
    std::vector<DisplayDevice> Enumerate() const override { return {}; }
    Result<std::vector<DisplayDevice>> Probe() override { return std::vector<DisplayDevice>{}; }
    DisplayProviderCapabilities Capabilities() const noexcept override {
        DisplayProviderCapabilities c;
        c.supportsHotPlug = false;
        c.supportsVirtualOutputs = false;
        return c;
    }
};

// --- Virtual provider: software displays (preview / headless / testing).
// Hot-plug is simulated through AddVirtualDevice / RemoveVirtualDevice.
class VirtualDisplayProvider final : public IDisplayProvider {
public:
    VirtualDisplayProvider();

    const char* Name() const noexcept override { return "Virtual"; }
    const char* Version() const noexcept override { return "1.0"; }

    std::vector<DisplayDevice> Enumerate() const override;
    Result<std::vector<DisplayDevice>> Probe() override;   // detects add/remove
    DisplayProviderCapabilities Capabilities() const noexcept override;

    Result<void> AddVirtualDevice(const DisplayDevice& device);
    Result<void> RemoveVirtualDevice(std::string_view id);
    Result<DisplayDevice> GetDevice(std::string_view id) const;

private:
    mutable std::mutex mutex_;
    std::vector<DisplayDevice> devices_;
    std::map<std::string, DisplayDevice, std::less<>> lastSeen_;
};

class PhysicalDisplayProvider final : public IDisplayProvider {
public:
    PhysicalDisplayProvider() = default;

    const char* Name() const noexcept override { return "Linux"; }
    const char* Version() const noexcept override { return "1.0"; }

    std::vector<DisplayDevice> Enumerate() const override;
    Result<std::vector<DisplayDevice>> Probe() override;
    DisplayProviderCapabilities Capabilities() const noexcept override;

private:
    // Convert PAL MonitorInfo -> DisplayDevice (also used by Probe).
    static std::vector<DisplayDevice> FromPlatform();
    mutable std::map<std::string, DisplayDevice, std::less<>> lastSeen_;
};

class NdiDisplayProvider final : public IDisplayProvider {
public:
    NdiDisplayProvider();

    const char* Name() const noexcept override { return "Ndi"; }
    const char* Version() const noexcept override { return "1.0"; }

    std::vector<DisplayDevice> Enumerate() const override;
    Result<std::vector<DisplayDevice>> Probe() override;   // static device set
    DisplayProviderCapabilities Capabilities() const noexcept override;

    Result<void> SendFrame(const RenderFrameView& frame);

    // Sender name used for the NDI source ("VGR Program" by default).
    void SetSenderName(std::string name);
    void SetFrameRate(float fps);
    // The rate SetFrameRate last accepted (after clamping) — the advertised
    // fps, for tests and telemetry.
    float FrameRate() const;
    void ResetSender();
    std::string SenderId() const;
    std::string SenderName() const;
    uint64_t FramesSent() const;

private:
    mutable std::mutex mutex_;
    mutable std::mutex sendMutex_;
    std::vector<DisplayDevice> devices_;
    std::string senderName_ = "VGR Program";
    float frameRate_ = 30.0f;       // advertised fps (SetFrameRate; legacy default)
    std::string senderId_;          // BroadcastEngine sender id (lazily created)
    std::atomic<uint64_t> framesSent_{0};
    int sendFailStreak_ = 0;
    std::chrono::steady_clock::time_point lastHeal_{};
    std::vector<uint32_t> sendBuffer_;
};
} // namespace bps::display