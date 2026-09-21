#include "modules/broadcast/BroadcastEngine.hpp"

#include "core/events/EventBus.hpp"
#include "core/logging/Logger.hpp"

#include <algorithm>
#include <format>
#include <sstream>

namespace bps::broadcast {

namespace {

constexpr const char* kModule = "BroadcastEngine";

// ---------------------------------------------------------------------------
// Built-in software loopback provider (docs/specs/29 §11). Senders write into
// a per-sender ring; receivers pull the newest frame. This exercises the whole
// provider contract in CI and rehearsal without any SDK or hardware.
// ---------------------------------------------------------------------------
class SoftwareProvider final : public IBroadcastProvider {
public:
    const char* Name() const noexcept override { return "software"; }
    ProviderKind Kind() const noexcept override { return ProviderKind::Software; }
    Result<ProviderState> Probe() override {
        return ProviderState::Available;   // always present
    }

    std::vector<NdiSourceInfo> DiscoverSources() override {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<NdiSourceInfo> out;
        for (const auto& [id, name] : senders_)
            out.push_back(NdiSourceInfo{name, "loopback://" + id});
        return out;
    }

    Result<BroadcastSenderId> CreateSender(std::string_view name,
                                           const NdiSenderConfig&) override {
        std::lock_guard<std::mutex> lock(mutex_);
        const std::string id = std::format("sw-send-{}", ++senderSeq_);
        senders_[id] = std::string(name);
        return id;
    }

    Result<void> SendVideo(std::string_view senderId, const VideoFrameInfo& info,
                           const void* data, size_t bytes) override {
        if (bytes == 0 || data == nullptr)
            return Error::Make(Err::Broadcast_InvalidFrame, Name(),
                               "software send: empty frame");
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = senders_.find(std::string(senderId));
        if (it == senders_.end())
            return Error::Make(Err::Broadcast_NotFound, Name(),
                               "software send: unknown sender");
        auto& ring = rings_[it->first];
        const auto* raw = static_cast<const uint8_t*>(data);
        ring = FrameBuffer{info, bytes, std::vector<uint8_t>(raw, raw + bytes)};
        return Ok();
    }

    Result<void> SendAudio(std::string_view senderId, const AudioFrameInfo& info,
                           const void* data, size_t bytes) override {
        (void)info;
        std::lock_guard<std::mutex> lock(mutex_);
        if (!senders_.count(std::string(senderId)))
            return Error::Make(Err::Broadcast_NotFound, Name(),
                               "software send: unknown sender");
        if (bytes == 0 || data == nullptr) return Ok();   // silence is valid audio
        lastAudioSize_ = bytes;
        return Ok();
    }

    Result<void> StopSender(std::string_view senderId) override {
        std::lock_guard<std::mutex> lock(mutex_);
        const std::string id(senderId);
        senders_.erase(id);
        rings_.erase(id);
        return Ok();
    }

    Result<BroadcastReceiverId> CreateReceiver(std::string_view sourceName) override {
        std::lock_guard<std::mutex> lock(mutex_);
        // Match "loopback://<sender-id>" or the display name of a live sender.
        std::string src(sourceName);
        if (src.starts_with("loopback://")) src = src.substr(11);
        bool found = senders_.count(src) > 0;
        if (!found) {
            for (const auto& [id, name] : senders_)
                if (name == src) { found = true; break; }
        }
        if (!found)
            return Error::Make(Err::Broadcast_SourceNotFound, Name(),
                               "software receive: unknown source '" +
                                   std::string(sourceName) + "'");
        const std::string id = std::format("sw-recv-{}", ++receiverSeq_);
        receivers_[id] = std::string(sourceName);
        return id;
    }

