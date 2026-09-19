#pragma once

// PngCodec (docs/specs/13 §Asset Loader + docs/specs/17 §Image rendering):
// a self-contained PNG decoder that turns PNG bytes into an RGBA8 RgbaImage.
// PNG's IDAT stream is DEFLATE, so the decoder reuses zlib (compiled when
// BPS_HAVE_ZLIB is set; otherwise Err::Unsupported). No libpng dependency —
// the chunk parser, unfiltering (all five filter types) and color conversion
// are implemented here. Handles bit depths 1/2/4/8/16 and color types
// grayscale, grayscale+alpha, palette, RGB and RGBA; 16-bit channels are
// reduced to 8-bit. Corrupt input fails cleanly (Err::ParseError) and never
// reads out of bounds.

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

} // namespace bps::rendering
