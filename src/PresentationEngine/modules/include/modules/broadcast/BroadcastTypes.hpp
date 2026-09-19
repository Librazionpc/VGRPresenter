#pragma once

// BroadcastTypes (docs/specs/29): the canonical NDI/SDI model. NDI (NewTek
// SDK) and SDI (Blackmagic DeckLink SDK) are proprietary runtime libraries,
// so the engine models sources/senders/receivers/devices generically and
// resolves the SDKs at runtime through providers. The software provider
// exercises the same contract in memory (tests + rehearsal).

#include "core/common/Common.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace bps::broadcast {

enum class ProviderKind { Ndi, Sdi, Software };
enum class ProviderState { Unavailable, Available, Active, Degraded, Failed };

const char* ToString(ProviderKind kind) noexcept;
const char* ToString(ProviderState state) noexcept;

// A frame's geometry + timing. The payload is passed separately so the
// production graph can hand its own buffers with no copy.
struct VideoFrameInfo {
    uint32_t width = 0;
    uint32_t height = 0;
    // FourCC pixel format, e.g. UYVY (0x59565955) or BGRA (0x41524742).
    uint32_t fourCC = 0x59565955;
    double fps = 30.0;
    int64_t timestampMs = 0;
};

struct AudioFrameInfo {
    int sampleRate = 48000;
    int channels = 2;
    int samples = 0;            // frames (per channel)
    int64_t timestampMs = 0;
};

// A discoverable NDI source (docs/specs/29 §4).
struct NdiSourceInfo {
    std::string name;        // "CAM 1 (192.168.1.10)"
    std::string urlAddress;  // "ndi://192.168.1.10/CAM%201"
};

// A DeckLink/SDI device (docs/specs/29 §5).
struct SdiDeviceInfo {
    int index = 0;
    std::string modelName;
    std::string displayName;
    bool supportsCapture = true;
    bool supportsOutput = false;
    ProviderState state = ProviderState::Available;
};

// Sender configuration.
struct NdiSenderConfig {
    std::string groups;             // optional NDI groups
    bool clockVideo = true;
    bool clockAudio = true;
};

// Per-provider runtime statistics.
struct BroadcastStats {
    uint64_t framesSent = 0;
    uint64_t framesReceived = 0;
    uint64_t sourcesDiscovered = 0;
    uint64_t errors = 0;
};

} // namespace bps::broadcast