    Result<bool> ReceiveFrame(std::string_view receiverId, VideoFrameInfo& info,
                              std::vector<uint8_t>& out) override {
        std::lock_guard<std::mutex> lock(mutex_);
        const std::string id(receiverId);
        auto rit = receivers_.find(id);
        if (rit == receivers_.end())
            return Error::Make(Err::Broadcast_NotFound, Name(),
                               "software receive: unknown receiver");
        // Resolve the source this receiver is bound to (a sender display name)
        // back to its sender id, which is the ring key.
        std::string src = rit->second;
        if (src.starts_with("loopback://")) src = src.substr(11);
        std::string senderId;
        for (const auto& [id, name] : senders_)
            if (name == src || id == src) { senderId = id; break; }
        if (senderId.empty()) return false;
        auto ring = rings_.find(senderId);
        if (ring == rings_.end() || ring->second.data.empty())
            return false;   // no frame yet
        info = ring->second.info;
        out = ring->second.data;
        return true;
    }

    Result<void> DisconnectReceiver(std::string_view receiverId) override {
        std::lock_guard<std::mutex> lock(mutex_);
        receivers_.erase(std::string(receiverId));
        return Ok();
    }

    size_t SenderCount() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return senders_.size();
    }

private:
    struct FrameBuffer {
        VideoFrameInfo info;
        size_t bytes = 0;
        std::vector<uint8_t> data;
    };
    mutable std::mutex mutex_;
    std::map<std::string, std::string, std::less<>> senders_;    // id -> display name
    std::map<std::string, std::string, std::less<>> receivers_;  // id -> source name
    std::map<std::string, FrameBuffer, std::less<>> rings_;
    uint64_t senderSeq_ = 0;
    uint64_t receiverSeq_ = 0;
    size_t lastAudioSize_ = 0;
};

} // namespace

BroadcastEngine& BroadcastEngine::Instance() {
    static BroadcastEngine instance;
    return instance;
}

// --- Lifecycle --------------------------------------------------------------

Result<void> BroadcastEngine::Initialize() {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (initialized_.load()) return Ok();
    // The software loopback provider is always available — CI and rehearsal
    // exercise the full contract without any SDK (docs/specs/29 §11).
    (void)RegisterProviderLocked(std::make_shared<SoftwareProvider>());
    // NDI + SDI providers resolve their SDKs at runtime; on machines without
    // them Probe() reports Unsupported cleanly (docs/specs/29 §2).
    (void)RegisterBuiltinBroadcastProviders(*this);
    initialized_.store(true);
    return Ok();
}

Result<void> BroadcastEngine::Start() {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!initialized_.load())
        return Error::Make(Err::Broadcast_InvalidState, kModule,
                           "initialize before start");
    running_.store(true);
    WireEvents();
    return Ok();
}

Result<void> BroadcastEngine::Stop() {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!running_.load()) return Ok();
    running_.store(false);
    UnwireEvents();
    return Ok();
}

Result<void> BroadcastEngine::Shutdown() {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    (void)Stop();
    for (auto& [name, p] : providers_) (void)p->ShutdownProvider();
    providers_.clear();
    senders_.clear();
    receivers_.clear();
    sdiCaptures_.clear();
    initialized_.store(false);
    return Ok();
}

Result<void> BroadcastEngine::Reload() {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    for (auto& [name, p] : providers_) (void)p->Probe();
    return Ok();
}

Result<void> BroadcastEngine::Reset() {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    senders_.clear();
    receivers_.clear();
    sdiCaptures_.clear();
    preferredProvider_.clear();
    framesSent_.store(0);
    framesReceived_.store(0);
    sourcesDiscovered_.store(0);
    return Ok();
}

HealthReport BroadcastEngine::GetHealth() const {
    HealthReport hr;
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    hr.errorCount = errorCount_.load();
    if (hr.errorCount > 0) {
        hr.state = HealthState::Degraded;
        hr.detail = std::format("provider errors: {}", hr.errorCount);
    } else {
        hr.detail = std::format("providers: {}, senders: {}, receivers: {}",
                                providers_.size(), senders_.size(), receivers_.size());
    }
    if (providers_.empty() && initialized_.load())
        hr.state = HealthState::Degraded;
    return hr;
}

Metrics BroadcastEngine::MetricsSnapshot() const {
    Metrics m;
    m.errorCount = errorCount_.load();
    m.queueLength = senders_.size() + receivers_.size();
    m.health = GetHealth().state;
    return m;
}

// --- Provider registry --------------------------------------------------------

