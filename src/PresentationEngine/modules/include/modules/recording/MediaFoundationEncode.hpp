#pragma once

// The real encoders behind a recording profile's "encoder" choice, driven through Windows
// Media Foundation — the same API the machine's GPU drivers expose their hardware H.264
// encoders through:
//
//   NVENC      the NVIDIA H.264 encoder MFT      (vendor id 10DE)
//   AMF        the AMD H.264 encoder MFT         (vendor id 1002)
//   QuickSync  the Intel H.264 encoder MFT       (vendor id 8086)
//   Software   Microsoft's H.264 encoder MFT
//
// A hardware kind exists only when the machine really HAS that encoder (it is found by
// enumerating the hardware MFTs and reading each one's vendor id), so the list a preset
// can pick from comes from the device, not from a fixed table. The containers
// (MediaFoundationMp4 for the hardware path, MatroskaContainer) take their encoded
// pictures from H264Encoder; AacEncoder produces the raw AAC the Matroska file carries.

#include "core/common/Common.hpp"
#include "modules/recording/RecordingTypes.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace bps::recording {

class RecordingEngine;

// One encoded picture (Annex B byte stream: NAL units behind 00 00 00 01 start codes).
struct EncodedVideo {
    std::vector<uint8_t> data;
    int64_t timeHns = 0;        // 100 ns units
    int64_t durationHns = 0;
    bool keyframe = false;      // an IDR picture (NAL type 5) — a point the file can seek to
};

// One raw AAC frame (no ADTS header).
struct EncodedAudio {
    std::vector<uint8_t> data;
    int64_t timeHns = 0;
    int64_t durationHns = 0;
};

// The hardware encoders this machine has, probed once and cached. Empty when the machine
// has none (or Media Foundation is unavailable).
std::vector<EncoderKind> HardwareEncoderKinds();

// "NVIDIA H.264 Encoder MFT" and the like, for the kind (empty when the machine has none).
std::string HardwareEncoderName(EncoderKind kind);

// The kind a profile actually gets: its own preference when set, otherwise the best the
// machine has (NVENC, AMF, QuickSync, then Software).
EncoderKind ResolveEncoderKind(EncoderKind preference);

// Registers the hardware kinds this machine has with `engine`, so they appear in
// DetectEncoders() and a profile may pick them. Safe to call again. Returns how many exist.
size_t RegisterHardwareEncoders(RecordingEngine& engine);

// ---- Checking a finished file --------------------------------------------------------------

// What Media Foundation's own source reader finds in a recorded file. It is how the tests (and
// anyone diagnosing a take) prove a file really plays rather than just exists.
struct MediaFileInfo {
    bool opened = false;        // the reader accepted the file
    std::string videoSubtype;   // "H264"
    int width = 0;
    int height = 0;
    int videoSamples = 0;       // pictures read to the end of the file
    int audioSamples = 0;       // audio frames read to the end of the file (0 = no audio track)
    double audioPeak = -1.0;    // loudest decoded audio sample, 0..1 (-1 = no audio, or it could not be decoded)
    double seconds = 0.0;       // the file's own duration
    std::string error;
};
MediaFileInfo InspectMediaFile(const std::string& path);

// The AAC encoder offers 96, 128, 160 and 192 kbit/s (12000, 16000, 20000, 24000 bytes per second):
// the profile's audio bitrate is rounded to the nearest of those.
inline uint32_t AacBytesPerSecond(int kbps) {
    const int choices[4] = {96, 128, 160, 192};
    int best = choices[3];
    for (const int c : choices)
        if (std::abs(kbps - c) < std::abs(kbps - best)) best = c;
    return static_cast<uint32_t>(best) * 125u;
}

// ---- Annex B helpers (shared by the containers) ----------------------------------------

// The NAL units of an Annex B buffer as (offset, size) of each unit's bytes, start code
// excluded.
std::vector<std::pair<size_t, size_t>> SplitAnnexB(const uint8_t* data, size_t size);

// The H.264 NAL unit type (low 5 bits of the first byte).
inline int NalType(const uint8_t* nal) { return nal[0] & 0x1F; }

// ---- H.264 encoder ------------------------------------------------------------------------

class H264Encoder {
public:
    ~H264Encoder();
    H264Encoder(const H264Encoder&) = delete;
    H264Encoder& operator=(const H264Encoder&) = delete;

    // Opens the encoder of `kind` for `width` x `height` (both even) at `fpsNum`/1000 fps
    // and `bitrateKbps`. A kind the machine does not have is refused with the reason.
    static Result<void> Create(EncoderKind kind, int width, int height, uint32_t fpsNum,
                               int bitrateKbps, std::unique_ptr<H264Encoder>& out);

    // Encodes one NV12 picture; whatever pictures the encoder has ready are appended to
    // `out` (a hardware encoder usually answers a few frames later).
    Result<void> Encode(const uint8_t* nv12, size_t bytes, int64_t timeHns, int64_t durationHns,
                        std::vector<EncodedVideo>& out);
    // Flushes the encoder at the end of the recording.
    Result<void> Drain(std::vector<EncodedVideo>& out);

    EncoderKind Kind() const;
    // The encoder's own name ("NVIDIA H.264 Encoder MFT").
    const std::string& Name() const;

private:
    H264Encoder();
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// ---- AAC encoder ------------------------------------------------------------------------

class AacEncoder {
public:
    ~AacEncoder();
    AacEncoder(const AacEncoder&) = delete;
    AacEncoder& operator=(const AacEncoder&) = delete;

    // Stereo AAC-LC at 44.1 or 48 kHz, `bytesPerSecond` of 12000, 16000, 20000 or 24000.
    static Result<void> Create(uint32_t rateHz, uint32_t bytesPerSecond,
                               std::unique_ptr<AacEncoder>& out);

    // Encodes interleaved 16-bit stereo PCM; the AAC frames that are ready are appended.
    Result<void> Encode(const int16_t* pcm, size_t frames, int64_t timeHns,
                        std::vector<EncodedAudio>& out);
    Result<void> Drain(std::vector<EncodedAudio>& out);

    // The two-byte AudioSpecificConfig the Matroska / MP4 track header carries.
    const std::vector<uint8_t>& AudioSpecificConfig() const;

private:
    AacEncoder();
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace bps::recording
