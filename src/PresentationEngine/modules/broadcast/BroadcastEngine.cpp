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
    {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        if (initialized_.load()) return Ok();
        // The software loopback provider is always available — CI and rehearsal
        // exercise the full contract without any SDK (docs/specs/29 §11).
        (void)RegisterProviderLocked(std::make_shared<SoftwareProvider>());
    }
    // NDI + SDI provider CONSTRUCTION resolves their SDKs at runtime
    // (LoadLibrary), so it runs OUTSIDE the engine lock — the same rule as
    // every other provider call here. Registration itself only takes the
    // registry's short lock. On machines without the SDKs, Probe() reports
    // Unsupported cleanly (docs/specs/29 §2).
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
    (void)Stop();
    // Snapshot the registry, then shut providers down OUTSIDE every lock:
    // ShutdownProvider() closes vendor SDK handles (the NDI finder teardown
    // alone can take seconds) and must never make a lookup wait.
    std::vector<std::shared_ptr<IBroadcastProvider>> providers;
    {
        std::unique_lock<std::shared_mutex> plock(providersMu_);
        providers.reserve(providers_.size());
        for (auto& [name, p] : providers_) providers.push_back(p);
        providers_.clear();
    }
    for (auto& p : providers) (void)p->ShutdownProvider();
    {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        senders_.clear();
        receivers_.clear();
        sdiCaptures_.clear();
    }
    initialized_.store(false);
    return Ok();
}

Result<void> BroadcastEngine::Reload() {
    // Probe OUTSIDE the lock: Probe() loads and probes a vendor SDK. The
    // "sdi" feature switch still gates its own probe (reloading with the SDK
    // absent used to publish "DeckLink SDK not installed" at every boot); a
    // switched-off provider keeps its registration and just isn't probed.
    const bool sdiOn = sdiEnabled_.load();
    for (const auto& p : ProviderSnapshot()) {
        if (!sdiOn && std::string_view(p->Name()) == "sdi") continue;
        (void)p->Probe();
    }
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
    hr.errorCount = errorCount_.load();
    size_t providerCount = 0;
    {
        std::shared_lock<std::shared_mutex> plock(providersMu_);
        providerCount = providers_.size();
    }
    if (hr.errorCount > 0) {
        hr.state = HealthState::Degraded;
        hr.detail = std::format("provider errors: {}", hr.errorCount);
    } else {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        hr.detail = std::format("providers: {}, senders: {}, receivers: {}",
                                providerCount, senders_.size(), receivers_.size());
    }
    if (providerCount == 0 && initialized_.load())
        hr.state = HealthState::Degraded;
    return hr;
}

Metrics BroadcastEngine::MetricsSnapshot() const {
    Metrics m;
    m.errorCount = errorCount_.load();
    {
        // These two maps were read WITHOUT the lock here; that is a data race
        // with StopSender/DisconnectReceiver, not just a stale number.
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        m.queueLength = senders_.size() + receivers_.size();
    }
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
    {
        std::unique_lock<std::shared_mutex> plock(providersMu_);
        if (providers_.count(name))
            return Error::Make(Err::Broadcast_ProviderExists, kModule,
                               "provider already registered: " + name);
        providers_[name] = std::move(provider);
    }
    (void)EventBus::Instance().Publish(events::BroadcastProviderRegistered{name});
    return Ok();
}

std::vector<std::string> BroadcastEngine::ProviderNames() const {
    std::shared_lock<std::shared_mutex> plock(providersMu_);
    std::vector<std::string> out;
    out.reserve(providers_.size());
    for (const auto& [name, p] : providers_) out.push_back(name);
    return out;
}

