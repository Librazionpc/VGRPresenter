#pragma once

#include "core/common/Common.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace bps::media {
enum class MediaKind : int { Image = 0, Video = 1, Audio = 2 };

// A decoded picture: tightly packed RGBA8, row 0 at the top.
struct Picture {
    int width = 0;
    int height = 0;
    std::vector<uint8_t> rgba;

    bool empty() const { return width <= 0 || height <= 0 || rgba.empty(); }
};

class IFrameSource {
public:
    virtual ~IFrameSource() = default;

    virtual Result<Picture> Grab(const std::string& path, MediaKind kind, double fraction,
                               int targetWidth) = 0;
};

// The platform's frame source (never null).
std::shared_ptr<IFrameSource> MakePlatformFrameSource();
} // namespace bps::media