Result<void> BroadcastEngine::RegisterProvider(std::shared_ptr<IBroadcastProvider> provider) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    return RegisterProviderLocked(std::move(provider));
}

Result<void> BroadcastEngine::RegisterProviderLocked(std::shared_ptr<IBroadcastProvider> provider) {
    if (!provider)
        return Error::Make(Err::InvalidArgument, kModule, "null provider");
    const std::string name = provider->Name();
    if (providers_.count(name))
        return Error::Make(Err::Broadcast_ProviderExists, kModule,
                           "provider already registered: " + name);
    providers_[name] = std::move(provider);
    (void)EventBus::Instance().Publish(events::BroadcastProviderRegistered{name});
    return Ok();
}

std::vector<std::string> BroadcastEngine::ProviderNames() const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    std::vector<std::string> out;
    out.reserve(providers_.size());
    for (const auto& [name, p] : providers_) out.push_back(name);
    return out;
}

Result<ProviderState> BroadcastEngine::Probe(std::string_view name) {
    auto p = Find(name);
    if (!p)
        return Error::Make(Err::Broadcast_ProviderNotFound, kModule,
                           "unknown provider '" + std::string(name) + "'");
    auto r = p->Probe();
    if (!r.ok()) {
        // A missing optional SDK is expected on machines without broadcast
        // hardware — record it as a warning, not an error (docs/specs/29 §2).
        Logger::Instance().Warning("[" + std::string(p->Name()) + "] " +
                                       r.error().message,
                                   kModule);
        (void)EventBus::Instance().Publish(
            events::BroadcastProviderUnavailable{p->Name(), r.error().message});
    }
    return r;
}

// --- NDI -----------------------------------------------------------------------

Result<std::vector<NdiSourceInfo>> BroadcastEngine::DiscoverNdiSources() {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    std::vector<NdiSourceInfo> out;
    // Discovery is provider-agnostic: the software provider reports loopback
    // senders; a real NDI provider reports network sources.
    for (const auto& [name, p] : providers_) {
        auto found = p->DiscoverSources();
        out.insert(out.end(), found.begin(), found.end());
    }
    sourcesDiscovered_.store(out.size());
    (void)EventBus::Instance().Publish(events::NdiSourcesChanged{out.size()});
    return out;
}

Result<BroadcastSenderId> BroadcastEngine::CreateNdiSender(std::string_view name,
                                                           const NdiSenderConfig& cfg) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (name.empty())
        return Error::Make(Err::InvalidArgument, kModule, "sender name required");
    // Prefer a real NDI provider when it is actually usable (SDK present);
    // otherwise fall back to the always-available software loopback.
    std::shared_ptr<IBroadcastProvider> p;
    if (!preferredProvider_.empty()) {
        p = Find(preferredProvider_);
    } else {
        p = Find("ndi");
        if (p) {
            auto probe = p->Probe();
            if (!probe.ok() || probe.value() != ProviderState::Available) p = nullptr;
        }
        if (!p) p = Find("software");
    }
    if (!p)
        return Error::Make(Err::Broadcast_ProviderNotFound, kModule,
                           "no sender-capable provider registered");
    auto r = p->CreateSender(name, cfg);
    if (!r.ok()) return r;
    senders_[r.value()] = p->Name();
    (void)EventBus::Instance().Publish(events::BroadcastSenderStarted{r.value(),
                                                                      std::string(name)});
    return r;
}

Result<void> BroadcastEngine::SendVideoFrame(std::string_view senderId,
                                             const VideoFrameInfo& info,
                                             const void* data, size_t bytes) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    auto it = senders_.find(std::string(senderId));
    if (it == senders_.end())
        return Error::Make(Err::Broadcast_NotFound, kModule,
                           "unknown sender '" + std::string(senderId) + "'");
    auto p = Find(it->second);
    if (!p) return Error::Make(Err::Broadcast_ProviderNotFound, kModule, "provider gone");
    auto r = p->SendVideo(senderId, info, data, bytes);
    if (r.ok()) {
        framesSent_.fetch_add(1);
        (void)EventBus::Instance().Publish(
            events::BroadcastFrameSent{std::string(senderId), info.width, info.height});
    } else {
        RecordError(p->Name(), r.error().message);
    }
    return r;
}

