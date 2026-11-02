#pragma once

#include "core/common/Common.hpp"
#include "modules/rendering/IGraphicsBackend.hpp"

#include <cstdint>
#include <vector>

namespace bps::rendering {
// Decodes a complete PNG file buffer into an RGBA8 image.
// Returns Err::Unsupported when zlib is not available at build time.
Result<RgbaImage> DecodePng(const uint8_t* data, size_t len);

// True when the buffer starts with a PNG signature (cheap sniff for loaders).
bool LooksLikePng(const uint8_t* data, size_t len);

// Encodes tightly packed RGBA8 pixels (width * height * 4 bytes, row 0 first) as a PNG file.
// Used for cached thumbnails. Err::Unsupported when zlib is not available at build time.
Result<std::vector<uint8_t>> EncodePngRgba8(const uint8_t* rgba, int width, int height);
} // namespace bps::rendering