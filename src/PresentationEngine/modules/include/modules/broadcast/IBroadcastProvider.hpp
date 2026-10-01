#pragma once

// IBroadcastProvider (docs/specs/29 §3): the provider seam for NDI, SDI and
// software loopback. Providers are registered with the BroadcastEngine and can
// be added by plugins without modifying the engine. A provider owns the SDK
// lifecycle for its technology (runtime dlopen for NDI/DeckLink) and reports
// Unsupported when the library or hardware is absent.

#include "modules/broadcast/BroadcastTypes.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace bps::broadcast {

// A send handle: opaque id owned by the engine.
using BroadcastSenderId = std::string;
using BroadcastReceiverId = std::string;

class IBroadcastProvider {
public:
    virtual ~IBroadcastProvider() = default;

    virtual const char* Name() const noexcept = 0;   // "ndi" | "sdi" | "software"
    virtual ProviderKind Kind() const noexcept = 0;

    // Probing never throws and never aborts: it resolves the runtime SDK and
    // reports availability. Returns Unsupported when the SDK/hardware is absent.
    virtual Result<ProviderState> Probe() = 0;
    // Vendor runtime version string once Probe() succeeded ("" when unknown /
    // not applicable) — surfaced in Settings · Plugins.
    virtual std::string RuntimeVersion() const { return {}; }

    // --- NDI-style discovery / send / receive (all providers) ---
    virtual std::vector<NdiSourceInfo> DiscoverSources() { return {}; }

    virtual Result<BroadcastSenderId> CreateSender(std::string_view /*name*/,
                                                   const NdiSenderConfig&) {
        return Error::Make(Err::Broadcast_Unsupported, Name(),
                           "senders not supported by this provider");
    }
    // Sends one video frame; the payload is borrowed for the duration of the call.
    virtual Result<void> SendVideo(std::string_view senderId, const VideoFrameInfo& info,
                                   const void* data, size_t bytes) {
        (void)senderId; (void)info; (void)data; (void)bytes;
        return Error::Make(Err::Broadcast_Unsupported, Name(),
                           "video send not supported by this provider");
    }
    virtual Result<void> SendAudio(std::string_view senderId, const AudioFrameInfo& info,
                                   const void* data, size_t bytes) {
        (void)senderId; (void)info; (void)data; (void)bytes;
        return Error::Make(Err::Broadcast_Unsupported, Name(),
                           "audio send not supported by this provider");
    }
    virtual Result<void> StopSender(std::string_view senderId) {
        (void)senderId;
        return Ok();
    }
    // How many receivers are currently CONNECTED to a sender (-1 = this
    // provider can't tell). The NDI provider reads the SDK's own connection
    // count for its NDI-shaped senders: "frames flowing but 0 receivers" is
    // THE firewall/discovery symptom — frames go out, nothing accepts them.
    virtual int ConnectedReceiverCount(std::string_view /*senderId*/) const {
        return -1;
    }
    // How many receivers are currently CONNECTED to this sender (-1 = the
    // provider can't tell). The NDI provider reads the SDK's own connection
    // count: "frames flowing but 0 receivers" is THE firewall/discovery
    // symptom — frames go out, nothing on the network accepts them.

    // --- Receive ---
    virtual Result<BroadcastReceiverId> CreateReceiver(std::string_view sourceName) {
        (void)sourceName;
        return Error::Make(Err::Broadcast_Unsupported, Name(),
                           "receiving not supported by this provider");
    }
    // Pulls the newest frame; returns true when a frame was available.
    virtual Result<bool> ReceiveFrame(std::string_view receiverId, VideoFrameInfo& info,
                                      std::vector<uint8_t>& out) {
        (void)receiverId; (void)info; (void)out;
        return Error::Make(Err::Broadcast_Unsupported, Name(),
                           "receiving not supported by this provider");
    }
    virtual Result<void> DisconnectReceiver(std::string_view receiverId) {
        (void)receiverId;
        return Ok();
    }

    // --- SDI ---
    virtual std::vector<SdiDeviceInfo> EnumerateSdiDevices() { return {}; }
    virtual Result<std::string> ConnectSdiCapture(int deviceIndex,
                                                  std::string_view graphNodeId) {
        (void)deviceIndex; (void)graphNodeId;
        return Error::Make(Err::Broadcast_Unsupported, Name(),
                           "SDI capture not supported by this provider");
    }
    virtual Result<void> DisconnectSdiCapture(std::string_view captureId) {
        (void)captureId;
        return Ok();
    }

    // Cleanup of any resources this provider owns.
    virtual Result<void> ShutdownProvider() { return Ok(); }
};

using BroadcastProviderFactory = std::function<std::shared_ptr<IBroadcastProvider>()>;

} // namespace bps::broadcast
