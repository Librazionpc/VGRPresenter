#pragma once

// Linux PAL backend for the audio device subsystem. Enumerates ALSA PCM
// streams from /proc/asound/pcm (input = capture streams, output = playback
// streams). Sample rates/channels are read from active hw_params when a
// stream is open, and — for idle devices — probed through the runtime-loaded
// libasound library (PAL ILibrary seam; degrades to 0 = unknown when the
// library is absent).

#include "../IAudio.hpp"

namespace bps::platform {

class LinuxAudio final : public IAudio {
public:
    std::vector<AudioDeviceInfo> Enumerate() const override;
    Result<AudioDeviceInfo> DefaultOutput() const override;
    Result<AudioDeviceInfo> DefaultInput() const override;
    std::string Fingerprint() const override;
};

} // namespace bps::platform
