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

    // One lock for the WHOLE send — the buffer lifetime contract below makes
    // the copy and the send one atomic operation per frame (see the
    // sendBuffer_ comment in the header for the crash this prevents).
    std::lock_guard<std::mutex> lock(mutex_);

    // Lazy sender creation on first use (idempotent).
    if (senderId_.empty()) {
        auto& bc = broadcast::BroadcastEngine::Instance();
        auto r = bc.Initialize();   // no-op if already initialized
        if (!r.ok()) return r;
        broadcast::NdiSenderConfig cfg;
        auto id = bc.CreateNdiSender(senderName_, cfg);
        if (!id.ok()) return id.error();
        senderId_ = id.value();
    }

    // NO conversion: the engine's pixels ARE a valid NDI wire format. Color::Pack
    // (0xAABBGGRR, R in the low byte) is little-endian memory order R,G,B,A —
    // exactly NDI's 'RGBA' (0x41424752). The previous per-pixel software
    // RGBA->UYVY pass was both wasted CPU AND the green-screen bug: receivers
    // that decoded its bytes as zero-YUV painted RGB(0,135,0) — the dark green
    // Studio Monitor showed while 4587 frames were "flowing". Sending native
    // RGBA leaves every color decision to the SDK's own proven pipeline.
    broadcast::VideoFrameInfo info;
    info.width = static_cast<uint32_t>(frame.width);
    info.height = static_cast<uint32_t>(frame.height);
    info.fourCC = 0x41424752;   // 'RGBA' — the engine's native layout, byte for byte
    info.fps = 30.0;
    // NDI's RGBA honors per-pixel alpha (UYVY dropped it implicitly): the
    // compositor's scenes can carry <255 alpha in transparent regions and a
    // receiver would composite those over black. The program feed is a
    // flattened picture — force full opacity.
    //
    // ALWAYS copy into sendBuffer_ and send FROM IT. NDI's contract: a sent
    // frame's buffer must remain valid from its send call until the NEXT one
    // (send_send_video_v2 does not copy; its worker thread reads the buffer
    // asynchronously). A scoped/local buffer (or a zero-copy pointer into the
    // caller's frame, which dies milliseconds later) leaves the SDK reading
    // freed heap — the 0xc0000374 heap-corruption crash on GO LIVE. The
    // member's previous contents ARE the "previous frame" the contract
    // requires; the next call overwrites it only after the SDK has had its
    // turn. A 1080p copy is ~8 MB — trivial next to the corruption it buys off.
    const size_t count = static_cast<size_t>(frame.width) * static_cast<size_t>(frame.height);
    sendBuffer_.assign(frame.pixels, frame.pixels + count);
    for (auto& p : sendBuffer_)
        p |= 0xFF000000u;

    auto& bc = broadcast::BroadcastEngine::Instance();
    const size_t bytes = count * 4u;
    auto r = bc.SendVideoFrame(senderId_, info, sendBuffer_.data(), bytes);
    if (r.ok()) framesSent_.fetch_add(1);
    return r;
}

void NdiDisplayProvider::SetSenderName(std::string name) {
    std::lock_guard<std::mutex> lock(mutex_);
    senderName_ = std::move(name);
}

void NdiDisplayProvider::ResetSender() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (senderId_.empty())
        return;   // nothing live to reset
    (void)broadcast::BroadcastEngine::Instance().StopSender(senderId_);
    senderId_.clear();   // the next SendFrame recreates it under senderName_
}

std::string NdiDisplayProvider::SenderName() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return senderName_;
}

std::string NdiDisplayProvider::SenderId() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return senderId_;
}

uint64_t NdiDisplayProvider::FramesSent() const {
    return framesSent_.load();
}

} // namespace bps::display
