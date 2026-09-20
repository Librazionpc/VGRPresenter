#pragma once

// Windows PAL backend for the video-capture subsystem (Media Foundation
// device + mode enumeration — no streaming, no frame I/O).

#include "platform/IVideo.hpp"

namespace bps::platform {

class WindowsVideo final : public IVideo {
public:
    std::vector<VideoDeviceInfo> Enumerate() const override;
    std::string Fingerprint() const override;
};

} // namespace bps::platform
