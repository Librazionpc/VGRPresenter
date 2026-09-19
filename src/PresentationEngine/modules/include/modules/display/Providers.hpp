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

// --- Linux provider: physical monitors via the PAL IMonitor subsystem.
class LinuxDisplayProvider final : public IDisplayProvider {
public:
    LinuxDisplayProvider() = default;

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
// Enumerates one virtual "NDI Program" device. SendFrame() converts an RGBA8
// frame to UYVY and pushes it through BroadcastEngine::SendVideoFrame; when
// the NDI SDK is not installed the engine's software loopback carries it, so
// the whole path is exercised in CI. Frame delivery itself stays with the
// caller (the sink in DisplayEngine::RouteFrame) — this provider is the
// *device*, exactly like the Linux provider.
class NdiDisplayProvider final : public IDisplayProvider {
public:
    NdiDisplayProvider();

    const char* Name() const noexcept override { return "Ndi"; }
    const char* Version() const noexcept override { return "1.0"; }

    std::vector<DisplayDevice> Enumerate() const override;
    Result<std::vector<DisplayDevice>> Probe() override;   // static device set
    DisplayProviderCapabilities Capabilities() const noexcept override;

    // Sends one RGBA frame as UYVY video over the BroadcastEngine. Creates the
    // NDI sender on first use; errors (unconfigured engine) are reported.
    Result<void> SendFrame(const RenderFrameView& frame);

    // Sender name used for the NDI source ("VGR Program" by default).
    void SetSenderName(std::string name);
    std::string SenderName() const;
    uint64_t FramesSent() const;

private:
    mutable std::mutex mutex_;
    std::vector<DisplayDevice> devices_;
    std::string senderName_ = "VGR Program";
    std::string senderId_;          // BroadcastEngine sender id (lazily created)
    std::atomic<uint64_t> framesSent_{0};
};

} // namespace bps::display
