#pragma once

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