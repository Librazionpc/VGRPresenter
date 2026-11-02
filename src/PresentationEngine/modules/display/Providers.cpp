#include "modules/display/Providers.hpp"

#include "core/logging/Logger.hpp"
#include "modules/broadcast/BroadcastEngine.hpp"
#include "platform/IPlatform.hpp"
#include "platform/PlatformAccessor.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
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
// PhysicalDisplayProvider (physical monitors via the PAL — cross-platform;
// the "Linux" name string is legacy, see the header comment)
// ---------------------------------------------------------------------------
std::vector<DisplayDevice> PhysicalDisplayProvider::FromPlatform() {
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
            d.provider = "Linux";   // legacy provider tag (see Providers.hpp)
            d.virtual_ = false;
            out.push_back(std::move(d));
        }
    } catch (...) {
        // No platform backend installed (unit tests) — report no devices.
    }
    return out;
}

std::vector<DisplayDevice> PhysicalDisplayProvider::Enumerate() const {
    return FromPlatform();
}

Result<std::vector<DisplayDevice>> PhysicalDisplayProvider::Probe() {
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

DisplayProviderCapabilities PhysicalDisplayProvider::Capabilities() const noexcept {
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
    ndi.refreshRateHz = static_cast<int>(kDefaultOutputFrameRateHz);   // mirrors frameRate_'s default; SetFrameRate keeps it in sync
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
    std::vector<DisplayDevice> changed;
    for (const auto& d : devices_)
        if (d.id == "ndi-program") changed.push_back(d);
    return changed;
}

DisplayProviderCapabilities NdiDisplayProvider::Capabilities() const noexcept {
    DisplayProviderCapabilities c;
    c.supportsHotPlug = false;
    c.supportsVirtualOutputs = true;
    c.maxOutputs = 16;
    c.scalingModes = {ScalingMode::Fit, ScalingMode::Stretch, ScalingMode::Fill};
    c.colorProfiles = {ColorProfile::SRgb};
    return c;
}

Result<void> NdiDisplayProvider::SendFrame(const RenderFrameView& frame) {
    if (!frame.pixels || frame.width <= 0 || frame.height <= 0)
        return Error::Make(Err::InvalidArgument, "NdiDisplayProvider", "empty frame");

    auto& bc = broadcast::BroadcastEngine::Instance();

    std::unique_lock<std::mutex> sendLock(sendMutex_);

    std::string senderName;
    float frameRate = 30.0f;
    std::string senderId;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        senderName = senderName_;
        frameRate = frameRate_;
        senderId = senderId_;
    }

    // Lazy sender creation on first use (idempotent). The SDK call runs with
    // no state lock held.
    if (senderId.empty()) {
        auto r = bc.Initialize();   // no-op if already initialized
        if (!r.ok()) return r;
        broadcast::NdiSenderConfig cfg;
        auto id = bc.CreateNdiSender(senderName, cfg);
        if (!id.ok()) return id.error();
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (senderId_.empty()) senderId_ = id.value();
            senderId = senderId_;
        }
    }

    broadcast::VideoFrameInfo info;
    info.width = static_cast<uint32_t>(frame.width);
    info.height = static_cast<uint32_t>(frame.height);
    info.fourCC = 0x41424752;   // 'RGBA' — the engine's native layout, byte for byte
    info.fps = frameRate;       // the caller's real cadence (SetFrameRate)
    const size_t count = static_cast<size_t>(frame.width) * static_cast<size_t>(frame.height);
    sendBuffer_.assign(frame.pixels, frame.pixels + count);

    const size_t bytes = count * 4u;
    auto r = bc.SendVideoFrame(senderId, info, sendBuffer_.data(), bytes);   // SDK send, no state lock
    if (r.ok()) {
        framesSent_.fetch_add(1);
        sendFailStreak_ = 0;
        return r;
    }

    // ---- SELF-HEAL: "NDI stays at zero until the app is restarted" ----------
    const auto now = std::chrono::steady_clock::now();
    const bool firstOfStreak = sendFailStreak_ == 0;
    ++sendFailStreak_;
    if (firstOfStreak || now - lastHeal_ > std::chrono::seconds(1)) {
        lastHeal_ = now;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            senderId_.clear();   // next SendFrame recreates it (lazily, same name)
        }
        sendLock.unlock();       // no provider lock across the engine teardown
        (void)bc.StopSender(senderId);
        Logger::Instance().Warning(
            "NdiDisplayProvider: frame send failed (" + r.error().message
                + ") — sender reset; the next frame recreates it as '" + senderName + "'",
            "Display");
    }
    return r;
}

void NdiDisplayProvider::SetSenderName(std::string name) {
    std::lock_guard<std::mutex> lock(mutex_);
    senderName_ = std::move(name);
}

void NdiDisplayProvider::SetFrameRate(float fps) {
    std::lock_guard<std::mutex> lock(mutex_);
    frameRate_ = AcceptedOutputFrameRate(fps);
    if (!devices_.empty())
        devices_.front().refreshRateHz = static_cast<int>(std::lround(frameRate_));
}

float NdiDisplayProvider::FrameRate() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return frameRate_;
}

void NdiDisplayProvider::ResetSender() {
    std::string senderId;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (senderId_.empty())
            return;   // nothing live to reset
        senderId = senderId_;
        senderId_.clear();   // the next SendFrame recreates it under senderName_
    }
    (void)broadcast::BroadcastEngine::Instance().StopSender(senderId);
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