Result<ProviderState> BroadcastEngine::Probe(std::string_view name) {
    // The "sdi" feature switch gates its probe BEFORE the provider is
    // touched: EngineBridge::enumerateDevices probes sdi on every device
    // refresh, and without this the absent-SDK failure was published as an
    // error toast each time. The refusal is a clean error (the UI shows it
    // as the sdi status line), not a BroadcastProviderUnavailable event —
    // a switched-off feature is a user choice, not a failure.
    if (name == "sdi" && !sdiEnabled_.load())
        return Error::Make(Err::Broadcast_Unsupported, kModule,
                           "SDI is switched off (Settings · Plugins)");
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
    std::vector<NdiSourceInfo> out;
    // The feature switch (Settings · Plugins): NDI off = no sources, ever —
    // a network browse is exactly what the switch promises to stop.
    if (!ndiEnabled_.load()) {
        sourcesDiscovered_.store(0);
        (void)EventBus::Instance().Publish(events::NdiSourcesChanged{0});
        return out;
    }
    // Discovery is provider-agnostic (the software provider reports loopback
    // senders; a real NDI provider browses the network) and it is SLOW, so it
    // runs OUTSIDE every lock — a network browse under the engine mutex was
    // what froze unrelated callers.
    for (const auto& p : ProviderSnapshot()) {
        auto found = p->DiscoverSources();
        out.insert(out.end(), found.begin(), found.end());
    }
    sourcesDiscovered_.store(out.size());
    (void)EventBus::Instance().Publish(events::NdiSourcesChanged{out.size()});
    return out;
}

Result<BroadcastSenderId> BroadcastEngine::CreateNdiSender(std::string_view name,
                                                           const NdiSenderConfig& cfg) {
    if (name.empty())
        return Error::Make(Err::InvalidArgument, kModule, "sender name required");
    // The feature switch (Settings · Plugins): NDI off refuses every
    // NDI-shaped sender — the software loopback is NDI-shaped too, so it
    // must not silently keep "sending" what the user switched off.
    if (!ndiEnabled_.load())
        return Error::Make(Err::Broadcast_Unsupported, kModule,
                           "NDI is switched off (Settings · Plugins)");
    // Prefer a real NDI provider when it is actually usable (SDK present);
    // otherwise fall back to the always-available software loopback. The
    // Probe() below runs OUTSIDE every lock — it opens the vendor SDK.
    std::string preferred;
    {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        preferred = preferredProvider_;
    }
    std::shared_ptr<IBroadcastProvider> p;
    if (!preferred.empty()) {
        p = Find(preferred);
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
    auto r = p->CreateSender(name, cfg);   // SDK call, lock-free
    if (!r.ok()) return r;
    {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        senders_[r.value()] = p->Name();
    }
    (void)EventBus::Instance().Publish(events::BroadcastSenderStarted{r.value(),
                                                                      std::string(name)});
    return r;
}

Result<void> BroadcastEngine::SendVideoFrame(std::string_view senderId,
                                             const VideoFrameInfo& info,
                                             const void* data, size_t bytes) {
    std::shared_ptr<IBroadcastProvider> p;
    {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        auto it = senders_.find(std::string(senderId));
        if (it == senders_.end())
            return Error::Make(Err::Broadcast_NotFound, kModule,
                               "unknown sender '" + std::string(senderId) + "'");
        p = Find(it->second);
    }
    if (!p) return Error::Make(Err::Broadcast_ProviderNotFound, kModule, "provider gone");
    // The vendor send is the slow part (30 fps per output) and it runs with NO
    // lock held, so a stalled SDK send cannot freeze the engine mutex.
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
    std::shared_ptr<IBroadcastProvider> p;
    {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        auto it = senders_.find(std::string(senderId));
        if (it == senders_.end())
            return Error::Make(Err::Broadcast_NotFound, kModule,
                               "unknown sender '" + std::string(senderId) + "'");
        p = Find(it->second);
    }
    if (!p) return Error::Make(Err::Broadcast_ProviderNotFound, kModule, "provider gone");
    return p->SendAudio(senderId, info, data, bytes);   // SDK call, lock-free
}

Result<void> BroadcastEngine::StopSender(std::string_view senderId) {
    std::shared_ptr<IBroadcastProvider> p;
    {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        auto it = senders_.find(std::string(senderId));
        if (it == senders_.end())
            return Error::Make(Err::Broadcast_NotFound, kModule,
                               "unknown sender '" + std::string(senderId) + "'");
        p = Find(it->second);
        senders_.erase(it);
    }
    if (p) (void)p->StopSender(senderId);   // SDK teardown, lock-free
    (void)EventBus::Instance().Publish(events::BroadcastSenderStopped{std::string(senderId)});
    return Ok();
}

std::vector<std::string> BroadcastEngine::SenderIds() const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    std::vector<std::string> out;
    for (const auto& [id, p] : senders_) out.push_back(id);
    return out;
}

