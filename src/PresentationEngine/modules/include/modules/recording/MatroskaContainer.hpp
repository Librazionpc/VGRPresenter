#pragma once

// The MKV (Matroska) recording container. Windows has no Matroska writer, so this one
// writes the file itself: EBML header, SeekHead, Info, Tracks (H.264 + AAC), Clusters of
// SimpleBlocks, and Cues at the end so the file seeks. The pictures come from
// H264Encoder — NVENC, AMF, QuickSync or the software encoder, whichever the profile
// picked — and the sound from AacEncoder (see MediaFoundationEncode.hpp).
//
// The track header needs the encoder's SPS/PPS, which only exist once the first keyframe
// is out, so the file's head is written then; sound that arrives before it is held.

#include "modules/recording/RecordingPlugin.hpp"

#include <memory>
#include <string>
#include <string_view>

namespace bps::recording {

class MatroskaContainer final : public IRecordingContainer {
public:
    MatroskaContainer();
    ~MatroskaContainer() override;

    const char* Name() const noexcept override { return "mkv"; }
    Result<std::string> Open(const RecordingProfile& profile, std::string_view path) override;
    Result<void> Close(bool finalized) override;
    Result<void> WriteVideo(const VideoFrameIn& frame) override;
    Result<void> WriteAudio(const AudioBlockIn& block) override;

    // The encoder the open file is using ("NVIDIA H.264 Encoder MFT"), "" when closed.
    std::string EncoderName() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace bps::recording