Result<void> BroadcastEngine::SendAudioFrame(std::string_view senderId,
                                             const AudioFrameInfo& info,
                                             const void* data, size_t bytes) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    auto it = senders_.find(std::string(senderId));
    if (it == senders_.end())
        return Error::Make(Err::Broadcast_NotFound, kModule,
                           "unknown sender '" + std::string(senderId) + "'");
    auto p = Find(it->second);
    if (!p) return Error::Make(Err::Broadcast_ProviderNotFound, kModule, "provider gone");
    return p->SendAudio(senderId, info, data, bytes);
}

Result<void> BroadcastEngine::StopSender(std::string_view senderId) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    auto it = senders_.find(std::string(senderId));
    if (it == senders_.end())
        return Error::Make(Err::Broadcast_NotFound, kModule,
                           "unknown sender '" + std::string(senderId) + "'");
    auto p = Find(it->second);
    if (p) (void)p->StopSender(senderId);
    senders_.erase(it);
    (void)EventBus::Instance().Publish(events::BroadcastSenderStopped{std::string(senderId)});
    return Ok();
}

std::vector<std::string> BroadcastEngine::SenderIds() const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    std::vector<std::string> out;
    for (const auto& [id, p] : senders_) out.push_back(id);
    return out;
}

Result<BroadcastReceiverId> BroadcastEngine::CreateNdiReceiver(std::string_view sourceName) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    std::shared_ptr<IBroadcastProvider> p;
    if (!preferredProvider_.empty()) {
        p = Find(preferredProvider_);
    } else {
        p = Find("ndi");
        if (p) {
            auto probe = p->Probe();
            if (!probe.ok() || probe.value() != ProviderState::Available) p = nullptr;
        }
        if (!p) p = Find("software");
    }
    if (!p)
        return Error::Make(Err::Broadcast_ProviderNotFound, kModule,
                           "no receiver-capable provider registered");
    auto r = p->CreateReceiver(sourceName);
    if (!r.ok()) return r;
    receivers_[r.value()] = p->Name();
    (void)EventBus::Instance().Publish(
        events::BroadcastReceiverConnected{r.value(), std::string(sourceName)});
    return r;
}

Result<bool> BroadcastEngine::ReceiveFrame(std::string_view receiverId,
                                           VideoFrameInfo& info,
                                           std::vector<uint8_t>& out) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    auto it = receivers_.find(std::string(receiverId));
    if (it == receivers_.end())
        return Error::Make(Err::Broadcast_NotFound, kModule,
                           "unknown receiver '" + std::string(receiverId) + "'");
    auto p = Find(it->second);
    if (!p) return Error::Make(Err::Broadcast_ProviderNotFound, kModule, "provider gone");
    auto r = p->ReceiveFrame(receiverId, info, out);
    if (r.ok() && r.value()) {
        framesReceived_.fetch_add(1);
        (void)EventBus::Instance().Publish(
            events::BroadcastFrameReceived{std::string(receiverId), info.width,
                                           info.height});
    }
    return r;
}

Result<void> BroadcastEngine::DisconnectReceiver(std::string_view receiverId) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    auto it = receivers_.find(std::string(receiverId));
    if (it == receivers_.end())
        return Error::Make(Err::Broadcast_NotFound, kModule,
                           "unknown receiver '" + std::string(receiverId) + "'");
    auto p = Find(it->second);
    if (p) (void)p->DisconnectReceiver(receiverId);
    receivers_.erase(it);
    (void)EventBus::Instance().Publish(
        events::BroadcastReceiverDisconnected{std::string(receiverId)});
    return Ok();
}

std::vector<std::string> BroadcastEngine::ReceiverIds() const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    std::vector<std::string> out;
    for (const auto& [id, p] : receivers_) out.push_back(id);
    return out;
}

// --- SDI -----------------------------------------------------------------------

