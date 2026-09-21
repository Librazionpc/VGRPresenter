#include "modules/import/Archive.hpp"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <format>

#ifdef BPS_HAVE_ZLIB
#include <zlib.h>
#endif

namespace bps::import {

namespace {

constexpr const char* kModule = "Archive";

uint16_t U16(std::string_view b, size_t at) {
    return static_cast<uint16_t>(static_cast<uint8_t>(b[at]) | (static_cast<uint8_t>(b[at + 1]) << 8));
}
uint32_t U32(std::string_view b, size_t at) {
    return static_cast<uint32_t>(U16(b, at)) | (static_cast<uint32_t>(U16(b, at + 2)) << 16);
}

void AppendUtf8(std::string& out, uint32_t cp) {
    if (cp < 0x80) out += static_cast<char>(cp);
    else if (cp < 0x800) { out += static_cast<char>(0xC0 | (cp >> 6)); out += static_cast<char>(0x80 | (cp & 0x3F)); }
    else if (cp < 0x10000) { out += static_cast<char>(0xE0 | (cp >> 12)); out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F)); out += static_cast<char>(0x80 | (cp & 0x3F)); }
    else { out += static_cast<char>(0xF0 | (cp >> 18)); out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F)); out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F)); out += static_cast<char>(0x80 | (cp & 0x3F)); }
}

bool ValidUtf8(std::string_view s) {
    size_t i = 0;
    while (i < s.size()) {
        const uint8_t c = static_cast<uint8_t>(s[i]);
        size_t extra = 0;
        if (c < 0x80) extra = 0;
        else if ((c & 0xE0) == 0xC0 && c >= 0xC2) extra = 1;
        else if ((c & 0xF0) == 0xE0) extra = 2;
        else if ((c & 0xF8) == 0xF0 && c <= 0xF4) extra = 3;
        else return false;
        for (size_t k = 1; k <= extra; ++k)
            if (i + k >= s.size() || (static_cast<uint8_t>(s[i + k]) & 0xC0) != 0x80) return false;
        i += extra + 1;
    }
    return true;
}

// Windows-1252 0x80..0x9F (the rest matches Latin-1).
constexpr uint32_t kCp1252[32] = {
    0x20AC, 0x81, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021, 0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0x8D, 0x017D, 0x8F,
    0x90, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014, 0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0x9D, 0x017E, 0x0178,
};

#ifdef BPS_HAVE_ZLIB
Result<std::string> Inflate(std::string_view in, size_t expected, size_t limit) {
    z_stream zs{};
    if (inflateInit2(&zs, -MAX_WBITS) != Z_OK) return Error::Make(Err::ParseError, kModule, "inflateInit failed");
    zs.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(in.data()));
    zs.avail_in = static_cast<uInt>(in.size());
    std::string out;
    out.reserve(std::min(expected, limit));
    char buffer[16384];
    int rc = Z_OK;
    while (rc != Z_STREAM_END) {
        zs.next_out = reinterpret_cast<Bytef*>(buffer);
        zs.avail_out = sizeof(buffer);
        rc = inflate(&zs, Z_NO_FLUSH);
        if (rc != Z_OK && rc != Z_STREAM_END) {
            inflateEnd(&zs);
            return Error::Make(Err::ParseError, kModule, std::format("the archive is damaged (zlib rc {})", rc));
        }
        out.append(buffer, sizeof(buffer) - zs.avail_out);
        if (out.size() > limit) {
            inflateEnd(&zs);
            return Error::Make(Err::ParseError, kModule, "a file in the archive is too big");
        }
    }
    inflateEnd(&zs);
    return out;
}
#endif

} // namespace

bool LooksLikeZip(std::string_view bytes) {
    return bytes.size() >= 4 && bytes[0] == 'P' && bytes[1] == 'K' && (bytes[2] == 3 || bytes[2] == 5);
}

