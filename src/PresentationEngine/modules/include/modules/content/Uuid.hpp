#pragma once

// v4-style random UUID used by CAMS (docs/specs/13). Header-only, dependency-free.

#include <cstdint>
#include <cstdio>
#include <random>
#include <string>

namespace bps::content {

struct Uuid {
    uint64_t hi = 0;   // first 8 bytes
    uint64_t lo = 0;   // last 8 bytes

    static Uuid Generate() {
        static thread_local std::mt19937_64 rng{std::random_device{}()};
        Uuid u;
        u.hi = rng();
        u.lo = rng();
        // RFC 4122 v4: version 4 in the upper 4 bits of byte 6, variant 10xx
        // in the upper 2 bits of byte 8.
        u.hi = (u.hi & 0xFFFFFFFFFFFF0FFFULL) | 0x0000000000004000ULL;
        u.lo = (u.lo & 0x3FFFFFFFFFFFFFFFULL) | 0x8000000000000000ULL;
        return u;
    }

    std::string ToString() const {
        char buf[40];
        std::snprintf(buf, sizeof buf,
                      "%08x-%04x-%04x-%04x-%012llx",
                      static_cast<unsigned>(hi >> 32),
                      static_cast<unsigned>((hi >> 16) & 0xFFFF),
                      static_cast<unsigned>(hi & 0xFFFF),
                      static_cast<unsigned>(lo >> 48),
                      static_cast<unsigned long long>(lo & 0xFFFFFFFFFFFFULL));
        return buf;
    }

    static Uuid FromString(std::string_view s) {
        Uuid u{};
        unsigned a = 0, b = 0, c = 0, d = 0;
        unsigned long long e = 0;
        if (std::sscanf(s.data(), "%8x-%4x-%4x-%4x-%12llx", &a, &b, &c, &d, &e) == 5) {
            u.hi = (static_cast<uint64_t>(a) << 32) | (static_cast<uint64_t>(b) << 16) | c;
            u.lo = (static_cast<uint64_t>(d) << 48) | e;
        }
        return u;
    }

    bool operator==(const Uuid& o) const { return hi == o.hi && lo == o.lo; }
    bool operator!=(const Uuid& o) const { return !(*this == o); }
    bool operator<(const Uuid& o) const {
        return hi < o.hi || (hi == o.hi && lo < o.lo);
    }
};

} // namespace bps::content

namespace std {
template <>
struct hash<bps::content::Uuid> {
    size_t operator()(const bps::content::Uuid& u) const noexcept {
        return static_cast<size_t>(u.hi ^ (u.lo * 0x9E3779B97F4A7C15ULL));
    }
};
} // namespace std
