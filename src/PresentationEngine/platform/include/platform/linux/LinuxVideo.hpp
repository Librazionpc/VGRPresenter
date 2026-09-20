#pragma once

// Linux PAL backend for the video-capture subsystem. V4L2 enumeration from
// /dev/video* (ioctl limits — real per-mode sizes and frame-rate ranges).
// Pending implementation (requires a Linux host to develop and verify —
// same convention as the other pending Linux/macOS pieces, PAL.md §11);
// degrades to an empty roster, which every consumer treats as "no
// cameras" rather than an error.

#include "../IVideo.hpp"

namespace bps::platform {

class LinuxVideo final : public IVideo {
public:
    std::vector<VideoDeviceInfo> Enumerate() const override { return {}; }
    std::string Fingerprint() const override { return {}; }
};

} // namespace bps::platform
