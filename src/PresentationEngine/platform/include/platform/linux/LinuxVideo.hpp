#pragma once

#include "../IVideo.hpp"

namespace bps::platform {
class LinuxVideo final : public IVideo {
public:
    std::vector<VideoDeviceInfo> Enumerate() const override { return {}; }
    std::string Fingerprint() const override { return {}; }
};
} // namespace bps::platform