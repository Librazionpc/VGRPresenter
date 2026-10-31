#pragma once

// RecordingPlugin (docs/specs/28): the Phase 16 extension seams. Containers,
// codecs and capture devices are registry-provided — new formats, codecs and
// hardware are registered by plugins, never added to the recording engine core.

#include "core/common/Common.hpp"
#include "modules/production/ProductionTypes.hpp"
#include "modules/recording/RecordingTypes.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace bps::recording {

// A capture source: camera, capture card, screen, NDI, network, audio
// interface, system audio or virtual device (user brief §31).
class ICaptureSource {
public:
    virtual ~ICaptureSource() = default;
    virtual const char* DeviceId() const noexcept = 0;
    virtual const char* Kind() const noexcept = 0;      // "camera" | "screen" | ...
    virtual const char* DisplayName() const noexcept { return DeviceId(); }
    virtual production::SignalType Signal() const noexcept {
        return production::SignalType::Video;
    }
    virtual production::SourceState State() const noexcept {
        return production::SourceState::Connected;
    }
    virtual Result<void> Start() { return Ok(); }
    virtual Result<void> Stop() { return Ok(); }
};
using CaptureSourceFactory = std::function<std::shared_ptr<ICaptureSource>()>;

// One picture for the container: packed RGBA8 pixels (Color::Pack layout — R in the
// low byte), `width` x `height`, stamped in milliseconds from the recording's start.
struct VideoFrameIn {
    const uint32_t* pixels = nullptr;
    int width = 0;
    int height = 0;
    double timestampMs = 0.0;
};

// One block of interleaved float audio, `frames` frames of `channels` channels at
// `rateHz`, stamped in milliseconds from the recording's start.
struct AudioBlockIn {
    const float* samples = nullptr;
    size_t frames = 0;
    uint32_t channels = 0;
    uint32_t rateHz = 0;
    double timestampMs = 0.0;
};

// A recording container (user brief §11): mkv/mp4/mov/webm/wav/flac/...
class IRecordingContainer {
public:
    virtual ~IRecordingContainer() = default;
    virtual const char* Name() const noexcept = 0;      // "mkv" | "mp4" | ...
    // Opens the container at `path` for `profile`; returns the file path used.
    virtual Result<std::string> Open(const RecordingProfile& profile,
                                     std::string_view path) = 0;
    // Flushes the current segment. finalized=false means a segment boundary.
    virtual Result<void> Close(bool finalized) = 0;
    // Samples arrive on the recording's pump thread only, between Open and Close.
    virtual Result<void> WriteVideo(const VideoFrameIn& frame) = 0;
    virtual Result<void> WriteAudio(const AudioBlockIn& block) = 0;
};
using ContainerFactory = std::function<std::shared_ptr<IRecordingContainer>()>;

// A video encoder (user brief §12, §13).
class IVideoEncoder {
public:
    virtual ~IVideoEncoder() = default;
    virtual const char* Codec() const noexcept = 0;     // "H.264" | "HEVC" | ...
    virtual EncoderKind Kind() const noexcept = 0;
    virtual Result<void> Initialize(const RecordingProfile& profile) = 0;
    virtual Result<void> Shutdown() = 0;
};
using VideoEncoderFactory = std::function<std::shared_ptr<IVideoEncoder>()>;

// An audio encoder (user brief §12).
class IAudioEncoder {
public:
    virtual ~IAudioEncoder() = default;
    virtual const char* Codec() const noexcept = 0;     // "PCM" | "AAC" | ...
    virtual Result<void> Initialize(const RecordingProfile& profile) = 0;
    virtual Result<void> Shutdown() = 0;
};
using AudioEncoderFactory = std::function<std::shared_ptr<IAudioEncoder>()>;

} // namespace bps::recording
