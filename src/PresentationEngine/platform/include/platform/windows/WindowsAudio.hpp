#pragma once

// Windows PAL backend for the audio device subsystem (winmm waveIn/waveOut
// enumeration — no mixing, no playback).

#include "platform/IAudio.hpp"

namespace bps::platform {

class WindowsAudio final : public IAudio {
public:
    std::vector<AudioDeviceInfo> Enumerate() const override;
    Result<AudioDeviceInfo> DefaultOutput() const override;
    Result<AudioDeviceInfo> DefaultInput() const override;
    std::string Fingerprint() const override;
};

} // namespace bps::platform
