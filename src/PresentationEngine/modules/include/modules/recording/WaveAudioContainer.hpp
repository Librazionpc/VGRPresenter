#pragma once

#include "modules/recording/RecordingPlugin.hpp"

#include <cstdint>
#include <memory>
#include <string>

namespace bps::platform { class IFileWriteStream; }

namespace bps::recording {

// Portable 16-bit stereo PCM/WAV writer for audio-only presets.
class WaveAudioContainer final : public IRecordingContainer {
public:
    WaveAudioContainer();
    ~WaveAudioContainer() override;
    WaveAudioContainer(const WaveAudioContainer&) = delete;
    WaveAudioContainer& operator=(const WaveAudioContainer&) = delete;

    const char* Name() const noexcept override { return "wav"; }
    Result<std::string> Open(const RecordingProfile& profile, std::string_view path) override;
    Result<void> Close(bool finalized) override;
    Result<void> WriteVideo(const VideoFrameIn&) override { return Ok(); }
    Result<void> WriteAudio(const AudioBlockIn& block) override;

private:
    std::unique_ptr<bps::platform::IFileWriteStream> stream_;
    std::string path_;
    uint32_t rateHz_ = 0;
    uint64_t dataBytes_ = 0;
};

} // namespace bps::recording
