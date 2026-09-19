#include "modules/display/Providers.hpp"

#include "modules/broadcast/BroadcastEngine.hpp"
#include "platform/IPlatform.hpp"
#include "platform/PlatformAccessor.hpp"

#include <algorithm>
#include <cstring>
#include <mutex>

namespace bps::display {

// ---------------------------------------------------------------------------
// VirtualDisplayProvider
// ---------------------------------------------------------------------------
VirtualDisplayProvider::VirtualDisplayProvider() {
    // Default software displays: audience 1080p, stage 720p, preview 540p.
    DisplayDevice audience;
    audience.id = "virtual-aud";
    audience.name = "Virtual Audience";
    audience.width = 1920;
    audience.height = 1080;
    audience.refreshRateHz = 60;
    audience.dpi = 96;
    audience.primary = true;
    audience.connected = true;
    audience.provider = "Virtual";
    audience.virtual_ = true;

    DisplayDevice stage;
    stage.id = "virtual-stage";
    stage.name = "Virtual Stage";
    stage.x = 1920;
    stage.width = 1280;
    stage.height = 720;
    stage.refreshRateHz = 60;
    stage.connected = true;
    stage.provider = "Virtual";
    stage.virtual_ = true;

    DisplayDevice preview;
    preview.id = "virtual-preview";
    preview.name = "Virtual Preview";
    preview.x = 3200;
    preview.width = 960;
    preview.height = 540;
    preview.refreshRateHz = 60;
    preview.connected = true;
    preview.provider = "Virtual";
    preview.virtual_ = true;

    devices_ = {audience, stage, preview};
    for (const auto& d : devices_) lastSeen_[d.id] = d;
}

std::vector<DisplayDevice> VirtualDisplayProvider::Enumerate() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return devices_;
}

Result<std::vector<DisplayDevice>> VirtualDisplayProvider::Probe() {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<DisplayDevice> changed;
    std::map<std::string, DisplayDevice, std::less<>> now;
    for (const auto& d : devices_) now[d.id] = d;
    // Removed devices: report them as *disconnected* so the engine marks the
    // device down and publishes DisplayDeviceDisconnected (the engine keeps the
    // assignment and restores it when the device comes back).
    for (const auto& [id, d] : lastSeen_) {
        if (!now.count(id)) {
            DisplayDevice gone = d;
            gone.connected = false;
            changed.push_back(gone);
        }
    }
    // Added or changed devices.
    for (const auto& d : devices_) {
        auto it = lastSeen_.find(d.id);
        if (it == lastSeen_.end()) {
            changed.push_back(d);
        } else {
            const auto& prev = it->second;
            if (prev.connected != d.connected || prev.width != d.width ||
                prev.height != d.height || prev.refreshRateHz != d.refreshRateHz ||
                prev.x != d.x || prev.y != d.y)
                changed.push_back(d);
        }
    }
    lastSeen_ = now;
    return changed;
}

DisplayProviderCapabilities VirtualDisplayProvider::Capabilities() const noexcept {
    DisplayProviderCapabilities c;
    c.supportsHotPlug = true;
    c.supportsVirtualOutputs = true;
    c.maxOutputs = 16;
    c.scalingModes = {ScalingMode::Native, ScalingMode::Fit, ScalingMode::Fill,
                      ScalingMode::Stretch, ScalingMode::Letterbox, ScalingMode::Crop,
                      ScalingMode::PixelPerfect};
    c.colorProfiles = {ColorProfile::SRgb, ColorProfile::Rec709, ColorProfile::Gamma,
                       ColorProfile::SafeColor};
    return c;
}

Result<void> VirtualDisplayProvider::AddVirtualDevice(const DisplayDevice& device) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& d : devices_)
        if (d.id == device.id)
            return Error::Make(Err::Display_OutputExists, "VirtualDisplayProvider",
                               "device '" + device.id + "' already exists");
    devices_.push_back(device);
    return Ok();
}

