#pragma once

// AssetCompressor (docs/specs/13 §Compression): pluggable compressors for
// package/cache/metadata compression. Ships Stored (no-op), RLE (byte-run)
// and Deflate (zlib, RFC 1951 — compiled when ZLIB is found at build time).
// zstd remains a documented future backend behind the same interface.

#include "core/common/Common.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace bps::content {

class ICompressor {
public:
    virtual ~ICompressor() = default;
    virtual const char* Name() const noexcept = 0;
    virtual std::vector<uint8_t> Compress(const uint8_t* data, size_t len) const = 0;
    virtual Result<std::vector<uint8_t>> Decompress(const uint8_t* data, size_t len) const = 0;
};

class StoredCompressor final : public ICompressor {
public:
    const char* Name() const noexcept override { return "stored"; }
    std::vector<uint8_t> Compress(const uint8_t* data, size_t len) const override {
        return std::vector<uint8_t>(data, data + len);
    }
    Result<std::vector<uint8_t>> Decompress(const uint8_t* data, size_t len) const override {
        return Result<std::vector<uint8_t>>{std::vector<uint8_t>(data, data + len)};
    }
};

// Simple byte-run-length encoding: [count:varint][byte] repeats. Good for
// repetitive content (lyrics, metadata); worst case slightly larger than input.
class RleCompressor final : public ICompressor {
public:
    const char* Name() const noexcept override { return "rle"; }
    std::vector<uint8_t> Compress(const uint8_t* data, size_t len) const override;
    Result<std::vector<uint8_t>> Decompress(const uint8_t* data, size_t len) const override;
};

// zlib deflate (RFC 1951) backend. Compiled when ZLIB is available at build
// time (BPS_HAVE_ZLIB, set by CMake); on hosts without zlib Compress falls
// back to a stored copy and Decompress reports Err::Unsupported so callers
// degrade to Stored/RLE. Level 6 is the default balance of ratio vs speed.
class DeflateCompressor final : public ICompressor {
public:
    const char* Name() const noexcept override { return "deflate"; }
    std::vector<uint8_t> Compress(const uint8_t* data, size_t len) const override;
    Result<std::vector<uint8_t>> Decompress(const uint8_t* data, size_t len) const override;
};

} // namespace bps::content
