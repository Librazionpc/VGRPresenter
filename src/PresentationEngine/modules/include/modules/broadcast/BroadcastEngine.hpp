#pragma once

// BroadcastEngine (docs/specs/29): the Phase 17 facade for NDI and SDI.
// NDI (libndi) and SDI (Blackmagic DeckLink) are proprietary runtime SDKs, so
// the engine never links them: providers resolve the SDK at runtime and the
// engine stays testable without any hardware. A software loopback provider
// ships in-tree so the full send/receive/discover contract runs in CI and in
// operator rehearsal.

#include "core/common/Common.hpp"
#include "core/events/EventBus.hpp"
#include "core/events/Events.hpp"
#include "interfaces/IService.hpp"
#include "modules/broadcast/BroadcastTypes.hpp"
#include "modules/broadcast/IBroadcastProvider.hpp"

#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace bps::broadcast {

class BroadcastEngine final : public IService {
public:
    static BroadcastEngine& Instance();

    // --- Lifecycle (engine-wide contract) ---
    Result<void> Initialize() override;
    Result<void> Start() override;
    Result<void> Stop() override;
    Result<void> Shutdown() override;
    Result<void> Reload() override;
    Result<void> Reset() override;
    HealthReport GetHealth() const override;
    Metrics MetricsSnapshot() const override;
    const char* ServiceName() const noexcept override { return "BroadcastEngine"; }

    // --- Provider registry (docs/specs/29 §3) ---
    // Register a provider. Duplicate names are rejected; the built-in
    // "software" provider is pre-registered at Initialize.
    Result<void> RegisterProvider(std::shared_ptr<IBroadcastProvider> provider);
    std::vector<std::string> ProviderNames() const;
    Result<ProviderState> Probe(std::string_view name);

    // --- NDI discovery / send / receive (docs/specs/29 §4) ---
    Result<std::vector<NdiSourceInfo>> DiscoverNdiSources();
    Result<BroadcastSenderId> CreateNdiSender(std::string_view name,
                                              const NdiSenderConfig& cfg = {});
    Result<void> SendVideoFrame(std::string_view senderId, const VideoFrameInfo& info,
                                const void* data, size_t bytes);
    Result<void> SendAudioFrame(std::string_view senderId, const AudioFrameInfo& info,
                                const void* data, size_t bytes);
    Result<void> StopSender(std::string_view senderId);
    std::vector<std::string> SenderIds() const;
    // Receivers currently CONNECTED to this sender (-1 = unknown). The NDI
    // provider reads the SDK's own count; "sending but 0 connected" is the
    // firewall/discovery case the UI surfaces.
    int SenderConnectedReceivers(std::string_view senderId) const;

    Result<BroadcastReceiverId> CreateNdiReceiver(std::string_view sourceName);
    // Pulls the newest frame. Returns true when a frame was available.
    Result<bool> ReceiveFrame(std::string_view receiverId, VideoFrameInfo& info,
                              std::vector<uint8_t>& out);
    Result<void> DisconnectReceiver(std::string_view receiverId);
    std::vector<std::string> ReceiverIds() const;

    // --- SDI (docs/specs/29 §5) ---
    Result<std::vector<SdiDeviceInfo>> EnumerateSdiDevices();
    // Connects an SDI device as a production-graph capture source.
    Result<std::string> ConnectSdiCapture(int deviceIndex, std::string_view graphNodeId);
    Result<void> DisconnectSdiCapture(std::string_view captureId);
    std::vector<std::string> SdiCaptureIds() const;

    // --- Introspection ---
    BroadcastStats Stats() const;
    bool NdiAvailable() const;
    // Why NDI is or isn't usable — what the UI shows and acts on. NotInstalled
    // is the actionable case (link the user to the runtime download); Error is
    // "installed but unusable" (detail says why).
    struct NdiRuntimeStatus {
        enum class State { Ready, NotInstalled, Error };
        State state = State::NotInstalled;
        std::string detail;    // human-readable reason (empty when Ready)
        std::string version;   // vendor runtime version (Ready only)
    };
    NdiRuntimeStatus NdiStatus() const;
    // Pins sender/receiver creation to one provider ("software" makes tests
    // deterministic on machines that have the real NDI runtime; "" = automatic:
    // real NDI when usable, software loopback otherwise).
    void PreferProvider(std::string_view name);
    bool SdiAvailable() const;

