#pragma once

// FrameSource: "give me the picture of this media file". The one thing the media
// library needs from the operating system - decoding a photo, or a video at a chosen
// point - kept behind an interface so the thumbnail cache (ThumbnailCache) and its
// tests never depend on a codec. The Windows implementation uses Media Foundation
// (a video frame at any position) and falls back to the shell's own preview;
// other platforms get a source that reports Unsupported until one is written.

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

    // The picture of `path` scaled to `targetWidth` pixels wide (aspect kept; never
    // enlarged beyond the source). For a video, `fraction` (0..1) is how far into it to
    // look; images ignore it. Err::NotFound for a missing file, Err::Unsupported when
    // this source cannot decode that file, Err::IoError for any other failure. For AUDIO the picture is its
    // cover art, where the file has one (Unsupported otherwise - the UI shows a music placeholder).
    // Thread-safe: the cache calls it from several threads.
    virtual Result<Picture> Grab(const std::string& path, MediaKind kind, double fraction,
                               int targetWidth) = 0;
};

// The platform's frame source (never null).
std::shared_ptr<IFrameSource> MakePlatformFrameSource();

} // namespace bps::media
