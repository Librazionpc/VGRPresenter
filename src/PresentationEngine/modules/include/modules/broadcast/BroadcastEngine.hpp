#pragma once

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
#include <shared_mutex>
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
    struct NdiRuntimeStatus {
        enum class State { Ready, NotInstalled, Error };
        State state = State::NotInstalled;
        std::string detail;    // human-readable reason (empty when Ready)
        std::string version;   // vendor runtime version (Ready only)
    };
    NdiRuntimeStatus NdiStatus() const;
    void PreferProvider(std::string_view name);
    bool SdiAvailable() const;

    void SetNdiEnabled(bool enabled);
    bool NdiEnabled() const { return ndiEnabled_.load(); }
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

    std::vector<std::shared_ptr<IBroadcastProvider>> ProviderSnapshot() const;

    void RecordError(std::string_view provider, std::string_view message);

    std::map<std::string, std::shared_ptr<IBroadcastProvider>, std::less<>> providers_;
    std::string preferredProvider_;   // "" = automatic (see PreferProvider)
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
    mutable std::shared_mutex providersMu_;
};

Result<void> RegisterBuiltinBroadcastProviders(BroadcastEngine& engine);

std::shared_ptr<IBroadcastProvider> MakeNdiProvider();
} // namespace bps::broadcast