    // The "NDI Broadcast" feature switch (Settings · Plugins → AdaptiveRuntime's
    // registry), engine-side: OFF refuses every NDI-shaped sender/receiver
    // (including the software-loopback fallback — nothing NDI-shaped runs),
    // empties discovery of real network sources, reports the switched-off
    // state through NdiStatus()/NdiAvailable(), and tears down live senders
    // and receivers created through the real provider. Default on.
    void SetNdiEnabled(bool enabled);
    bool NdiEnabled() const { return ndiEnabled_.load(); }
    // The "sdi" feature switch (Settings · Plugins, OFF by default): while
    // off, every sdi path refuses WITHOUT probing the DeckLink SDK — the
    // absent-library probe used to publish "DeckLink SDK not installed" as
    // an error toast on every device refresh (the "stop spamming me until
    // we are ready for it" report).
    void SetSdiEnabled(bool enabled);
    bool SdiEnabled() const { return sdiEnabled_.load(); }

    // --- Events ---
    void WireEvents();
    void UnwireEvents();

private:
    BroadcastEngine() = default;

    // RegisterProvider under the engine mutex (Initialize holds it already).
    Result<void> RegisterProviderLocked(std::shared_ptr<IBroadcastProvider> provider);

    // Looks up the named provider (the "ndi"/"sdi" built-ins or a registered
    // plugin provider); returns Err::Broadcast_ProviderNotFound on miss.
    std::shared_ptr<IBroadcastProvider> Find(std::string_view name) const;

    void RecordError(std::string_view provider, std::string_view message);

    std::map<std::string, std::shared_ptr<IBroadcastProvider>, std::less<>> providers_;
    std::string preferredProvider_;   // "" = automatic (see PreferProvider)
    // Off by default (matches AdaptiveRuntime's own "ndi" feature default —
    // see its registration comment): NDI sending needs a firewall rule the
    // user hasn't granted on a fresh install yet. EngineBridge::boot() also
    // syncs this to the registry's actual state unconditionally, so this
    // starting value only matters for the narrow window before that sync
    // runs, or for a caller that reaches BroadcastEngine before EngineBridge
    // boots at all — staying off is the safe default either way.
    std::atomic<bool> ndiEnabled_{false};   // the "ndi" feature switch (SetNdiEnabled)
    std::atomic<bool> sdiEnabled_{false};   // the "sdi" feature switch (SetSdiEnabled)
    std::map<BroadcastSenderId, std::string, std::less<>> senders_;    // id -> provider
    std::map<BroadcastReceiverId, std::string, std::less<>> receivers_; // id -> provider
    std::map<std::string, std::string, std::less<>> sdiCaptures_;      // id -> provider
    std::vector<Subscription> subscriptions_;

    std::atomic<uint64_t> senderCounter_{0};
    std::atomic<uint64_t> receiverCounter_{0};
    std::atomic<uint64_t> captureCounter_{0};
    std::atomic<uint64_t> framesSent_{0};
    std::atomic<uint64_t> framesReceived_{0};
    std::atomic<uint64_t> sourcesDiscovered_{0};
    std::atomic<uint64_t> errorCount_{0};
    std::atomic<bool> initialized_{false};
    std::atomic<bool> running_{false};
    mutable std::recursive_mutex mutex_;
};

// Registers the built-in NDI and SDI providers (runtime SDK loading). Called by
// BroadcastEngine::Initialize; declared here so the engine and Kernel can wire
// providers without exposing provider internals.
Result<void> RegisterBuiltinBroadcastProviders(BroadcastEngine& engine);

// A fresh, unopened NDI provider instance (the concrete class is internal to
// BroadcastProviders.cpp). SetNdiEnabled's off→ teardown → on cycle uses this
// to re-register a clean provider after shutting the old one's SDK handles
// down; Initialize registers through it too.
std::shared_ptr<IBroadcastProvider> MakeNdiProvider();

} // namespace bps::broadcast