// Delegates to the sender's own provider — the NDI provider answers from the
// SDK's connection count; providers that can't tell return -1 (the UI hides
// the indicator rather than guessing).
int BroadcastEngine::SenderConnectedReceivers(std::string_view senderId) const {
    std::shared_ptr<IBroadcastProvider> p;
    {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        auto it = senders_.find(std::string(senderId));
        if (it == senders_.end()) return -1;
        p = Find(it->second);
    }
    // The SDK connection count is a vendor query — asked with no lock held.
    return p ? p->ConnectedReceiverCount(senderId) : -1;
}

Result<BroadcastReceiverId> BroadcastEngine::CreateNdiReceiver(std::string_view sourceName) {
    // The feature switch (Settings · Plugins): NDI off refuses receivers as
    // well — inputs and outputs stop together, as the switch promises.
    if (!ndiEnabled_.load())
        return Error::Make(Err::Broadcast_Unsupported, kModule,
                           "NDI is switched off (Settings · Plugins)");
    std::string preferred;
    {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        preferred = preferredProvider_;
    }
    std::shared_ptr<IBroadcastProvider> p;
    if (!preferred.empty()) {
        p = Find(preferred);
    } else {
        p = Find("ndi");
        if (p) {
            auto probe = p->Probe();   // OUTSIDE every lock — opens the SDK
            if (!probe.ok() || probe.value() != ProviderState::Available) p = nullptr;
        }
        if (!p) p = Find("software");
    }
    if (!p)
        return Error::Make(Err::Broadcast_ProviderNotFound, kModule,
                           "no receiver-capable provider registered");
    auto r = p->CreateReceiver(sourceName);   // SDK call, lock-free
    if (!r.ok()) return r;
    {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        receivers_[r.value()] = p->Name();
    }
    (void)EventBus::Instance().Publish(
        events::BroadcastReceiverConnected{r.value(), std::string(sourceName)});
    return r;
}

Result<bool> BroadcastEngine::ReceiveFrame(std::string_view receiverId,
                                           VideoFrameInfo& info,
                                           std::vector<uint8_t>& out) {
    std::shared_ptr<IBroadcastProvider> p;
    {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        auto it = receivers_.find(std::string(receiverId));
        if (it == receivers_.end())
            return Error::Make(Err::Broadcast_NotFound, kModule,
                               "unknown receiver '" + std::string(receiverId) + "'");
        p = Find(it->second);
    }
    if (!p) return Error::Make(Err::Broadcast_ProviderNotFound, kModule, "provider gone");
    auto r = p->ReceiveFrame(receiverId, info, out);   // SDK pull, lock-free
    if (r.ok() && r.value()) {
        framesReceived_.fetch_add(1);
        (void)EventBus::Instance().Publish(
            events::BroadcastFrameReceived{std::string(receiverId), info.width,
                                           info.height});
    }
    return r;
}