Result<std::vector<SdiDeviceInfo>> BroadcastEngine::EnumerateSdiDevices() {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    std::vector<SdiDeviceInfo> out;
    auto p = Find("sdi");
    if (!p)
        return Error::Make(Err::Broadcast_NoSdiDevices, kModule,
                           "no SDI provider registered (DeckLink SDK not installed)");
    auto devs = p->EnumerateSdiDevices();
    (void)EventBus::Instance().Publish(events::SdiDevicesChanged{devs.size()});
    return devs;
}

Result<std::string> BroadcastEngine::ConnectSdiCapture(int deviceIndex,
                                                       std::string_view graphNodeId) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    auto p = Find("sdi");
    if (!p)
        return Error::Make(Err::Broadcast_Unsupported, kModule,
                           "SDI capture requires the DeckLink SDK");
    auto r = p->ConnectSdiCapture(deviceIndex, graphNodeId);
    if (!r.ok()) return r;
    sdiCaptures_[r.value()] = p->Name();
    return r;
}

Result<void> BroadcastEngine::DisconnectSdiCapture(std::string_view captureId) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    auto it = sdiCaptures_.find(std::string(captureId));
    if (it == sdiCaptures_.end())
        return Error::Make(Err::Broadcast_NotFound, kModule,
                           "unknown SDI capture '" + std::string(captureId) + "'");
    auto p = Find(it->second);
    if (p) (void)p->DisconnectSdiCapture(captureId);
    sdiCaptures_.erase(it);
    return Ok();
}

std::vector<std::string> BroadcastEngine::SdiCaptureIds() const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    std::vector<std::string> out;
    for (const auto& [id, p] : sdiCaptures_) out.push_back(id);
    return out;
}

// --- Introspection --------------------------------------------------------------

BroadcastStats BroadcastEngine::Stats() const {
    BroadcastStats s;
    s.framesSent = framesSent_.load();
    s.framesReceived = framesReceived_.load();
    s.sourcesDiscovered = sourcesDiscovered_.load();
    s.errors = errorCount_.load();
    return s;
}

bool BroadcastEngine::NdiAvailable() const {
    auto p = Find("ndi");
    if (!p) return false;
    auto r = p->Probe();
    return r.ok() && r.value() == ProviderState::Available;
}

BroadcastEngine::NdiRuntimeStatus BroadcastEngine::NdiStatus() const {
    NdiRuntimeStatus st;
    auto p = Find("ndi");
    if (!p) {
        st.state = NdiRuntimeStatus::State::Error;
        st.detail = "NDI provider is not registered";
        return st;
    }
    auto r = p->Probe();
    if (r.ok() && r.value() == ProviderState::Available) {
        st.state = NdiRuntimeStatus::State::Ready;
        st.version = p->RuntimeVersion();
        return st;
    }
    st.detail = r.ok() ? "NDI runtime is unavailable" : r.error().message;
    st.state = (!r.ok() && r.error().code == Err::Broadcast_SdkNotInstalled)
                   ? NdiRuntimeStatus::State::NotInstalled
                   : NdiRuntimeStatus::State::Error;
    return st;
}

void BroadcastEngine::PreferProvider(std::string_view name) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    preferredProvider_ = std::string(name);
}

bool BroadcastEngine::SdiAvailable() const {
    auto p = Find("sdi");
    if (!p) return false;
    auto r = p->Probe();
    return r.ok() && r.value() == ProviderState::Available;
}

// --- Events ---------------------------------------------------------------------

void BroadcastEngine::WireEvents() {
    UnwireEvents();
}

void BroadcastEngine::UnwireEvents() {
    for (auto& s : subscriptions_) (void)EventBus::Instance().Unsubscribe(s);
    subscriptions_.clear();
}

// --- Private helpers ---------------------------------------------------------------

std::shared_ptr<IBroadcastProvider> BroadcastEngine::Find(std::string_view name) const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    auto it = providers_.find(std::string(name));
    return it == providers_.end() ? nullptr : it->second;
}

void BroadcastEngine::RecordError(std::string_view provider, std::string_view message) {
    errorCount_.fetch_add(1);
    Logger::Instance().Error("[" + std::string(provider) + "] " + std::string(message),
                             kModule);
    (void)EventBus::Instance().Publish(
        events::BroadcastError{std::string(provider), std::string(message)});
}

} // namespace bps::broadcast
