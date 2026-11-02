#pragma once

#include "modules/recording/RecordingPlugin.hpp"

#include <memory>
#include <string>
#include <string_view>

namespace bps::recording {
class MediaFoundationMp4 final : public IRecordingContainer {
public:
    MediaFoundationMp4();
    ~MediaFoundationMp4() override;

    const char* Name() const noexcept override { return "mp4"; }
    Result<std::string> Open(const RecordingProfile& profile, std::string_view path) override;
    Result<void> Close(bool finalized) override;
    Result<void> WriteVideo(const VideoFrameIn& frame) override;
    Result<void> WriteAudio(const AudioBlockIn& block) override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// Whether this machine can encode H.264 / AAC through Media Foundation. The error
// names the missing encoder.
Result<void> ProbeH264Encoder();
Result<void> ProbeAacEncoder();
} // namespace bps::recording