Result<void> VirtualDisplayProvider::RemoveVirtualDevice(std::string_view id) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto it = devices_.begin(); it != devices_.end(); ++it) {
        if (it->id == id) {
            devices_.erase(it);
            return Ok();
        }
    }
    return Error::Make(Err::Display_DeviceNotFound, "VirtualDisplayProvider",
                       "device '" + std::string(id) + "' not found");
}

Result<DisplayDevice> VirtualDisplayProvider::GetDevice(std::string_view id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& d : devices_)
        if (d.id == id) return d;
    return Error::Make(Err::Display_DeviceNotFound, "VirtualDisplayProvider",
                       "device '" + std::string(id) + "' not found");
}

// ---------------------------------------------------------------------------
// LinuxDisplayProvider
// ---------------------------------------------------------------------------
std::vector<DisplayDevice> LinuxDisplayProvider::FromPlatform() {
    std::vector<DisplayDevice> out;
    try {
        auto& platform = platform::PlatformAccessor::Get();
        auto monitors = platform.Monitor().Enumerate();
        out.reserve(monitors.size());
        for (const auto& m : monitors) {
            DisplayDevice d;
            d.id = m.id;
            d.name = m.name;
            d.x = m.x;
            d.y = m.y;
            d.width = m.widthPx;
            d.height = m.heightPx;
            d.refreshRateHz = m.refreshRateHz;
            d.dpi = m.dpi;
            d.orientation = m.orientation;
            d.primary = m.primary;
            d.connected = m.connected;
            d.hdrSupported = m.hdrSupported;
            d.provider = "Linux";
            d.virtual_ = false;
            out.push_back(std::move(d));
        }
    } catch (...) {
        // No platform backend installed (unit tests) — report no devices.
    }
    return out;
}

std::vector<DisplayDevice> LinuxDisplayProvider::Enumerate() const {
    return FromPlatform();
}

Result<std::vector<DisplayDevice>> LinuxDisplayProvider::Probe() {
    auto now = FromPlatform();
    std::vector<DisplayDevice> changed;
    std::map<std::string, DisplayDevice, std::less<>> nowMap;
    for (const auto& d : now) nowMap[d.id] = d;
    for (const auto& [id, d] : lastSeen_)
        if (!nowMap.count(id)) changed.push_back(d);
    for (const auto& d : now) {
        auto it = lastSeen_.find(d.id);
        if (it == lastSeen_.end()) {
            changed.push_back(d);
        } else if (it->second.width != d.width || it->second.height != d.height ||
                   it->second.connected != d.connected ||
                   it->second.refreshRateHz != d.refreshRateHz) {
            changed.push_back(d);
        }
    }
    lastSeen_ = nowMap;
    return changed;
}

DisplayProviderCapabilities LinuxDisplayProvider::Capabilities() const noexcept {
    DisplayProviderCapabilities c;
    c.supportsHotPlug = true;    // via PAL PollChanges
    c.supportsVirtualOutputs = false;
    c.scalingModes = {ScalingMode::Native, ScalingMode::Fit, ScalingMode::Fill,
                      ScalingMode::Stretch, ScalingMode::Letterbox, ScalingMode::Crop,
                      ScalingMode::PixelPerfect};
    c.colorProfiles = {ColorProfile::SRgb, ColorProfile::Rec709, ColorProfile::Gamma,
                       ColorProfile::SafeColor};
    return c;
}

// ---------------------------------------------------------------------------
// NdiDisplayProvider
// ---------------------------------------------------------------------------
NdiDisplayProvider::NdiDisplayProvider() {
    DisplayDevice ndi;
    ndi.id = "ndi-program";
    ndi.name = "NDI Program";
    ndi.width = 1920;
    ndi.height = 1080;
    ndi.refreshRateHz = 30;
    ndi.dpi = 96;
    ndi.connected = true;
    ndi.provider = "Ndi";
    ndi.virtual_ = true;
    devices_ = {ndi};
}

std::vector<DisplayDevice> NdiDisplayProvider::Enumerate() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return devices_;
}

Result<std::vector<DisplayDevice>> NdiDisplayProvider::Probe() {
    std::lock_guard<std::mutex> lock(mutex_);
    // Static device set: NDI output is always available; the underlying
    // provider (real NDI vs software loopback) is resolved by the
    // BroadcastEngine at send time.
    std::vector<DisplayDevice> changed;
    for (const auto& d : devices_)
        if (d.id == "ndi-program") changed.push_back(d);
    return changed;
}

