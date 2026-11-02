#pragma once

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