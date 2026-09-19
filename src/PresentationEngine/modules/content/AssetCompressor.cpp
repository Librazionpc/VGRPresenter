#include "modules/content/AssetCompressor.hpp"

#ifdef BPS_HAVE_ZLIB
#include <zlib.h>
#endif

#include <cstring>
#include <format>

namespace bps::content {

namespace {

void PutVarint(std::vector<uint8_t>& out, uint32_t v) {
    while (v >= 0x80) {
        out.push_back(static_cast<uint8_t>((v & 0x7F) | 0x80));
        v >>= 7;
    }
    out.push_back(static_cast<uint8_t>(v));
}

bool GetVarint(const uint8_t*& p, const uint8_t* end, uint32_t& out) {
    out = 0;
    int shift = 0;
    while (p < end && shift < 35) {
        uint8_t b = *p++;
        out |= static_cast<uint32_t>(b & 0x7F) << shift;
        if (!(b & 0x80)) return true;
        shift += 7;
    }
    return false;
}

} // namespace

std::vector<uint8_t> RleCompressor::Compress(const uint8_t* data, size_t len) const {
    std::vector<uint8_t> out;
    out.reserve(len / 2 + 16);
    size_t i = 0;
    while (i < len) {
        uint8_t b = data[i];
        size_t run = 1;
        while (i + run < len && data[i + run] == b && run < 0xFFFF) ++run;
        PutVarint(out, static_cast<uint32_t>(run));
        out.push_back(b);
        i += run;
    }
    return out;
}

Result<std::vector<uint8_t>> RleCompressor::Decompress(const uint8_t* data, size_t len) const {
    const uint8_t* p = data;
    const uint8_t* end = data + len;
    std::vector<uint8_t> out;
    out.reserve(len * 2);
    while (p < end) {
        uint32_t run = 0;
        if (!GetVarint(p, end, run)) {
            // Truncated: a trailing byte that can't form a complete run header
            // is dropped (lenient) rather than failing the whole decode.
            break;
        }
        if (p >= end)
            return Error::Make(Err::ParseError, "CAMS", "corrupt RLE stream (missing byte)");
        uint8_t b = *p++;
        out.insert(out.end(), run, b);
    }
    return Result<std::vector<uint8_t>>{std::move(out)};
}

// ---------------------------------------------------------------------------
// DeflateCompressor (zlib, RFC 1951)
// ---------------------------------------------------------------------------

#ifdef BPS_HAVE_ZLIB

std::vector<uint8_t> DeflateCompressor::Compress(const uint8_t* data, size_t len) const {
    if (len == 0) return {};
    uLongf bound = compressBound(static_cast<uLong>(len));
    std::vector<uint8_t> out(bound);
    uLongf destLen = bound;
    int rc = compress2(out.data(), &destLen, data, static_cast<uLong>(len), Z_DEFAULT_COMPRESSION);
    if (rc != Z_OK) return std::vector<uint8_t>(data, data + len);   // degrade to stored
    out.resize(destLen);
    return out;
}

Result<std::vector<uint8_t>> DeflateCompressor::Decompress(const uint8_t* data, size_t len) const {
    if (len == 0) return Result<std::vector<uint8_t>>{std::vector<uint8_t>{}};
    // Grow the output buffer geometrically; a corrupt stream fails cleanly.
    size_t capacity = std::max<size_t>(64, len * 2);
    for (int attempt = 0; attempt < 24; ++attempt) {
        std::vector<uint8_t> out(capacity);
        uLongf destLen = static_cast<uLongf>(capacity);
        int rc = uncompress(out.data(), &destLen, data, static_cast<uLong>(len));
        if (rc == Z_OK) {
            out.resize(destLen);
            return Result<std::vector<uint8_t>>{std::move(out)};
        }
        if (rc != Z_BUF_ERROR)
            return Error::Make(Err::ParseError, "CAMS",
                               std::format("corrupt deflate stream (zlib rc {})", rc));
        capacity *= 4;
        if (capacity > (1u << 30))   // 1 GiB guard
            return Error::Make(Err::ParseError, "CAMS", "deflate output exceeds 1 GiB");
    }
    return Error::Make(Err::ParseError, "CAMS", "deflate output too large");
}

#else   // !BPS_HAVE_ZLIB

std::vector<uint8_t> DeflateCompressor::Compress(const uint8_t* data, size_t len) const {
    return std::vector<uint8_t>(data, data + len);   // stored fallback
}

Result<std::vector<uint8_t>> DeflateCompressor::Decompress(const uint8_t* data, size_t len) const {
    (void)data;
    (void)len;
    return Error::Make(Err::Unsupported, "CAMS",
                       "deflate unavailable (zlib not found at build time)");
}

#endif  // BPS_HAVE_ZLIB

} // namespace bps::content
