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
namespace {
} // namespace

// One shared copy per rendered buffer. Color::Pack is 0xAABBGGRR, which is RGBA byte order in memory.
void ConvertToNdiRgba(const uint32_t* pixels, int width, int height, NdiRgbaFrame& target)
{
    if (!pixels || width <= 0 || height <= 0)
        return;
    const size_t bytes = static_cast<size_t>(width) * static_cast<size_t>(height) * 4u;
    if (target.bytes.size() != bytes)
        target.bytes.resize(bytes);   // only on a size change: a steady tick never reallocates
    target.width = width;
    target.height = height;
    std::memcpy(target.bytes.data(), pixels, bytes);
}

std::shared_ptr<const NdiRgbaFrame> MakeNdiRgbaFrame(const uint32_t* pixels, int width, int height)
{
    if (!pixels || width <= 0 || height <= 0)
        return {};
    auto frame = std::make_shared<NdiRgbaFrame>();
    ConvertToNdiRgba(pixels, width, height, *frame);
    return frame;
}

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
    float fps = 30.0f;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        fps = frameRate_;
    }
    return SendRgbaFrame(MakeNdiRgbaFrame(frame.pixels, frame.width, frame.height), fps);
}

Result<void> NdiDisplayProvider::SendRgbaFrame(std::shared_ptr<const NdiRgbaFrame> frame, float fps) {
    if (!frame || frame->bytes.empty() || frame->width <= 0 || frame->height <= 0)
        return Error::Make(Err::InvalidArgument, "NdiDisplayProvider", "empty frame");

    auto& bc = broadcast::BroadcastEngine::Instance();

    std::unique_lock<std::mutex> sendLock(sendMutex_);

    std::string senderName;
    std::string senderId;
    // The frame handed to the PREVIOUS async send is still being read until this one hands over a new one,
    // so it is kept alive here and released only once the runtime has this frame instead.
    std::shared_ptr<const NdiRgbaFrame> previous;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        senderName = senderName_;
        senderId = senderId_;
        previous = lastShared_;
    }
    const float frameRate = AcceptedOutputFrameRate(fps);

    // Lazy sender creation on first use (idempotent). The SDK call runs with
    // no state lock held.
    if (senderId.empty()) {
        auto r = bc.Initialize();   // no-op if already initialized
        if (!r.ok()) return r;
        broadcast::NdiSenderConfig cfg;
        // WE PACE THE SEND OURSELVES — a frame is handed over at the output's own fps — so the SDK's
        // clock would only hold each frame back to its internal tick: latency added to every frame,
        // which the receiver shows as a stream that is always a beat behind.
        cfg.clockVideo = false;
        auto id = bc.CreateNdiSender(senderName, cfg);
        if (!id.ok()) return id.error();
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (senderId_.empty()) senderId_ = id.value();
            senderId = senderId_;
        }
    }

    // RGBA carries the template's alpha channel; UYVY would flatten transparent pixels to black.
    const size_t rgbaBytes = frame->bytes.size();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        lastShared_ = frame;
    }

    broadcast::VideoFrameInfo info;
    info.width = static_cast<uint32_t>(frame->width);
    info.height = static_cast<uint32_t>(frame->height);
    info.fourCC = 0x41424752;   // 'RGBA' — preserve per-pixel alpha for downstream compositing
    info.fps = frameRate;       // the caller's real cadence
    auto r = bc.SendVideoFrame(senderId, info, frame->bytes.data(), rgbaBytes);   // async: returns at once
    if (r.ok()) {
        framesSent_.fetch_add(1);
        sendFailStreak_ = 0;

        // ---- ONE LINE PER SECOND: WHAT THIS SENDER IS ACTUALLY DOING --------------------------
        // Payload is measured, not assumed: it is the RGBA bytes handed to the SDK at this fps. The shortfall
        // is the honest drop signal on this side — frames the output cadence asked for that never
        // reached the SDK, which is what the far machine shows as stutter and drift. A sender that is
        // keeping up logs ~target fps and 0 short; lagging ones show the gap directly.
        const auto tickNow = std::chrono::steady_clock::now();
        if (sendWindowStart_ == std::chrono::steady_clock::time_point{})
            sendWindowStart_ = tickNow;
        sendWindowFrames_ += 1;
        sendWindowBytes_ += rgbaBytes;
        const double windowSec = std::chrono::duration<double>(tickNow - sendWindowStart_).count();
        if (windowSec >= 1.0) {
            const double fps = static_cast<double>(sendWindowFrames_) / windowSec;
            const double mbit = static_cast<double>(sendWindowBytes_) * 8.0 / windowSec / 1.0e6;
            const double target = static_cast<double>(frameRate);
            const double wanted = (target - fps) * windowSec;   // frames the cadence asked for
            const long long short_ = wanted > 0.0 ? static_cast<long long>(std::lround(wanted)) : 0LL;
            char line[256];
            std::snprintf(line, sizeof(line),
                          "%.1f fps, %.1f Mbit/s to the SDK, %.2f MB/frame (RGBA), %lld short of %.1f fps",
                          fps, mbit, static_cast<double>(rgbaBytes) / (1024.0 * 1024.0), short_, target);
            Logger::Instance().Info("NdiSender '" + senderName + "': " + line, "Display");
            sendWindowStart_ = tickNow;
            sendWindowFrames_ = 0;
            sendWindowBytes_ = 0;
        }
        return r;
    }

    // A FAILED SEND NEVER HANDED THE RUNTIME THIS FRAME: it is still reading the previous one, so that one
    // goes back rather than being freed under it.
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (lastShared_ == frame)
            lastShared_ = previous;
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
