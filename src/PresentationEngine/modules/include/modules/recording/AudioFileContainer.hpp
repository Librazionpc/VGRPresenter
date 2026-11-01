#pragma once

// The audio-only recording containers — what an AUDIO preset records to:
//
//   WAV   uncompressed 16-bit PCM, written here (RIFF header patched when the file closes)
//   M4A   AAC audio in an MPEG-4 file, through Media Foundation's sink writer
//   MP3   MP3, through the same writer and the system's MP3 encoder
//
// All three take the output mix the recording is listening to (the buses wired to the
// output) and ignore pictures. The mix's own sample rate is the file's rate.

#include "modules/recording/RecordingPlugin.hpp"

#include <memory>
#include <string>
#include <string_view>

namespace bps::recording {

class AudioFileContainer final : public IRecordingContainer {
public:
    // `kind` is WAV, M4A or MP3.
    explicit AudioFileContainer(ContainerKind kind);
    ~AudioFileContainer() override;

    const char* Name() const noexcept override;
    Result<std::string> Open(const RecordingProfile& profile, std::string_view path) override;
    Result<void> Close(bool finalized) override;
    Result<void> WriteVideo(const VideoFrameIn& frame) override;   // an audio file has no picture
    Result<void> WriteAudio(const AudioBlockIn& block) override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// Whether this machine has an MP3 encoder (the MP3 format is offered only when it does).
bool Mp3EncoderAvailable();

} // namespace bps::recording