Result<void> BroadcastEngine::DisconnectReceiver(std::string_view receiverId) {
    std::shared_ptr<IBroadcastProvider> p;
    {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        auto it = receivers_.find(std::string(receiverId));
        if (it == receivers_.end())
            return Error::Make(Err::Broadcast_NotFound, kModule,
                               "unknown receiver '" + std::string(receiverId) + "'");
        p = Find(it->second);
        receivers_.erase(it);
    }
    if (p) (void)p->DisconnectReceiver(receiverId);   // SDK teardown, lock-free
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
    // Feature switch: a disabled SDI refuses cleanly WITHOUT probing the
    // SDK (the old path LoadLibrary'd DeckLink on every device refresh and
    // published the failure as an error toast each time).
    if (!sdiEnabled_.load())
        return Error::Make(Err::Broadcast_Unsupported, kModule,
                           "SDI is switched off (Settings · Plugins)");
    auto p = Find("sdi");
    if (!p)
        return Error::Make(Err::Broadcast_NoSdiDevices, kModule,
                           "no SDI provider registered (DeckLink SDK not installed)");
    auto devs = p->EnumerateSdiDevices();   // DeckLink enumerate, lock-free
    (void)EventBus::Instance().Publish(events::SdiDevicesChanged{devs.size()});
    return devs;
}

Result<std::string> BroadcastEngine::ConnectSdiCapture(int deviceIndex,
                                                       std::string_view graphNodeId) {
    if (!sdiEnabled_.load())
        return Error::Make(Err::Broadcast_Unsupported, kModule,
                           "SDI is switched off (Settings · Plugins)");
    auto p = Find("sdi");
    if (!p)
        return Error::Make(Err::Broadcast_Unsupported, kModule,
                           "SDI capture requires the DeckLink SDK");
    auto r = p->ConnectSdiCapture(deviceIndex, graphNodeId);   // SDK call, lock-free
    if (!r.ok()) return r;
    {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        sdiCaptures_[r.value()] = p->Name();
    }
    return r;
}

Result<void> BroadcastEngine::DisconnectSdiCapture(std::string_view captureId) {
    std::shared_ptr<IBroadcastProvider> p;
    {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        auto it = sdiCaptures_.find(std::string(captureId));
        if (it == sdiCaptures_.end())
            return Error::Make(Err::Broadcast_NotFound, kModule,
                               "unknown SDI capture '" + std::string(captureId) + "'");
        p = Find(it->second);
        sdiCaptures_.erase(it);
    }
    if (p) (void)p->DisconnectSdiCapture(captureId);   // SDK teardown, lock-free
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
    if (!ndiEnabled_.load()) return false;
    auto p = Find("ndi");
    if (!p) return false;
    auto r = p->Probe();
    return r.ok() && r.value() == ProviderState::Available;
}