Result<std::vector<ArchiveEntry>> ReadZip(std::string_view bytes, size_t maxEntryBytes) {
    if (!LooksLikeZip(bytes)) return Error::Make(Err::ParseError, kModule, "this is not a zip archive");

    // the end-of-central-directory record: the last 22..(22+65535) bytes
    size_t eocd = std::string_view::npos;
    const size_t floor = bytes.size() > 65557 ? bytes.size() - 65557 : 0;
    for (size_t at = bytes.size() >= 22 ? bytes.size() - 22 : 0; ; --at) {
        if (U32(bytes, at) == 0x06054b50u) { eocd = at; break; }
        if (at == floor) break;
    }
    if (eocd == std::string_view::npos || eocd + 22 > bytes.size()) return Error::Make(Err::ParseError, kModule, "the archive has no directory");
    const size_t count = U16(bytes, eocd + 10);
    size_t cd = U32(bytes, eocd + 16);

    std::vector<ArchiveEntry> out;
    for (size_t n = 0; n < count; ++n) {
        if (cd + 46 > bytes.size() || U32(bytes, cd) != 0x02014b50u) return Error::Make(Err::ParseError, kModule, "the archive's directory is damaged");
        const uint16_t method = U16(bytes, cd + 10);
        const size_t compressed = U32(bytes, cd + 20);
        const size_t expected = U32(bytes, cd + 24);
        const size_t nameLen = U16(bytes, cd + 28), extraLen = U16(bytes, cd + 30), commentLen = U16(bytes, cd + 32);
        const size_t local = U32(bytes, cd + 42);
        if (cd + 46 + nameLen > bytes.size()) return Error::Make(Err::ParseError, kModule, "the archive's directory is damaged");
        std::string name(bytes.substr(cd + 46, nameLen));
        cd += 46 + nameLen + extraLen + commentLen;
        if (name.empty() || name.back() == '/') continue;   // a folder

        if (local + 30 > bytes.size() || U32(bytes, local) != 0x04034b50u) return Error::Make(Err::ParseError, kModule, std::format("'{}' is damaged", name));
        const size_t dataAt = local + 30 + U16(bytes, local + 26) + U16(bytes, local + 28);
        if (dataAt + compressed > bytes.size()) return Error::Make(Err::ParseError, kModule, std::format("'{}' is cut short", name));
        const std::string_view raw = bytes.substr(dataAt, compressed);

        ArchiveEntry entry;
        entry.name = std::move(name);
        if (method == 0) {
            if (raw.size() > maxEntryBytes) return Error::Make(Err::ParseError, kModule, "a file in the archive is too big");
            entry.data.assign(raw);
        } else if (method == 8) {
#ifdef BPS_HAVE_ZLIB
            auto inflated = Inflate(raw, expected, maxEntryBytes);
            if (!inflated.ok()) return inflated.error();
            entry.data = std::move(inflated.value());
#else
            (void)expected;
            return Error::Make(Err::Unsupported, kModule, "this build cannot read compressed archives");
#endif
        } else {
            return Error::Make(Err::Unsupported, kModule, std::format("'{}' uses compression method {} that is not supported", entry.name, method));
        }
        out.push_back(std::move(entry));
    }
    return out;
}

std::string ToUtf8(std::string_view bytes) {
    if (bytes.size() >= 3 && static_cast<uint8_t>(bytes[0]) == 0xEF && static_cast<uint8_t>(bytes[1]) == 0xBB && static_cast<uint8_t>(bytes[2]) == 0xBF)
        return std::string(bytes.substr(3));
    if (bytes.size() >= 2 && ((static_cast<uint8_t>(bytes[0]) == 0xFF && static_cast<uint8_t>(bytes[1]) == 0xFE) || (static_cast<uint8_t>(bytes[0]) == 0xFE && static_cast<uint8_t>(bytes[1]) == 0xFF))) {
        const bool little = static_cast<uint8_t>(bytes[0]) == 0xFF;
        std::string out;
        for (size_t i = 2; i + 1 < bytes.size(); i += 2) {
            uint32_t unit = little ? U16(bytes, i) : static_cast<uint32_t>((static_cast<uint8_t>(bytes[i]) << 8) | static_cast<uint8_t>(bytes[i + 1]));
            if (unit >= 0xD800 && unit < 0xDC00 && i + 3 < bytes.size()) {
                const uint32_t low = little ? U16(bytes, i + 2) : static_cast<uint32_t>((static_cast<uint8_t>(bytes[i + 2]) << 8) | static_cast<uint8_t>(bytes[i + 3]));
                if (low >= 0xDC00 && low < 0xE000) { unit = 0x10000 + ((unit - 0xD800) << 10) + (low - 0xDC00); i += 2; }
            }
            AppendUtf8(out, unit);
        }
        return out;
    }
    if (ValidUtf8(bytes)) return std::string(bytes);
    std::string out;
    out.reserve(bytes.size() + bytes.size() / 8);
    for (unsigned char c : bytes) AppendUtf8(out, c >= 0x80 && c < 0xA0 ? kCp1252[c - 0x80] : c);
    return out;
}

std::string Base64Decode(std::string_view text) {
    std::string out;
    uint32_t bits = 0;
    int count = 0;
    for (char c : text) {
        int v;
        if (c >= 'A' && c <= 'Z') v = c - 'A';
        else if (c >= 'a' && c <= 'z') v = c - 'a' + 26;
        else if (c >= '0' && c <= '9') v = c - '0' + 52;
        else if (c == '+' || c == '-') v = 62;
        else if (c == '/' || c == '_') v = 63;
        else continue;
        bits = (bits << 6) | static_cast<uint32_t>(v);
        if (++count == 4) {
            out += static_cast<char>((bits >> 16) & 0xFF);
            out += static_cast<char>((bits >> 8) & 0xFF);
            out += static_cast<char>(bits & 0xFF);
            bits = 0; count = 0;
        }
    }
    if (count == 2) out += static_cast<char>((bits >> 4) & 0xFF);
    else if (count == 3) { out += static_cast<char>((bits >> 10) & 0xFF); out += static_cast<char>((bits >> 2) & 0xFF); }
    return out;
}

} // namespace bps::import
