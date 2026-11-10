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

// One alpha-preserving RGBA frame shared by every NDI sender fed by the same rendered buffer.
struct NdiRgbaFrame {
    int width = 0;
    int height = 0;
    std::vector<uint8_t> bytes;   // RGBA8, width*height*4
};

// Copy Color::Pack RGBA pixels to a shared frame, preserving template alpha.
std::shared_ptr<const NdiRgbaFrame> MakeNdiRgbaFrame(const uint32_t* pixels, int width, int height);

// Convert into an EXISTING frame, reusing its bytes. The feed keeps a couple of slots per frame buffer and
// refills a slot only when nothing is still reading it, so the steady state allocates nothing per frame
// (a fresh 4 MB buffer every tick is malloc churn plus a full memset of the frame it is about to overwrite).
void ConvertToNdiRgba(const uint32_t* pixels, int width, int height, NdiRgbaFrame& target);

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

    // The single-frame entry: converts and sends (kept for callers that hold no shared frame).
    Result<void> SendFrame(const RenderFrameView& frame);
    // Send a frame already copied to RGBA, preserving alpha for transparent template backgrounds.
    // The provider keeps `frame` alive until it hands the runtime the NEXT one — that is the async send's
    // own contract, and why the shared frame is a shared_ptr rather than a borrowed pointer.
    Result<void> SendRgbaFrame(std::shared_ptr<const NdiRgbaFrame> frame, float fps);

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
    // The last converted frame handed to an async send. It stays ALIVE here until the next one replaces
    // it: the runtime reads its bytes until the next call for this instance (the frame is shared, not
    // owned — see SendRgbaFrame).
    std::shared_ptr<const NdiRgbaFrame> lastShared_;
    // ---- PER-SECOND SEND TELEMETRY (SendFrame) -------------------------------------------
    // What this sender is actually putting on the wire, summed over the current one-second window
    // (sendMutex_ guards all three). Logged as one line per second so a bandwidth/lag report can be
    // answered from engine.log instead of guessed at.
    std::chrono::steady_clock::time_point sendWindowStart_{};
    uint64_t sendWindowFrames_ = 0;
    uint64_t sendWindowBytes_ = 0;
};
} // namespace bps::display