BroadcastEngine::NdiRuntimeStatus BroadcastEngine::NdiStatus() const {
    NdiRuntimeStatus st;
    // The feature switch reports its own honest state first: the runtime may
    // be installed and fine, but the user switched NDI off — that is neither
    // "not installed" (nothing to download) nor an error (nothing broken).
    if (!ndiEnabled_.load()) {
        st.state = NdiRuntimeStatus::State::Error;
        st.detail = "NDI is switched off in Settings · Plugins";
        return st;
    }
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

// The feature switch, engine-side: OFF tears down everything the real NDI
// provider owns (senders + receivers; its ShutdownProvider also drops the
// persistent discovery finder), then re-registers a FRESH provider so the
// next ON re-opens the SDK cleanly. The software loopback stays registered
// (tests and non-NDI plumbing rely on it) but its NDI-shaped paths above
// are gated by the same switch.
void BroadcastEngine::SetNdiEnabled(bool enabled) {
    // The switch itself flips atomically (status queries stay lock-free); the
    // teardown it triggers — SDK shutdown, sender/receiver stops — runs OUTSIDE
    // every lock. Holding mutex_ across that teardown was the long holder that
    // froze the GUI: its Find() waited here behind NDI SDK work.
    const bool was = ndiEnabled_.exchange(enabled);
    if (was == enabled) return;
    if (enabled) {
        Logger::Instance().Info("[ndi] NDI switched on (Settings · Plugins)", kModule);
        return;
    }
    Logger::Instance().Info("[ndi] NDI switched off (Settings · Plugins) — tearing down senders/receivers", kModule);
    std::vector<std::string> toStop;
    std::vector<std::string> toDrop;
    {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        toStop.reserve(senders_.size());
        for (const auto& [id, provider] : senders_)
            if (provider == "ndi") toStop.push_back(id);
        toDrop.reserve(receivers_.size());
        for (const auto& [id, provider] : receivers_)
            if (provider == "ndi") toDrop.push_back(id);
    }
    for (const auto& id : toStop) (void)StopSender(id);   // publishes sender-stopped
    for (const auto& id : toDrop) (void)DisconnectReceiver(id);
    // Fresh provider: closes the SDK handles (incl. the persistent finder)
    // and re-enters the registry under the same name.
    std::shared_ptr<IBroadcastProvider> ndi;
    {
        std::unique_lock<std::shared_mutex> plock(providersMu_);
        auto it = providers_.find("ndi");
        if (it != providers_.end()) {
            ndi = it->second;
            providers_.erase(it);
        }
    }
    if (ndi) (void)ndi->ShutdownProvider();   // SDK teardown, lock-free
    (void)RegisterProvider(MakeNdiProvider());
    (void)EventBus::Instance().Publish(events::NdiSourcesChanged{0});
}

// The "sdi" feature switch, engine-side. OFF (the default) keeps every sdi
// path at a clean refusal without probing the DeckLink SDK — the absent-
// library probe used to run on every device refresh and publish "DeckLink
// SDK not installed" as an error toast each time (the spam report). ON just
// opens the gate; the next enumerate/refresh re-opens the SDK on demand.
// Going OFF drops any live captures (same teardown contract as NDI's).
void BroadcastEngine::SetSdiEnabled(bool enabled) {
    const bool was = sdiEnabled_.exchange(enabled);
    if (was == enabled) return;
    if (enabled) {
        Logger::Instance().Info("[sdi] SDI switched on (Settings · Plugins)", kModule);
        return;
    }
    Logger::Instance().Info("[sdi] SDI switched off (Settings · Plugins) — dropping live captures", kModule);
    std::vector<std::string> toDrop;
    {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        toDrop.reserve(sdiCaptures_.size());
        for (const auto& [id, provider] : sdiCaptures_) toDrop.push_back(id);
    }
    for (const auto& id : toDrop) (void)DisconnectSdiCapture(id);   // SDK teardown, lock-free
}

bool BroadcastEngine::SdiAvailable() const {
    // The feature switch gates the SDK probe: a disabled SDI reports
    // not-available without ever touching libDeckLinkAPI.
    if (!sdiEnabled_.load()) return false;
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
    // The registry has its OWN lock: a lookup is a map find and must never
    // queue behind a slow provider call (the GUI blocked here, waiting for the
    // engine mutex held across an NDI SDK teardown). Readers do not block each
    // other; only (rare) registration does.
    std::shared_lock<std::shared_mutex> plock(providersMu_);
    auto it = providers_.find(std::string(name));
    return it == providers_.end() ? nullptr : it->second;
}

std::vector<std::shared_ptr<IBroadcastProvider>> BroadcastEngine::ProviderSnapshot() const {
    std::shared_lock<std::shared_mutex> plock(providersMu_);
    std::vector<std::shared_ptr<IBroadcastProvider>> out;
    out.reserve(providers_.size());
    for (const auto& [name, p] : providers_) out.push_back(p);
    return out;
}

void BroadcastEngine::RecordError(std::string_view provider, std::string_view message) {
    errorCount_.fetch_add(1);
    Logger::Instance().Error("[" + std::string(provider) + "] " + std::string(message),
                             kModule);
    (void)EventBus::Instance().Publish(
        events::BroadcastError{std::string(provider), std::string(message)});
}

} // namespace bps::broadcast
