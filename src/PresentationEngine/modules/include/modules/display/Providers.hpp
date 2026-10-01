#pragma once

// Concrete display providers (docs/specs/18). Every provider implements
// IDisplayProvider; the Display Engine never knows them. Null = headless (the
// engine runs with zero displays). Virtual = software displays for previews /
// tests / headless operation. Linux = physical monitors through the PAL.
// Ndi = broadcast output through the Phase 17 BroadcastEngine (falls back to
// the software loopback when the NDI SDK is absent, so it is testable on any
// host).

#include "core/common/Common.hpp"
#include "modules/display/IDisplayProvider.hpp"

#include <atomic>
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

// --- Physical provider: real monitors via the PAL IMonitor subsystem
// (cross-platform by construction — the PAL carries the per-OS backends;
// the registered name string "Linux" is legacy-only, kept so stored
// profiles/event payloads that name the provider keep matching).
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

// --- NDI provider: renders frames out over the BroadcastEngine (docs/specs/29).
// Enumerates one virtual "NDI Program" device. SendFrame() pushes the engine's
// native RGBA8 frame through BroadcastEngine::SendVideoFrame UNCONVERTED (the
// layout is byte-for-byte NDI's 'RGBA' fourCC — the SDK owns color); when
// the NDI SDK is not installed the engine's software loopback carries it, so
// the whole path is exercised in CI. Frame delivery itself stays with the
// caller (the sink in DisplayEngine::RouteFrame) — this provider is the
// *device*, exactly like the physical provider.
class NdiDisplayProvider final : public IDisplayProvider {
public:
    NdiDisplayProvider();

    const char* Name() const noexcept override { return "Ndi"; }
    const char* Version() const noexcept override { return "1.0"; }

    std::vector<DisplayDevice> Enumerate() const override;
    Result<std::vector<DisplayDevice>> Probe() override;   // static device set
    DisplayProviderCapabilities Capabilities() const noexcept override;

    // Sends one native RGBA frame over the BroadcastEngine (no conversion —
    // see the class comment). Creates the NDI sender on first use; errors
    // (unconfigured engine) are reported.
    Result<void> SendFrame(const RenderFrameView& frame);

    // Sender name used for the NDI source ("VGR Program" by default).
    void SetSenderName(std::string name);
    // The frame rate ADVERTISED in every sent frame's NDI metadata. The
    // default (30) matches the legacy hardcoded value; the live feed's
    // REAL cadence is its poll timer (LiveOutputService's 100 ms tick →
    // 10 fps), and receivers key their smoothing/clock on this number —
    // advertising 30 for a 10 fps feed made Studio Monitor's frame clock
    // run 3× fast. Set from the actual loop that calls SendFrame.
    void SetFrameRate(float fps);
    // Tears the live sender down (SendFrame recreates it from senderName_ on
    // its next call). NDI's SDK cannot rename an already-created sender, so
    // a per-session reset is the only way a SetSenderName change — or the
    // same name on a NEW live session — reaches the network. No-op when no
    // sender exists yet.
    void ResetSender();
    // The engine sender id ("" until the first frame creates it lazily) —
    // callers query BroadcastEngine::SenderConnectedReceivers with it to
    // learn whether ANY monitor actually connected (the firewall signal).
    std::string SenderId() const;
    std::string SenderName() const;
    uint64_t FramesSent() const;

private:
    mutable std::mutex mutex_;
    std::vector<DisplayDevice> devices_;
    std::string senderName_ = "VGR Program";
    float frameRate_ = 30.0f;       // advertised fps (SetFrameRate; legacy default)
    std::string senderId_;          // BroadcastEngine sender id (lazily created)
    std::atomic<uint64_t> framesSent_{0};
    // THE SEND BUFFER, OWNED BY THE PROVIDER — the crash fix for "GO LIVE
    // crashes the app" (heap corruption 0xc0000374 in ntdll). NDI's contract:
    // a frame's buffer must stay valid from its send UNTIL THE NEXT SEND CALL
    // (send_send_video_v2 does not copy; its worker reads it asynchronously).
    // Handing it a stack/scoped buffer (the old local UYVY vector, or worse
    // the zero-copy pointer into the caller's frame) lets the SDK read freed
    // heap. sendBuffer_ holds the flattened, opaque-alpha pixels between
    // sends, so the SDK's view of "previous frame" is always live memory.
    std::vector<uint32_t> sendBuffer_;
};

} // namespace bps::display