DisplayProviderCapabilities NdiDisplayProvider::Capabilities() const noexcept {
    DisplayProviderCapabilities c;
    c.supportsHotPlug = false;
    c.supportsVirtualOutputs = true;
    c.maxOutputs = 1;
    c.scalingModes = {ScalingMode::Fit, ScalingMode::Stretch, ScalingMode::Fill};
    c.colorProfiles = {ColorProfile::SRgb};
    return c;
}

Result<void> NdiDisplayProvider::SendFrame(const RenderFrameView& frame) {
    if (!frame.pixels || frame.width <= 0 || frame.height <= 0)
        return Error::Make(Err::InvalidArgument, "NdiDisplayProvider", "empty frame");

    // Lazy sender creation on first use (idempotent).
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (senderId_.empty()) {
            auto& bc = broadcast::BroadcastEngine::Instance();
            auto r = bc.Initialize();   // no-op if already initialized
            if (!r.ok()) return r;
            broadcast::NdiSenderConfig cfg;
            auto id = bc.CreateNdiSender(senderName_, cfg);
            if (!id.ok()) return id.error();
            senderId_ = id.value();
        }
    }

    // Convert RGBA8 (0xAABBGGRR) -> UYVY422, the standard NDI 4:2:2 layout.
    const size_t w = static_cast<size_t>(frame.width);
    const size_t h = static_cast<size_t>(frame.height);
    std::vector<uint8_t> uyvy((w * h) * 2);
    for (size_t y = 0; y < h; ++y) {
        const uint32_t* row = frame.pixels + y * w;
        uint8_t* outRow = uyvy.data() + y * w * 2;
        for (size_t x = 0; x < w; x += 2) {
            uint32_t p0 = row[x];
            uint32_t p1 = (x + 1 < w) ? row[x + 1] : p0;
            int r0 = static_cast<int>(p0 & 0xFF);
            int g0 = static_cast<int>((p0 >> 8) & 0xFF);
            int b0 = static_cast<int>((p0 >> 16) & 0xFF);
            int r1 = static_cast<int>(p1 & 0xFF);
            int g1 = static_cast<int>((p1 >> 8) & 0xFF);
            int b1 = static_cast<int>((p1 >> 16) & 0xFF);
            auto clamp = [](int v) { return v < 0 ? 0 : (v > 255 ? 255 : v); };
            // BT.601 coefficients.
            int y0 = clamp(((66 * r0 + 129 * g0 + 25 * b0 + 128) >> 8) + 16);
            int y1 = clamp(((66 * r1 + 129 * g1 + 25 * b1 + 128) >> 8) + 16);
            int u = clamp(((-38 * r0 - 74 * g0 + 112 * b0 + 128) >> 8) + 128);
            int v = clamp(((112 * r0 - 94 * g0 - 18 * b0 + 128) >> 8) + 128);
            outRow[0] = static_cast<uint8_t>(u);
            outRow[1] = static_cast<uint8_t>(y0);
            outRow[2] = static_cast<uint8_t>(v);
            outRow[3] = static_cast<uint8_t>(y1);
        }
    }

    broadcast::VideoFrameInfo info;
    info.width = static_cast<uint32_t>(frame.width);
    info.height = static_cast<uint32_t>(frame.height);
    info.fourCC = 0x59565955;   // UYVY
    info.fps = 30.0;
    auto& bc = broadcast::BroadcastEngine::Instance();
    auto r = bc.SendVideoFrame(senderId_, info, uyvy.data(), uyvy.size());
    if (r.ok()) framesSent_.fetch_add(1);
    return r;
}

void NdiDisplayProvider::SetSenderName(std::string name) {
    std::lock_guard<std::mutex> lock(mutex_);
    senderName_ = std::move(name);
}

std::string NdiDisplayProvider::SenderName() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return senderName_;
}

uint64_t NdiDisplayProvider::FramesSent() const {
    return framesSent_.load();
}

} // namespace bps::display
