#include "modules/rendering/PngCodec.hpp"

#ifdef BPS_HAVE_ZLIB
#include <zlib.h>
#endif

#include <cstring>
#include <format>

namespace bps::rendering {

namespace {

// CRC32 used by PNG chunk integrity (zlib provides crc32 when available).
uint32_t Crc32(const uint8_t* data, size_t len) {
#ifdef BPS_HAVE_ZLIB
    return static_cast<uint32_t>(::crc32(0L, data, static_cast<uInt>(len)));
#else
    (void)data; (void)len;
    return 0;
#endif
}

struct Chunk {
    uint32_t length = 0;
    char type[5] = {};
    const uint8_t* data = nullptr;
};

// Reads the next chunk from the stream. Returns false on end of stream or
// malformed data. `pos` is advanced past the whole chunk.
bool NextChunk(const uint8_t* data, size_t len, size_t& pos, Chunk& out) {
    if (pos + 12 > len) return false;
    auto rd32 = [](const uint8_t* p) {
        return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
               (static_cast<uint32_t>(p[2]) << 8) | static_cast<uint32_t>(p[3]);
    };
    out.length = rd32(data + pos);
    std::memcpy(out.type, data + pos + 4, 4);
    out.type[4] = '\0';
    if (pos + 12 + out.length > len) return false;
    out.data = data + pos + 8;
    // Verify the chunk CRC (skip for the well-formed streams we control? No —
    // real files carry CRCs; a mismatch means corruption).
    uint32_t crc = rd32(data + pos + 8 + out.length);
    uint32_t expected = Crc32(data + pos + 4, out.length + 4);
    if (crc != expected) return false;
    pos += 12 + out.length;
    return true;
}

struct Ihdr {
    uint32_t width = 0;
    uint32_t height = 0;
    uint8_t bitDepth = 0;
    uint8_t colorType = 0;
    uint8_t interlace = 0;
};

// Channels per pixel for each color type (at 8-bit).
int ChannelsFor(uint8_t colorType) {
    switch (colorType) {
        case 0:  return 1;   // grayscale
        case 2:  return 3;   // RGB
        case 3:  return 1;   // palette (index)
        case 4:  return 2;   // grayscale + alpha
        case 6:  return 4;   // RGBA
        default: return 0;
    }
}

// Unfilter one scanline into `out` (channels * bpp bytes per pixel, 8-bit).
void Unfilter(uint8_t filter, const uint8_t* prev, const uint8_t* cur,
              uint8_t* out, size_t bytesPerLine, size_t bpp) {
    switch (filter) {
        case 0:   // None
            std::memcpy(out, cur, bytesPerLine);
            break;
        case 1: { // Sub
            for (size_t i = 0; i < bytesPerLine; ++i) {
                int left = (i >= bpp) ? out[i - bpp] : 0;
                out[i] = static_cast<uint8_t>(cur[i] + left);
            }
            break;
        }
        case 2: { // Up
            for (size_t i = 0; i < bytesPerLine; ++i) {
                int up = prev ? prev[i] : 0;
                out[i] = static_cast<uint8_t>(cur[i] + up);
            }
            break;
        }
        case 3: { // Average
            for (size_t i = 0; i < bytesPerLine; ++i) {
                int left = (i >= bpp) ? out[i - bpp] : 0;
                int up = prev ? prev[i] : 0;
                out[i] = static_cast<uint8_t>(cur[i] + ((left + up) >> 1));
            }
            break;
        }
        case 4: { // Paeth
            for (size_t i = 0; i < bytesPerLine; ++i) {
                int a = (i >= bpp) ? out[i - bpp] : 0;
                int b = prev ? prev[i] : 0;
                int c = (i >= bpp && prev) ? prev[i - bpp] : 0;
                int p = a + b - c;
                int pa = p > a ? p - a : a - p;
                int pb = p > b ? p - b : b - p;
                int pc = p > c ? p - c : c - p;
                int predictor = (pa <= pb && pa <= pc) ? a : (pb <= pc ? b : c);
                out[i] = static_cast<uint8_t>(cur[i] + predictor);
            }
            break;
        }
        default:
            break;   // corrupt filter byte -> caller fills zeros
    }
}

#ifdef BPS_HAVE_ZLIB
// zlib-inflates the concatenated IDAT payloads into one buffer, never producing
// more than maxOut bytes (decompression-bomb guard — a crafted but valid
// deflate stream could otherwise expand to gigabytes from a tiny input).
Result<std::vector<uint8_t>> InflateIdat(const std::vector<uint8_t>& idat,
                                         size_t maxOut) {
    z_stream zs;
    std::memset(&zs, 0, sizeof zs);
    if (inflateInit(&zs) != Z_OK)
        return Error::Make(Err::ParseError, "PngCodec", "inflateInit failed");
    zs.next_in = const_cast<Bytef*>(idat.data());
    zs.avail_in = static_cast<uInt>(idat.size());
    std::vector<uint8_t> out;
    out.reserve(std::min<size_t>(maxOut, idat.size() * 4));
    uint8_t buf[16384];
    int rc = Z_OK;
    while (rc == Z_OK) {
        zs.next_out = buf;
        zs.avail_out = sizeof buf;
        rc = inflate(&zs, Z_NO_FLUSH);
        size_t got = sizeof buf - zs.avail_out;
        if (out.size() + got > maxOut) {
            inflateEnd(&zs);
            return Error::Make(Err::ParseError, "PngCodec",
                               "IDAT expands beyond the image dimensions");
        }
        out.insert(out.end(), buf, buf + got);
        if (rc == Z_STREAM_END) break;
        if (rc != Z_OK && rc != Z_BUF_ERROR) {
            inflateEnd(&zs);
            return Error::Make(Err::ParseError, "PngCodec",
                               std::format("inflate failed (zlib rc {})", rc));
        }
        if (rc == Z_BUF_ERROR && zs.avail_in == 0) {
            inflateEnd(&zs);
            return Error::Make(Err::ParseError, "PngCodec", "truncated IDAT stream");
        }
    }
    inflateEnd(&zs);
    return Result<std::vector<uint8_t>>{std::move(out)};
}
#endif

// Converts a raw (unfiltered, 8-bit) scanline to RGBA8 into `rgba` at row `y`.
void ScanlineToRgba(const uint8_t* scan, size_t w, uint8_t colorType, uint8_t bitDepth,
                    const std::vector<uint8_t>& palette,
                    uint32_t* rgba, size_t y, size_t stride) {
    auto put = [&](size_t x, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
        rgba[y * stride + x] = (static_cast<uint32_t>(a) << 24) |
                               (static_cast<uint32_t>(b) << 16) |
                               (static_cast<uint32_t>(g) << 8) | r;
    };
    size_t bpp = static_cast<size_t>(ChannelsFor(colorType)) * (bitDepth == 16 ? 2 : 1);

    if (bitDepth == 16) {
        // 16-bit channels: take the high byte (standard practice).
        for (size_t x = 0; x < w; ++x) {
            size_t o = x * bpp;
            auto hi = [&](size_t c) { return scan[o + c * 2]; };
            switch (colorType) {
                case 0:  put(x, hi(0), hi(0), hi(0), 255); break;
                case 2:  put(x, hi(0), hi(1), hi(2), 255); break;
                case 4:  put(x, hi(0), hi(0), hi(0), hi(1)); break;
                case 6:  put(x, hi(0), hi(1), hi(2), hi(3)); break;
                default: put(x, 0, 0, 0, 0); break;
            }
        }
        return;
    }

    if (bitDepth < 8) {
        // Packed grayscale (bit depths 1/2/4).
        size_t bits = bitDepth;
        size_t maxv = (1u << bits) - 1;
        for (size_t x = 0; x < w; ++x) {
            size_t byteIdx = (x * bits) >> 3;
            size_t bitOff = 8 - bits - ((x * bits) & 7);
            uint8_t v = (scan[byteIdx] >> bitOff) & maxv;
            uint8_t g = static_cast<uint8_t>((v * 255) / maxv);
            put(x, g, g, g, 255);
        }
        return;
    }

    for (size_t x = 0; x < w; ++x) {
        size_t o = x * bpp;
        switch (colorType) {
            case 0:
                put(x, scan[o], scan[o], scan[o], 255);
                break;
            case 2:
                put(x, scan[o], scan[o + 1], scan[o + 2], 255);
                break;
            case 3: {
                size_t idx = scan[o];
                if (idx * 4 + 3 < palette.size())
                    put(x, palette[idx * 4], palette[idx * 4 + 1], palette[idx * 4 + 2],
                        palette[idx * 4 + 3]);
                else
                    put(x, 0, 0, 0, 0);
                break;
            }
            case 4:
                put(x, scan[o], scan[o], scan[o], scan[o + 1]);
                break;
            case 6:
                put(x, scan[o], scan[o + 1], scan[o + 2], scan[o + 3]);
                break;
            default:
                put(x, 0, 0, 0, 0);
                break;
        }
    }
}

} // namespace

bool LooksLikePng(const uint8_t* data, size_t len) {
    static const uint8_t kSig[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    return data && len >= 8 && std::memcmp(data, kSig, 8) == 0;
}

Result<RgbaImage> DecodePng(const uint8_t* data, size_t len) {
    if (!data || len < 8) return Error::Make(Err::ParseError, "PngCodec", "too short");
    if (!LooksLikePng(data, len))
        return Error::Make(Err::ParseError, "PngCodec", "not a PNG file");
#ifndef BPS_HAVE_ZLIB
    (void)len;
    return Error::Make(Err::Unsupported, "PngCodec",
                       "PNG decode unavailable (zlib not found at build time)");
#else
    size_t pos = 8;
    Chunk c;
    Ihdr ihdr;
    bool haveIhdr = false;
    bool haveIend = false;
    std::vector<uint8_t> idat;
    std::vector<uint8_t> palette;
    std::vector<uint8_t> trns;

    while (NextChunk(data, len, pos, c)) {
        if (std::strcmp(c.type, "IHDR") == 0 && c.length >= 13) {
            auto rd32 = [&](size_t off) {
                return (static_cast<uint32_t>(c.data[off]) << 24) |
                       (static_cast<uint32_t>(c.data[off + 1]) << 16) |
                       (static_cast<uint32_t>(c.data[off + 2]) << 8) |
                       static_cast<uint32_t>(c.data[off + 3]);
            };
            ihdr.width = rd32(0);
            ihdr.height = rd32(4);
            ihdr.bitDepth = c.data[8];
            ihdr.colorType = c.data[9];
            ihdr.interlace = c.data[12];
            haveIhdr = true;
        } else if (std::strcmp(c.type, "PLTE") == 0) {
            palette.assign(c.data, c.data + c.length);
        } else if (std::strcmp(c.type, "tRNS") == 0) {
            trns.assign(c.data, c.data + c.length);
        } else if (std::strcmp(c.type, "IDAT") == 0) {
            idat.insert(idat.end(), c.data, c.data + c.length);
        } else if (std::strcmp(c.type, "IEND") == 0) {
            haveIend = true;
            break;
        }
    }
    if (!haveIhdr || !haveIend || idat.empty())
        return Error::Make(Err::ParseError, "PngCodec", "missing IHDR/IEND/IDAT");
    if (ihdr.width == 0 || ihdr.height == 0 || ihdr.width > 16384 || ihdr.height > 16384)
        return Error::Make(Err::ParseError, "PngCodec", "bad dimensions");
    if (ihdr.interlace != 0)
        return Error::Make(Err::Unsupported, "PngCodec", "interlaced PNG not supported");

    int channels = ChannelsFor(ihdr.colorType);
    if (channels == 0)
        return Error::Make(Err::Unsupported, "PngCodec", "unsupported color type");
    bool validDepth = (ihdr.colorType == 3) ? (ihdr.bitDepth == 1 || ihdr.bitDepth == 2 ||
                                               ihdr.bitDepth == 4 || ihdr.bitDepth == 8)
                                            : (ihdr.bitDepth == 8 || ihdr.bitDepth == 16);
    if (!validDepth)
        return Error::Make(Err::Unsupported, "PngCodec", "unsupported bit depth");

    // For palette images tRNS carries a single alpha channel per palette entry.
    if (ihdr.colorType == 3 && !trns.empty()) {
        for (size_t i = 0; i < trns.size() && (i * 4 + 3) < palette.size(); ++i)
            palette[i * 4 + 3] = trns[i];
    }

    size_t bpp = static_cast<size_t>(channels) * (ihdr.bitDepth == 16 ? 2 : 1);
    if (ihdr.bitDepth < 8) bpp = 1;
    size_t bytesPerLine = ((static_cast<size_t>(ihdr.width) * ihdr.bitDepth * channels) + 7) / 8;
    size_t stride = static_cast<size_t>(ihdr.width);
    // Exact expected raw size; cap the inflate at it (bomb guard) and require
    // at least that much back out (truncated/corrupt stream).
    size_t expected = (bytesPerLine + 1) * ihdr.height;
    auto raw = InflateIdat(idat, expected);
    if (!raw.ok()) return raw.error();
    if (raw.value().size() < expected)
        return Error::Make(Err::ParseError, "PngCodec", "IDAT too small for image");

    RgbaImage img;
    img.width = static_cast<int>(ihdr.width);
    img.height = static_cast<int>(ihdr.height);
    img.pixels.assign(stride * ihdr.height, 0);

    std::vector<uint8_t> prev(bytesPerLine, 0);
    const uint8_t* src = raw.value().data();
    for (uint32_t y = 0; y < ihdr.height; ++y) {
        uint8_t filter = src[y * (bytesPerLine + 1)];
        const uint8_t* cur = src + y * (bytesPerLine + 1) + 1;
        std::vector<uint8_t> unf(bytesPerLine, 0);
        Unfilter(filter, y == 0 ? nullptr : prev.data(), cur, unf.data(), bytesPerLine, bpp);
        ScanlineToRgba(unf.data(), ihdr.width, ihdr.colorType, ihdr.bitDepth, palette,
                       img.pixels.data(), y, stride);
        prev = unf;
    }
    return Result<RgbaImage>{std::move(img)};
#endif  // BPS_HAVE_ZLIB
}

// ---------------------------------------------------------------------------
// Encoder
// ---------------------------------------------------------------------------

Result<std::vector<uint8_t>> EncodePngRgba8(const uint8_t* rgba, int width, int height) {
    if (!rgba || width <= 0 || height <= 0)
        return Error::Make(Err::InvalidArgument, "PngCodec", "empty image");
#ifndef BPS_HAVE_ZLIB
    return Error::Make(Err::Unsupported, "PngCodec", "PNG encoding needs zlib");
#else
    const size_t stride = static_cast<size_t>(width) * 4;
    std::vector<uint8_t> raw;
    raw.reserve((stride + 1) * static_cast<size_t>(height));
    for (int y = 0; y < height; ++y) {
        raw.push_back(0);   // filter type 0 (None) for every row
        raw.insert(raw.end(), rgba + static_cast<size_t>(y) * stride,
                   rgba + (static_cast<size_t>(y) + 1) * stride);
    }
    uLongf packedSize = compressBound(static_cast<uLong>(raw.size()));
    std::vector<uint8_t> packed(packedSize);
    if (compress2(packed.data(), &packedSize, raw.data(), static_cast<uLong>(raw.size()), 6) != Z_OK)
        return Error::Make(Err::IoError, "PngCodec", "deflate failed");
    packed.resize(packedSize);

    std::vector<uint8_t> out = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    auto put32 = [&](uint32_t v) {
        out.push_back(static_cast<uint8_t>(v >> 24));
        out.push_back(static_cast<uint8_t>(v >> 16));
        out.push_back(static_cast<uint8_t>(v >> 8));
        out.push_back(static_cast<uint8_t>(v));
    };
    auto chunk = [&](const char* type, const std::vector<uint8_t>& data) {
        put32(static_cast<uint32_t>(data.size()));
        const size_t typeAt = out.size();
        out.insert(out.end(), type, type + 4);
        out.insert(out.end(), data.begin(), data.end());
        // CRC covers the type and the data. (Not called for an empty payload: zlib's crc32 treats
        // a null buffer as "start over" and returns 0.)
        uLong crc = ::crc32(0L, out.data() + typeAt, 4);
        if (!data.empty()) crc = ::crc32(crc, data.data(), static_cast<uInt>(data.size()));
        put32(static_cast<uint32_t>(crc));
    };

    std::vector<uint8_t> ihdr;
    auto hdr32 = [&](uint32_t v) {
        ihdr.push_back(static_cast<uint8_t>(v >> 24));
        ihdr.push_back(static_cast<uint8_t>(v >> 16));
        ihdr.push_back(static_cast<uint8_t>(v >> 8));
        ihdr.push_back(static_cast<uint8_t>(v));
    };
    hdr32(static_cast<uint32_t>(width));
    hdr32(static_cast<uint32_t>(height));
    ihdr.push_back(8);   // bit depth
    ihdr.push_back(6);   // colour type: RGBA
    ihdr.push_back(0);   // compression
    ihdr.push_back(0);   // filter method
    ihdr.push_back(0);   // no interlace
    chunk("IHDR", ihdr);
    chunk("IDAT", packed);
    chunk("IEND", {});
    return Result<std::vector<uint8_t>>{std::move(out)};
#endif
}

} // namespace bps::rendering
