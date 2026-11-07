#include "modules/rendering/GraphicsBackends.hpp"

#include "core/common/StageTiming.hpp"
#include "core/logging/Logger.hpp"
#include "modules/rendering/ImageScale.hpp"
#include "platform/IPlatform.hpp"
#include "platform/PlatformAccessor.hpp"

#if defined(__x86_64__) || defined(_M_X64)
#include <immintrin.h>
#endif
#include <algorithm>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <format>
#include <functional>
#include <mutex>
#include <set>
#include <thread>
#include <vector>

namespace bps::rendering {
// ===========================================================================
// Shared pixel helpers
// ===========================================================================

namespace {
inline float Clamp01(float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }
inline uint8_t To8(float v) {
    return static_cast<uint8_t>(v < 0.0f ? 0.0f : (v > 1.0f ? 255.0f : v * 255.0f));
}

// Straight-alpha blend helpers.
inline uint32_t Pack8(uint32_t r, uint32_t g, uint32_t b, uint32_t a) {
    return (a << 24) | (b << 16) | (g << 8) | r;
}

// Scales a packed colour's alpha — the coverage of a partially covered pixel
// (anti-aliasing) or an edge feather.
inline uint32_t ScaleAlpha(uint32_t px, float k) {
    const float a = static_cast<float>((px >> 24) & 0xFF) * k;
    const uint32_t ai = static_cast<uint32_t>(a < 0.0f ? 0.0f : (a > 255.0f ? 255.0f : a));
    return (px & 0x00FFFFFFu) | (ai << 24);
}
} // namespace

namespace {
// ---- shared by the scaled blit, the scaled-image cache and the prefix cache ------------------

// A 64-bit fingerprint of a whole pixel buffer (every byte counts: two textures hash alike only when
// their pixels are identical, which is what lets one video frame, held separately by each output's
// scene, be recognised as ONE picture). ~1 ms per 1080p frame: one multiply per 8 bytes.
inline uint64_t HashPixels(const std::vector<uint32_t>& px, int w, int h) {
    uint64_t hash = 0x9E3779B97F4A7C15ull ^ (static_cast<uint64_t>(static_cast<uint32_t>(w)) << 32)
                    ^ static_cast<uint64_t>(static_cast<uint32_t>(h));
    const size_t words = px.size() / 2;
    const uint64_t* p = reinterpret_cast<const uint64_t*>(px.data());
    // Four independent lanes: one chain of multiply-xor-shift was latency-bound (~3 ms for a 1080p frame);
    // four overlap on the multiplier. Every word still reaches the result.
    uint64_t lane[4] = {hash, hash ^ 0xA5A5A5A5A5A5A5A5ull, hash + 0x1234567ull, hash * 31u + 7u};
    size_t i = 0;
    for (; i + 4 <= words; i += 4) {
        for (int k = 0; k < 4; ++k) {
            lane[k] = (lane[k] ^ p[i + static_cast<size_t>(k)]) * 0x9E3779B185EBCA87ull;
            lane[k] ^= lane[k] >> 32;
        }
    }
    for (; i < words; ++i) {
        lane[0] = (lane[0] ^ p[i]) * 0x9E3779B185EBCA87ull;
        lane[0] ^= lane[0] >> 32;
    }
    hash = lane[0];
    for (int k = 1; k < 4; ++k) {
        hash = (hash ^ lane[k]) * 0x9E3779B185EBCA87ull;
        hash ^= hash >> 29;
    }
    if (px.size() & 1u)
        hash = (hash ^ px.back()) * 0x9E3779B185EBCA87ull;
    return hash;
}

inline uint64_t HashMix(uint64_t hash, uint64_t v) {
    hash = (hash ^ v) * 0x100000001B3ull;
    return hash ^ (hash >> 29);
}

inline uint64_t HashFloat(uint64_t hash, float f) {
    uint32_t bits = 0;
    std::memcpy(&bits, &f, sizeof(bits));
    return HashMix(hash, bits);
}

// One axis of the resample with 16.16 integer weights that sum to exactly 65536.
struct FixedAxisTaps {
    int n = 0;
    int idx[4] = {0, 0, 0, 0};
    uint32_t w[4] = {0, 0, 0, 0};
};

inline FixedAxisTaps ToFixedTaps(const ScaleTaps& t, int extent) {
    FixedAxisTaps f;
    f.n = t.count;
    int64_t sum = 0;
    int heaviest = 0;
    for (int i = 0; i < t.count; ++i) {
        f.idx[i] = std::clamp(t.index[i], 0, extent - 1);
        const double wd = t.weight[i] * 65536.0 + 0.5;
        f.w[i] = static_cast<uint32_t>(wd < 0.0 ? 0.0 : wd);
        sum += f.w[i];
        if (f.w[i] > f.w[heaviest]) heaviest = i;
    }
    if (f.n > 0) {   // the rounding goes to the heaviest tap, so the weights sum to exactly 65536
        const int64_t fixed = static_cast<int64_t>(f.w[heaviest]) + (65536 - sum);
        f.w[heaviest] = static_cast<uint32_t>(fixed < 0 ? 0 : fixed);
    }
    return f;
}

// Runs fn(rowBegin, rowEnd) over [y0, y1) split into bands on a few threads when the job is big
// enough to repay starting them. The caller's bands must write disjoint rows.
// A few long-lived raster workers. A job is a set of independent bands; the CALLING thread claims
// bands too (alongside the workers), so a machine whose cores are all busy never leaves the frame
// waiting on a worker that has not been scheduled: the caller simply does that band itself. The
// workers run above normal priority, like the loop that feeds them. Never destroyed (exit-safe).
class RasterPool {
public:
    static RasterPool& Instance() {
        static RasterPool* pool = new RasterPool();   // intentionally leaked: workers outlive statics
        return *pool;
    }
    unsigned Workers() const { return workers_; }
    // Runs fn(band) for band in [0, bands) and returns when every band has finished.
    void Run(int bands, const std::function<void(int)>& fn) {
        std::lock_guard<std::mutex> serial(runMutex_);   // one job at a time
        job_.fn = &fn;
        job_.bands = bands;
        job_.next.store(0);
        job_.done.store(0);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            ++generation_;
        }
        wake_.notify_all();
        Work();   // the caller takes bands too
        std::unique_lock<std::mutex> lock(mutex_);
        finished_.wait(lock, [&] { return job_.done.load() >= job_.bands; });   // lock-ok: the pool's own mutex_, released by the wait itself; the bands being waited on are in-memory pixel loops (no PAL, provider, file or engine singleton)
        job_.fn = nullptr;
    }

private:
    RasterPool() {
        const unsigned hw = std::max(1u, std::thread::hardware_concurrency());
        workers_ = hw > 1 ? std::min(hw - 1u, 5u) : 0u;
        for (unsigned i = 0; i < workers_; ++i)
            std::thread([this] { WorkerMain(); }).detach();
    }
    void Work() {
        for (;;) {
            const int b = job_.next.fetch_add(1);
            if (b >= job_.bands) return;
            (*job_.fn)(b);
            if (job_.done.fetch_add(1) + 1 >= job_.bands) {
                std::lock_guard<std::mutex> lock(mutex_);
                finished_.notify_all();
            }
        }
    }
    void WorkerMain() {
        if (platform::PlatformAccessor::Installed())
            (void)platform::PlatformAccessor::Get().Threading().SetCurrentThreadPriority(4);
        uint64_t seen = 0;
        for (;;) {
            {
                std::unique_lock<std::mutex> lock(mutex_);
                wake_.wait(lock, [&] { return generation_ != seen; });
                seen = generation_;
            }
            Work();
        }
    }
    struct Job {
        const std::function<void(int)>* fn = nullptr;
        int bands = 0;
        std::atomic<int> next{0};
        std::atomic<int> done{0};
    };
    unsigned workers_ = 0;
    std::mutex runMutex_;
    std::mutex mutex_;
    std::condition_variable wake_;
    std::condition_variable finished_;
    uint64_t generation_ = 0;
    Job job_;
};

template <class Fn>
inline void ForRowBands(int y0, int y1, size_t workPixels, Fn&& fn) {
    const int rows = y1 - y0;
    if (rows <= 0) return;
    RasterPool& pool = RasterPool::Instance();
    const int bands = (workPixels >= 200000u && pool.Workers() > 0)
                          ? std::min<int>(rows, static_cast<int>(pool.Workers()) + 1) : 1;
    if (bands <= 1) {
        fn(y0, y1);
        return;
    }
    const std::function<void(int)> job = [&](int b) {
        const int b0 = y0 + static_cast<int>(static_cast<int64_t>(rows) * b / bands);
        const int b1 = y0 + static_cast<int>(static_cast<int64_t>(rows) * (b + 1) / bands);
        fn(b0, b1);
    };
    pool.Run(bands, job);
}

// One destination row of the two-tap resample: planar 8.8 rows a (weight w0) and b (weight w1,
// w0 + w1 = 256) -> packed opaque pixels. Contiguous arrays and no branches, so it vectorises.
#if defined(__GNUC__) && !defined(__clang__)
__attribute__((optimize("O3")))
#endif
inline void VerticalBlendRow(uint32_t* dst, const uint16_t* a, const uint16_t* b, size_t w, size_t stride,
                             uint32_t w0, uint32_t w1) {
    const uint16_t* aR = a; const uint16_t* aG = a + stride; const uint16_t* aB = a + 2 * stride;
    const uint16_t* bR = b; const uint16_t* bG = b + stride; const uint16_t* bB = b + 2 * stride;
    for (size_t x = 0; x < w; ++x) {
        const uint32_t r = (aR[x] * w0 + bR[x] * w1 + 0x8000u) >> 16;
        const uint32_t g = (aG[x] * w0 + bG[x] * w1 + 0x8000u) >> 16;
        const uint32_t bl = (aB[x] * w0 + bB[x] * w1 + 0x8000u) >> 16;
        dst[x] = 0xFF000000u | (bl << 16) | (g << 8) | r;
    }
}

#if defined(__x86_64__) || defined(_M_X64)
#define BPS_RASTER_X86 1
// AVX2 versions of the two resample passes, picked at run time (a CPU without AVX2 keeps the scalar
// loops above/below). Same arithmetic as the scalar code, eight pixels per step.
// 8 x int32 (each <= 65280) -> 8 x uint16
__attribute__((target("avx2")))
inline void Store16Avx2(uint16_t* dst, __m256i v) {
    const __m256i packed = _mm256_permute4x64_epi64(_mm256_packus_epi32(v, v), 0xD8);
    _mm_storeu_si128(reinterpret_cast<__m128i*>(dst), _mm256_castsi256_si128(packed));
}

__attribute__((target("avx2")))
inline __m256i LaneBlendAvx2(const uint16_t* pa, const uint16_t* pb, size_t x, __m256i vw0, __m256i vw1,
                             __m256i half) {
    const __m256i va = _mm256_cvtepu16_epi32(_mm_loadu_si128(reinterpret_cast<const __m128i*>(pa + x)));
    const __m256i vb = _mm256_cvtepu16_epi32(_mm_loadu_si128(reinterpret_cast<const __m128i*>(pb + x)));
    return _mm256_srli_epi32(_mm256_add_epi32(_mm256_add_epi32(_mm256_mullo_epi32(va, vw0),
                                                               _mm256_mullo_epi32(vb, vw1)), half), 16);
}

__attribute__((target("avx2")))
inline void HorizontalRowAvx2(const uint32_t* s, const int32_t* i0, const int32_t* i1, const int32_t* w0,
                              size_t w, uint16_t* R, uint16_t* G, uint16_t* B) {
    const __m256i mask = _mm256_set1_epi32(0xFF);
    const __m256i k256 = _mm256_set1_epi32(256);
    for (size_t x = 0; x < w; x += 8) {   // the tap arrays are padded to a multiple of 8
        const __m256i a = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(i0 + x));
        const __m256i b = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(i1 + x));
        const __m256i wa = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(w0 + x));
        const __m256i wb = _mm256_sub_epi32(k256, wa);
        const __m256i p0 = _mm256_i32gather_epi32(reinterpret_cast<const int*>(s), a, 4);
        const __m256i p1 = _mm256_i32gather_epi32(reinterpret_cast<const int*>(s), b, 4);
        const __m256i r = _mm256_add_epi32(_mm256_mullo_epi32(_mm256_and_si256(p0, mask), wa),
                                           _mm256_mullo_epi32(_mm256_and_si256(p1, mask), wb));
        const __m256i g = _mm256_add_epi32(
            _mm256_mullo_epi32(_mm256_and_si256(_mm256_srli_epi32(p0, 8), mask), wa),
            _mm256_mullo_epi32(_mm256_and_si256(_mm256_srli_epi32(p1, 8), mask), wb));
        const __m256i bl = _mm256_add_epi32(
            _mm256_mullo_epi32(_mm256_and_si256(_mm256_srli_epi32(p0, 16), mask), wa),
            _mm256_mullo_epi32(_mm256_and_si256(_mm256_srli_epi32(p1, 16), mask), wb));
        Store16Avx2(R + x, r);
        Store16Avx2(G + x, g);
        Store16Avx2(B + x, bl);
    }
}

__attribute__((target("avx2")))
inline void VerticalBlendRowAvx2(uint32_t* dst, const uint16_t* a, const uint16_t* b, size_t w, size_t stride,
                                 uint32_t w0, uint32_t w1) {
    const __m256i vw0 = _mm256_set1_epi32(static_cast<int>(w0));
    const __m256i vw1 = _mm256_set1_epi32(static_cast<int>(w1));
    const __m256i half = _mm256_set1_epi32(0x8000);
    const uint16_t* aR = a; const uint16_t* aG = a + stride; const uint16_t* aB = a + 2 * stride;
    const uint16_t* bR = b; const uint16_t* bG = b + stride; const uint16_t* bB = b + 2 * stride;
    size_t x = 0;
    for (; x + 8 <= w; x += 8) {
        const __m256i r = LaneBlendAvx2(aR, bR, x, vw0, vw1, half);
        const __m256i g = LaneBlendAvx2(aG, bG, x, vw0, vw1, half);
        const __m256i bl = LaneBlendAvx2(aB, bB, x, vw0, vw1, half);
        const __m256i px = _mm256_or_si256(
            _mm256_or_si256(_mm256_set1_epi32(static_cast<int>(0xFF000000u)), _mm256_slli_epi32(bl, 16)),
            _mm256_or_si256(_mm256_slli_epi32(g, 8), r));
        _mm256_storeu_si256(reinterpret_cast<__m256i*>(dst + x), px);
    }
    for (; x < w; ++x) {
        const uint32_t r = (aR[x] * w0 + bR[x] * w1 + 0x8000u) >> 16;
        const uint32_t g = (aG[x] * w0 + bG[x] * w1 + 0x8000u) >> 16;
        const uint32_t bl = (aB[x] * w0 + bB[x] * w1 + 0x8000u) >> 16;
        dst[x] = 0xFF000000u | (bl << 16) | (g << 8) | r;
    }
}
inline bool HaveAvx2() {
    static const bool yes = __builtin_cpu_supports("avx2");
    return yes;
}
#endif
} // namespace

namespace {
// ---- CSS blending (W3C "Compositing and Blending Level 1") -----------------
inline float BMultiply(float cb, float cs) { return cb * cs; }
inline float BScreen(float cb, float cs) { return cb + cs - cb * cs; }
inline float BOverlay(float cb, float cs) { return cb <= 0.5f ? 2.0f * cb * cs : 1.0f - 2.0f * (1.0f - cb) * (1.0f - cs); }
inline float BHardLight(float cb, float cs) {
    return cs <= 0.5f ? BMultiply(cb, 2.0f * cs) : BScreen(cb, 2.0f * cs - 1.0f);
}
inline float BSoftLight(float cb, float cs) {
    const float d = cb <= 0.25f ? ((16.0f * cb - 12.0f) * cb + 4.0f) * cb : std::sqrt(cb);
    return cs <= 0.5f ? cb - (1.0f - 2.0f * cs) * cb * (1.0f - cb)
                      : cb + (2.0f * cs - 1.0f) * (d - cb);
}
inline float BDodge(float cb, float cs) {
    if (cb <= 0.0f) return 0.0f;
    if (cs >= 1.0f) return 1.0f;
    return std::min(1.0f, cb / (1.0f - cs));
}
inline float BBurn(float cb, float cs) {
    if (cb >= 1.0f) return 1.0f;
    if (cs <= 0.0f) return 0.0f;
    return 1.0f - std::min(1.0f, (1.0f - cb) / cs);
}

struct Rgb {
    float r, g, b;
};

inline float Lum(const Rgb& c) { return 0.3f * c.r + 0.59f * c.g + 0.11f * c.b; }

inline Rgb ClipColor(const Rgb& c) {
    const float l = Lum(c);
    float n = std::min(c.r, std::min(c.g, c.b));
    float x = std::max(c.r, std::max(c.g, c.b));
    Rgb out = c;
    if (n < 0.0f) {
        const float k = l - n;
        if (k > 1e-6f) {
            out.r = l + (out.r - l) * l / k;
            out.g = l + (out.g - l) * l / k;
            out.b = l + (out.b - l) * l / k;
        } else {
            out = {l, l, l};
        }
    }
    if (x > 1.0f) {
        const float k = x - l;
        if (k > 1e-6f) {
            out.r = l + (out.r - l) * (1.0f - l) / k;
            out.g = l + (out.g - l) * (1.0f - l) / k;
            out.b = l + (out.b - l) * (1.0f - l) / k;
        }
    }
    return out;
}

inline Rgb SetLum(const Rgb& c, float l) {
    const float d = l - Lum(c);
    return ClipColor({c.r + d, c.g + d, c.b + d});
}

inline float Sat(const Rgb& c) { return std::max(c.r, std::max(c.g, c.b)) - std::min(c.r, std::min(c.g, c.b)); }

inline Rgb SetSat(const Rgb& c, float s) {
    const float v[3] = {c.r, c.g, c.b};
    int rank[3] = {0, 1, 2};
    const auto swapR = [&rank](int a, int b) { const int t = rank[a]; rank[a] = rank[b]; rank[b] = t; };
    if (v[rank[0]] > v[rank[1]]) swapR(0, 1);
    if (v[rank[1]] > v[rank[2]]) swapR(1, 2);
    if (v[rank[0]] > v[rank[1]]) swapR(0, 1);
    float out[3] = {0.0f, 0.0f, 0.0f};
    if (v[rank[2]] - v[rank[0]] > 1e-6f) {
        out[rank[1]] = (v[rank[1]] - v[rank[0]]) * s / (v[rank[2]] - v[rank[0]]);
        out[rank[2]] = s;
    }
    out[rank[0]] = 0.0f;
    return {out[0], out[1], out[2]};
}

inline float SeparableBlend(BlendMode mode, float cb, float cs) {
    switch (mode) {
        case BlendMode::Multiply: return BMultiply(cb, cs);
        case BlendMode::Screen: return BScreen(cb, cs);
        case BlendMode::Overlay: return BOverlay(cb, cs);
        case BlendMode::Darken: return std::min(cb, cs);
        case BlendMode::Lighten: return std::max(cb, cs);
        case BlendMode::ColorDodge: return BDodge(cb, cs);
        case BlendMode::ColorBurn: return BBurn(cb, cs);
        case BlendMode::HardLight: return BHardLight(cb, cs);
        case BlendMode::SoftLight: return BSoftLight(cb, cs);
        case BlendMode::Difference: return std::fabs(cb - cs);
        case BlendMode::Exclusion: return cb + cs - 2.0f * cb * cs;
        default: return cs;
    }
}
} // namespace

uint32_t SoftwareGraphicsBackend::BlendPixel(uint32_t dst, uint32_t src, BlendMode mode) {
    // ---- THE COMMON CASES, IN INTEGERS -------------------------------------------------------
    // Alpha-over is what draws almost every pixel of a frame, and the general path below is float
    // math with several divisions per pixel — ~50 ns each, 2 million times per full-screen layer.
    // An opaque source is just the source; a clear one leaves the destination; a translucent source
    // over an OPAQUE destination (a backdrop, a video) needs no division by the output alpha.
    // (Truncating division, like the float path's To8; an exact result stays exact.)
    if (mode == BlendMode::Alpha) {
        const uint32_t sa8 = src >> 24;
        if (sa8 == 255u) return src;
        if (sa8 == 0u) return dst;
        if ((dst >> 24) == 255u) {
            const uint32_t ia = 255u - sa8;
            const uint32_t r = ((src & 0xFFu) * sa8 + (dst & 0xFFu) * ia) / 255u;
            const uint32_t g = (((src >> 8) & 0xFFu) * sa8 + ((dst >> 8) & 0xFFu) * ia) / 255u;
            const uint32_t b = (((src >> 16) & 0xFFu) * sa8 + ((dst >> 16) & 0xFFu) * ia) / 255u;
            return 0xFF000000u | (b << 16) | (g << 8) | r;
        }
    }
    const float sa = static_cast<float>((src >> 24) & 0xFF) / 255.0f;
    const float da = static_cast<float>((dst >> 24) & 0xFF) / 255.0f;
    const float sr = static_cast<float>((src >> 0) & 0xFF) / 255.0f;
    const float sg = static_cast<float>((src >> 8) & 0xFF) / 255.0f;
    const float sb = static_cast<float>((src >> 16) & 0xFF) / 255.0f;
    const float dr = static_cast<float>((dst >> 0) & 0xFF) / 255.0f;
    const float dg = static_cast<float>((dst >> 8) & 0xFF) / 255.0f;
    const float db = static_cast<float>((dst >> 16) & 0xFF) / 255.0f;

    float or_, og, ob, oa;
    if (mode == BlendMode::Additive) {
        or_ = dr + sr * sa;
        og = dg + sg * sa;
        ob = db + sb * sa;
        oa = std::max(da, sa);
    } else if (mode == BlendMode::Alpha) {
        const float outA = sa + da * (1.0f - sa);
        if (outA <= 0.0f) return 0;
        or_ = (sr * sa + dr * da * (1.0f - sa)) / outA;
        og = (sg * sa + dg * da * (1.0f - sa)) / outA;
        ob = (sb * sa + db * da * (1.0f - sa)) / outA;
        oa = outA;
    } else {
        const float ao = sa + da * (1.0f - sa);
        if (ao <= 0.0f) return 0;
        const Rgb cb{dr, dg, db};
        const Rgb cs{sr, sg, sb};
        Rgb blended;
        switch (mode) {
            case BlendMode::Hue:
                blended = SetLum(SetSat(cs, Sat(cb)), Lum(cb));
                break;
            case BlendMode::Saturation:
                blended = SetLum(SetSat(cb, Sat(cs)), Lum(cb));
                break;
            case BlendMode::Color:
                blended = SetLum(cs, Lum(cb));
                break;
            case BlendMode::Luminosity:
                blended = SetLum(cb, Lum(cs));
                break;
            default:
                blended = {SeparableBlend(mode, dr, sr), SeparableBlend(mode, dg, sg),
                           SeparableBlend(mode, db, sb)};
                break;
        }
        const auto mixChannel = [sa, da, ao](float srcC, float bl, float dstC) {
            return (sa * (1.0f - da) * srcC + sa * da * bl + (1.0f - sa) * da * dstC) / ao;
        };
        or_ = mixChannel(sr, blended.r, dr);
        og = mixChannel(sg, blended.g, dg);
        ob = mixChannel(sb, blended.b, db);
        oa = ao;
    }
    return Pack8(To8(or_), To8(og), To8(ob), To8(oa));
}

void SoftwareGraphicsBackend::ApplyEffect(RgbaImage& img, const Rect& region, int effect,
                                          float param) {
    if (img.empty()) return;
    const int x0 = std::max(0, static_cast<int>(region.x));
    const int y0 = std::max(0, static_cast<int>(region.y));
    const int x1 = std::min(img.width, static_cast<int>(region.Right()));
    const int y1 = std::min(img.height, static_cast<int>(region.Bottom()));
    if (x1 <= x0 || y1 <= y0) return;

    switch (effect) {
        // EffectType::Brightness / Contrast / Saturation / Opacity
        case 3: {   // Opacity — scale alpha in region
            const float k = Clamp01(param);
            for (int y = y0; y < y1; ++y)
                for (int x = x0; x < x1; ++x) {
                    uint32_t& px = img.pixels[static_cast<size_t>(y) * img.width + x];
                    uint32_t a = static_cast<uint32_t>(
                        static_cast<float>((px >> 24) & 0xFF) * k);
                    px = (px & 0x00FFFFFFu) | (a << 24);
                }
            break;
        }
        case 4: {   // Brightness [0..2]
            const float k = param;
            for (int y = y0; y < y1; ++y)
                for (int x = x0; x < x1; ++x) {
                    uint32_t& px = img.pixels[static_cast<size_t>(y) * img.width + x];
                    const auto r = static_cast<uint32_t>(
                        std::min(255.0f, static_cast<float>((px >> 0) & 0xFF) * k));
                    const auto g = static_cast<uint32_t>(
                        std::min(255.0f, static_cast<float>((px >> 8) & 0xFF) * k));
                    const auto b = static_cast<uint32_t>(
                        std::min(255.0f, static_cast<float>((px >> 16) & 0xFF) * k));
                    px = Pack8(r, g, b, (px >> 24) & 0xFF);
                }
            break;
        }
        case 5: {   // Contrast [0..2]; 1 = identity
            const float c = std::max(0.0f, param);
            for (int y = y0; y < y1; ++y)
                for (int x = x0; x < x1; ++x) {
                    uint32_t& px = img.pixels[static_cast<size_t>(y) * img.width + x];
                    auto adj = [c](uint32_t v) {
                        const float f = static_cast<float>(v) / 255.0f;
                        return To8((f - 0.5f) * c + 0.5f);
                    };
                    px = Pack8(adj((px >> 0) & 0xFF), adj((px >> 8) & 0xFF),
                               adj((px >> 16) & 0xFF), (px >> 24) & 0xFF);
                }
            break;
        }
        case 6: {   // Saturation [0..2]; 1 = identity
            const float s = std::max(0.0f, param);
            for (int y = y0; y < y1; ++y)
                for (int x = x0; x < x1; ++x) {
                    uint32_t& px = img.pixels[static_cast<size_t>(y) * img.width + x];
                    const float r = static_cast<float>((px >> 0) & 0xFF) / 255.0f;
                    const float g = static_cast<float>((px >> 8) & 0xFF) / 255.0f;
                    const float b = static_cast<float>((px >> 16) & 0xFF) / 255.0f;
                    const float l = 0.2126f * r + 0.7152f * g + 0.0722f * b;
                    px = Pack8(To8((l + (r - l) * s)), To8((l + (g - l) * s)),
                               To8((l + (b - l) * s)), (px >> 24) & 0xFF);
                }
            break;
        }
        case 0: {   // Blur (box blur, param = radius)
            const int radius = std::max(1, static_cast<int>(param));
            RgbaImage tmp = img;
            for (int y = y0; y < y1; ++y)
                for (int x = x0; x < x1; ++x) {
                    long sumR = 0, sumG = 0, sumB = 0, sumA = 0, n = 0;
                    for (int dy = -radius; dy <= radius; ++dy)
                        for (int dx = -radius; dx <= radius; ++dx) {
                            const int sx = x + dx, sy = y + dy;
                            if (sx < 0 || sx >= img.width || sy < 0 || sy >= img.height)
                                continue;
                            const uint32_t sp =
                                img.pixels[static_cast<size_t>(sy) * img.width + sx];
                            sumR += (sp >> 0) & 0xFF;
                            sumG += (sp >> 8) & 0xFF;
                            sumB += (sp >> 16) & 0xFF;
                            sumA += (sp >> 24) & 0xFF;
                            ++n;
                        }
                    if (n == 0) continue;
                    tmp.pixels[static_cast<size_t>(y) * img.width + x] =
                        Pack8(static_cast<uint32_t>(sumR / n),
                              static_cast<uint32_t>(sumG / n),
                              static_cast<uint32_t>(sumB / n),
                              static_cast<uint32_t>(sumA / n));
                }
            img.pixels.swap(tmp.pixels);
            break;
        }
        default:
            break;   // Glow/Shadow/Crop/Mask handled as composite ops upstream
    }
}

// ===========================================================================
// NullGraphicsBackend
// ===========================================================================

Result<TextureId> NullGraphicsBackend::CreateTexture(const RgbaImage& image) {
    std::lock_guard<std::mutex> lock(mutex_);
    TextureId id{nextTexture_++};
    textures_[id.id] = image;
    textureBytes_ += image.ByteSize();
    return id;
}

Result<void> NullGraphicsBackend::UpdateTexture(TextureId id, const RgbaImage& image) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = textures_.find(id.id);
    if (it == textures_.end()) return Error::Make(Err::Render_TextureNotFound, "Render", "no texture");
    textureBytes_ -= it->second.ByteSize();
    it->second = image;
    textureBytes_ += image.ByteSize();
    return Ok();
}

Result<void> NullGraphicsBackend::DestroyTexture(TextureId id) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = textures_.find(id.id);
    if (it == textures_.end()) return Error::Make(Err::Render_TextureNotFound, "Render", "no texture");
    textureBytes_ -= it->second.ByteSize();
    textures_.erase(it);
    return Ok();
}

RgbaImage NullGraphicsBackend::ReadTexture(TextureId id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = textures_.find(id.id);
    return it == textures_.end() ? RgbaImage{} : it->second;
}

Result<uint32_t> NullGraphicsBackend::CreateShader(std::string_view name,
                                                   std::string_view src) {
    std::lock_guard<std::mutex> lock(mutex_);
    const uint32_t id = nextShader_++;
    shaders_[id] = std::string(src);
    (void)name;
    return id;
}

Result<void> NullGraphicsBackend::DestroyShader(uint32_t id) {
    std::lock_guard<std::mutex> lock(mutex_);
    shaders_.erase(id);
    return Ok();
}

Result<void> NullGraphicsBackend::BeginFrame(int width, int height) {
    std::lock_guard<std::mutex> lock(mutex_);
    frameWidth_ = width;
    frameHeight_ = height;
    return Ok();
}

Result<void> NullGraphicsBackend::Submit(const std::vector<DrawCommand>& commands) {
    std::lock_guard<std::mutex> lock(mutex_);
    submitCount_ += commands.size();
    return Ok();
}

Result<RgbaImage> NullGraphicsBackend::Readback() const {
    std::lock_guard<std::mutex> lock(mutex_);
    RgbaImage img;
    img.width = frameWidth_;
    img.height = frameHeight_;
    img.pixels.assign(static_cast<size_t>(frameWidth_) * frameHeight_, 0u);
    return img;
}

Result<void> NullGraphicsBackend::Shutdown() {
    std::lock_guard<std::mutex> lock(mutex_);
    textures_.clear();
    shaders_.clear();
    textureBytes_ = 0;
    return Ok();
}

// ===========================================================================
// SoftwareGraphicsBackend
// ===========================================================================

// The pixels' hash, computed once per picture: the last few pictures hashed are remembered by pointer
// (and held, so a pointer can never be reused for different pixels while it is remembered).
uint64_t SoftwareGraphicsBackend::HashOf(const SharedRgbaImage& image, bool* opaqueOut) {
    for (const HashMemoEntry& e : hashMemo_)
        if (e.image == image) {
            if (opaqueOut) *opaqueOut = e.opaque;
            return e.hash;
        }
    const uint64_t hash = HashPixels(image->pixels, image->width, image->height);
    uint32_t acc = ~0u;   // alpha stays 255 only if every pixel's alpha is 255 (vectorises; once per picture)
    for (const uint32_t px : image->pixels) acc &= px;
    const bool opaque = !image->pixels.empty() && (acc >> 24) == 0xFFu;
    constexpr size_t kMemo = 6;
    if (hashMemo_.size() >= kMemo) hashMemo_.erase(hashMemo_.begin());
    hashMemo_.push_back(HashMemoEntry{image, hash, opaque});
    if (opaqueOut) *opaqueOut = opaque;
    return hash;
}

Result<TextureId> SoftwareGraphicsBackend::CreateTextureShared(const SharedRgbaImage& image) {
    if (!image) return Error::Make(Err::Render_TextureNotFound, "Render", "no image");
    std::lock_guard<std::mutex> lock(mutex_);
    TextureId id{nextTexture_++};
    textures_[id.id] = image;
    textureBytes_ += image->ByteSize();
    textureVersion_[id.id] = 1;
    bool opaque = false;
    textureHash_[id.id] = HashOf(image, &opaque);
    textureOpaque_[id.id] = opaque;
    return id;
}

Result<TextureId> SoftwareGraphicsBackend::CreateTexture(const RgbaImage& image) {
    return CreateTextureShared(std::make_shared<const RgbaImage>(image));
}

Result<void> SoftwareGraphicsBackend::UpdateTextureShared(TextureId id, const SharedRgbaImage& image) {
    if (!image) return Error::Make(Err::Render_TextureNotFound, "Render", "no image");
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = textures_.find(id.id);
    if (it == textures_.end()) return Error::Make(Err::Render_TextureNotFound, "Render", "no texture");
    textureBytes_ -= it->second->ByteSize();
    it->second = image;
    textureBytes_ += image->ByteSize();
    ++textureVersion_[id.id];   // the picture changed: every cached raster that drew it is stale
    bool opaque = false;
    textureHash_[id.id] = HashOf(image, &opaque);
    textureOpaque_[id.id] = opaque;
    return Ok();
}

Result<void> SoftwareGraphicsBackend::UpdateTexture(TextureId id, const RgbaImage& image) {
    return UpdateTextureShared(id, std::make_shared<const RgbaImage>(image));
}

Result<void> SoftwareGraphicsBackend::DestroyTexture(TextureId id) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = textures_.find(id.id);
    if (it == textures_.end()) return Error::Make(Err::Render_TextureNotFound, "Render", "no texture");
    textureBytes_ -= it->second->ByteSize();
    textures_.erase(it);
    textureVersion_.erase(id.id);
    textureSeen_.erase(id.id);
    textureHash_.erase(id.id);
    textureOpaque_.erase(id.id);
    return Ok();
}

RgbaImage SoftwareGraphicsBackend::ReadTexture(TextureId id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = textures_.find(id.id);
    return it == textures_.end() ? RgbaImage{} : *it->second;
}

Result<uint32_t> SoftwareGraphicsBackend::CreateShader(std::string_view name,
                                                       std::string_view src) {
    std::lock_guard<std::mutex> lock(mutex_);
    const uint32_t id = nextShader_++;
    shaders_[id] = std::string(src);
    (void)name;
    return id;
}

Result<void> SoftwareGraphicsBackend::DestroyShader(uint32_t id) {
    std::lock_guard<std::mutex> lock(mutex_);
    shaders_.erase(id);
    return Ok();
}

Result<void> SoftwareGraphicsBackend::BeginFrame(int width, int height) {
    std::lock_guard<std::mutex> lock(mutex_);
    fbWidth_ = width;
    fbHeight_ = height;
    fb_.width = width;
    fb_.height = height;
    const size_t n = static_cast<size_t>(std::max(0, width)) * static_cast<size_t>(std::max(0, height));
    fbNeedsClear_ = false;
    if (fb_.pixels.size() != n) {
        std::vector<uint32_t> spare = FramebufferPool::Instance().Take(n);
        if (spare.size() == n) {
            fb_.pixels = std::move(spare);   // recycled: not cleared yet — Submit clears what it does not overwrite
            fbNeedsClear_ = true;
        } else {
            fb_.pixels.assign(n, 0u);
        }
    } else {
        std::fill(fb_.pixels.begin(), fb_.pixels.end(), 0u);
    }
    pending_ = PendingEffect{};
    return Ok();
}

// The number of LEADING commands whose pixels cannot have changed since the last Submit: none of them
// touches a texture whose version moved, and the run ends where no effect region is left open (an
// effect reads the pixels under it, so the cut must not fall between its Push and Pop).
size_t SoftwareGraphicsBackend::StaticPrefixLength(const std::vector<DrawCommand>& commands) const {
    size_t balanced = 0;
    int depth = 0;
    for (size_t i = 0; i < commands.size(); ++i) {
        const DrawCommand& c = commands[i];
        if (c.type == DrawCommand::Type::BlitImage || c.type == DrawCommand::Type::DrawGlyph) {
            const auto v = textureVersion_.find(c.texture.id);
            const auto s = textureSeen_.find(c.texture.id);
            const uint64_t version = v != textureVersion_.end() ? v->second : 0u;
            const uint64_t seen = s != textureSeen_.end() ? s->second : 0u;
            if (version != seen)
                break;   // a picture that moved: it and everything after it is drawn fresh
        }
        if (c.type == DrawCommand::Type::PushEffect) ++depth;
        if (c.type == DrawCommand::Type::PopEffect) --depth;
        if (depth == 0) balanced = i + 1;
    }
    return balanced;
}

uint64_t SoftwareGraphicsBackend::HashPrefix(const std::vector<DrawCommand>& commands, size_t count) const {
    return HashCommands(commands, 0, count);
}

uint64_t SoftwareGraphicsBackend::HashCommands(const std::vector<DrawCommand>& commands, size_t from,
                                               size_t to) const {
    uint64_t h = HashMix(0xCBF29CE484222325ull, static_cast<uint64_t>(fbWidth_));
    h = HashMix(h, static_cast<uint64_t>(fbHeight_));
    for (size_t i = from; i < to; ++i) {
        const DrawCommand& c = commands[i];
        h = HashMix(h, static_cast<uint64_t>(c.type));
        h = HashFloat(h, c.rect.x); h = HashFloat(h, c.rect.y);
        h = HashFloat(h, c.rect.width); h = HashFloat(h, c.rect.height);
        h = HashFloat(h, c.color.r); h = HashFloat(h, c.color.g);
        h = HashFloat(h, c.color.b); h = HashFloat(h, c.color.a);
        h = HashFloat(h, c.srcRect.x); h = HashFloat(h, c.srcRect.y);
        h = HashFloat(h, c.srcRect.width); h = HashFloat(h, c.srcRect.height);
        h = HashFloat(h, c.cornerRadius);
        h = HashFloat(h, c.effectParam);
        h = HashMix(h, static_cast<uint64_t>(static_cast<uint32_t>(c.effect)));
        h = HashMix(h, static_cast<uint64_t>(static_cast<uint32_t>(c.blend)));
        h = HashMix(h, c.shaderId);
        if (c.type == DrawCommand::Type::BlitImage || c.type == DrawCommand::Type::DrawGlyph) {
            // The texture by its CONTENT, not its id: the same picture under another id (a scene
            // rebuilt, a texture re-created) is the same pixels.
            const auto th = textureHash_.find(c.texture.id);
            h = HashMix(h, th != textureHash_.end() ? th->second : c.texture.id);
        }
    }
    return h;
}

Result<void> SoftwareGraphicsBackend::Submit(const std::vector<DrawCommand>& commands) {
    std::lock_guard<std::mutex> lock(mutex_);
    static timing::StageTiming stageTiming("software raster");
    // ---- the unchanged leading part of the frame comes from the snapshot, not the rasteriser ----
    size_t start = 0;
    // ---- everything a full-frame OPAQUE picture overdraws is never drawn -------------------------------
    // A camera/video that fills the frame replaces every pixel beneath it (the clear, the scaled
    // background, the plates under it): restoring a snapshot of that and then copying the video over it
    // wrote the whole frame twice. The last such picture (alpha-over, full opacity, opaque pixels, covering
    // the frame, outside any open effect) is where drawing starts.
    size_t overdrawnUpTo = 0;
    static const bool overdrawSkipOff = std::getenv("BPS_NO_OVERDRAW_SKIP") != nullptr;   // A/B hook for measurements
    if (fbWidth_ > 0 && fbHeight_ > 0 && !overdrawSkipOff) {
        int depthAt = 0;
        std::vector<int> depthBefore(commands.size());
        for (size_t i = 0; i < commands.size(); ++i) {
            depthBefore[i] = depthAt;
            if (commands[i].type == DrawCommand::Type::PushEffect) ++depthAt;
            if (commands[i].type == DrawCommand::Type::PopEffect) --depthAt;
        }
        for (size_t i = commands.size(); i-- > 0;) {
            const DrawCommand& c = commands[i];
            if (c.type != DrawCommand::Type::BlitImage || depthBefore[i] != 0) continue;
            if (static_cast<BlendMode>(c.blend) != BlendMode::Alpha || c.color.a < 0.999f) continue;
            const auto op = textureOpaque_.find(c.texture.id);
            if (op == textureOpaque_.end() || !op->second || textures_.find(c.texture.id) == textures_.end()) continue;
            const int bx = static_cast<int>(c.rect.x), by = static_cast<int>(c.rect.y);
            const int bw = static_cast<int>(c.rect.width), bh = static_cast<int>(c.rect.height);
            if (bx <= 0 && by <= 0 && bx + bw >= fbWidth_ && by + bh >= fbHeight_) {
                overdrawnUpTo = i;
                break;
            }
        }
    }
    const size_t staticCount = overdrawnUpTo > 0 ? 0 : StaticPrefixLength(commands);
    if (overdrawnUpTo > 0) {
        start = overdrawnUpTo;
        fbNeedsClear_ = false;   // the picture that covers the frame overwrites every pixel of the old one
    } else if (staticCount == 0) {
        ClearIfStale();
    }
    if (staticCount > 0 && fbWidth_ > 0 && fbHeight_ > 0) {
        const auto tPrefix = std::chrono::steady_clock::now();
        const uint64_t hash = HashPrefix(commands, staticCount);
        PrefixEntry* hit = nullptr;
        for (PrefixEntry& e : prefixCache_)
            if (e.hash == hash && e.count == staticCount && e.width == fbWidth_ && e.height == fbHeight_) {
                hit = &e;
                break;
            }
        if (hit) {
            std::memcpy(fb_.pixels.data(), hit->pixels.data(), hit->pixels.size() * sizeof(uint32_t));
            fbNeedsClear_ = false;   // the snapshot is the whole frame
            hit->used = ++prefixTick_;
            start = staticCount;
            stageTiming.Add("prefix restored", timing::MsSince(tPrefix));
        } else {
            ClearIfStale();
            for (size_t i = 0; i < staticCount; ++i) Execute(commands[i]);
            PrefixEntry* slot = nullptr;
            constexpr size_t kMaxPrefixEntries = 8;   // a few scenes' worth (program + each output)
            if (prefixCache_.size() < kMaxPrefixEntries) {
                prefixCache_.emplace_back();
                slot = &prefixCache_.back();
            } else {
                slot = &prefixCache_.front();
                for (PrefixEntry& e : prefixCache_)
                    if (e.used < slot->used) slot = &e;
            }
            slot->hash = hash;
            slot->count = staticCount;
            slot->width = fbWidth_;
            slot->height = fbHeight_;
            slot->used = ++prefixTick_;
            slot->pixels = fb_.pixels;
            start = staticCount;
            stageTiming.Add("prefix drawn", timing::MsSince(tPrefix));
        }
    }
    // ---- the run AFTER the last moving picture: blended from a cached transparent layer -----------------
    size_t drawEnd = commands.size();
    const OverlayEntry* overlay = nullptr;
    if (fbWidth_ > 0 && fbHeight_ > 0) {
        size_t lastMoving = commands.size();   // == size(): no picture moved this frame
        for (size_t i = start; i < commands.size(); ++i) {
            const DrawCommand& c = commands[i];
            if (c.type != DrawCommand::Type::BlitImage && c.type != DrawCommand::Type::DrawGlyph) continue;
            const auto v = textureVersion_.find(c.texture.id);
            const auto s = textureSeen_.find(c.texture.id);
            const uint64_t version = v != textureVersion_.end() ? v->second : 0u;
            const uint64_t seen = s != textureSeen_.end() ? s->second : 0u;
            if (version != seen) lastMoving = i;
        }
        constexpr size_t kMinOverlayCommands = 3;
        const size_t begin = lastMoving < commands.size() ? lastMoving + 1 : commands.size();
        bool qualifies = begin + kMinOverlayCommands <= commands.size();
        int depth = 0;
        for (size_t i = start; qualifies && i < begin; ++i) {
            if (commands[i].type == DrawCommand::Type::PushEffect) ++depth;
            if (commands[i].type == DrawCommand::Type::PopEffect) --depth;
        }
        qualifies = qualifies && depth == 0;
        for (size_t i = begin; qualifies && i < commands.size(); ++i) {
            const DrawCommand& c = commands[i];
            const bool simple = c.type == DrawCommand::Type::FillRect || c.type == DrawCommand::Type::BlitImage
                                || c.type == DrawCommand::Type::DrawGlyph || c.type == DrawCommand::Type::DrawShape;
            qualifies = simple && static_cast<BlendMode>(c.blend) == BlendMode::Alpha;
        }
        if (qualifies) {
            const uint64_t hash = HashCommands(commands, begin, commands.size());
            for (const OverlayEntry& e : overlayCache_)
                if (e.hash == hash && e.width == fbWidth_ && e.height == fbHeight_) { overlay = &e; break; }
            if (!overlay) {
                if (std::find(overlaySeen_.begin(), overlaySeen_.end(), hash) != overlaySeen_.end()) {
                    // Seen last time too: rasterise it onto a transparent layer once.
                    const auto tBuild = std::chrono::steady_clock::now();
                    std::vector<uint32_t> saved = std::move(fb_.pixels);
                    const PendingEffect savedPending = pending_;
                    pending_ = PendingEffect{};
                    fb_.pixels.assign(static_cast<size_t>(fbWidth_) * static_cast<size_t>(fbHeight_), 0u);
                    for (size_t i = begin; i < commands.size(); ++i) Execute(commands[i]);
                    OverlayEntry fresh;
                    fresh.hash = hash;
                    fresh.width = fbWidth_;
                    fresh.height = fbHeight_;
                    fresh.pixels = std::move(fb_.pixels);
                    fb_.pixels = std::move(saved);
                    pending_ = savedPending;
                    // The runs that hold ink: gaps shorter than a few pixels are bridged, so a line of
                    // text is a handful of runs rather than one per letter stroke.
                    constexpr int kBridge = 24;
                    for (int y = 0; y < fbHeight_; ++y) {
                        const uint32_t* row = fresh.pixels.data() + static_cast<size_t>(y) * fbWidth_;
                        int x = 0;
                        while (x < fbWidth_) {
                            while (x < fbWidth_ && (row[x] >> 24) == 0u) ++x;
                            if (x >= fbWidth_) break;
                            const int runStart = x;
                            int last = x;
                            while (x < fbWidth_ && x - last <= kBridge) {
                                if ((row[x] >> 24) != 0u) last = x;
                                ++x;
                            }
                            fresh.runs.push_back(LayerRun{y, runStart, last + 1});
                        }
                    }
                    constexpr size_t kMaxOverlays = 4;
                    if (overlayCache_.size() >= kMaxOverlays) {
                        size_t oldest = 0;
                        for (size_t i = 1; i < overlayCache_.size(); ++i)
                            if (overlayCache_[i].used < overlayCache_[oldest].used) oldest = i;
                        overlayCache_.erase(overlayCache_.begin() + static_cast<std::ptrdiff_t>(oldest));
                    }
                    overlayCache_.push_back(std::move(fresh));
                    overlay = &overlayCache_.back();
                    stageTiming.Add("overlay built", timing::MsSince(tBuild));
                } else {
                    constexpr size_t kMaxSeen = 16;
                    if (overlaySeen_.size() >= kMaxSeen) overlaySeen_.erase(overlaySeen_.begin());
                    overlaySeen_.push_back(hash);
                }
            }
            if (overlay) {
                drawEnd = begin;
                overlayCache_[static_cast<size_t>(overlay - overlayCache_.data())].used = ++overlayTick_;
            }
        }
    }
    // What this Submit draws is now what the NEXT one compares against.
    for (const DrawCommand& c : commands)
        if (c.type == DrawCommand::Type::BlitImage || c.type == DrawCommand::Type::DrawGlyph) {
            const auto v = textureVersion_.find(c.texture.id);
            if (v != textureVersion_.end()) textureSeen_[c.texture.id] = v->second;
        }
    for (size_t idx = start; idx < drawEnd; ++idx) {
        const auto& cmd = commands[idx];
        const auto t0 = std::chrono::steady_clock::now();
        Execute(cmd);
        const char* name = "other";
        switch (cmd.type) {
            case DrawCommand::Type::Clear: name = "clear"; break;
            case DrawCommand::Type::FillRect: name = "fill"; break;
            case DrawCommand::Type::BlitImage: name = "blit"; break;
            case DrawCommand::Type::DrawGlyph: name = "glyph"; break;
            case DrawCommand::Type::DrawShape: name = "shape"; break;
            case DrawCommand::Type::PushEffect:
            case DrawCommand::Type::PopEffect: name = "effect"; break;
            default: break;
        }
        const double ms = timing::MsSince(t0);
        stageTiming.Add(name, ms);
        if (cmd.type == DrawCommand::Type::BlitImage && ms > 40.0)
            stageTiming.Add("blit>40ms", ms);
    }
    if (overlay) {
        const auto tOverlay = std::chrono::steady_clock::now();
        const size_t W = static_cast<size_t>(fbWidth_);
        for (const LayerRun& r : overlay->runs) {
            const uint32_t* s = overlay->pixels.data() + static_cast<size_t>(r.y) * W + static_cast<size_t>(r.x0);
            uint32_t* d = fb_.pixels.data() + static_cast<size_t>(r.y) * W + static_cast<size_t>(r.x0);
            const int n = r.x1 - r.x0;
            for (int i = 0; i < n; ++i) {
                const uint32_t sp = s[i];
                const uint32_t a = sp >> 24;
                if (a == 255u) d[i] = sp;
                else if (a != 0u) d[i] = BlendPixel(d[i], sp, BlendMode::Alpha);
            }
        }
        stageTiming.Add("overlay blended", timing::MsSince(tOverlay));
    }
    stageTiming.EndUnit();
    return Ok();
}

Result<RgbaImage> SoftwareGraphicsBackend::Readback() const {
    std::lock_guard<std::mutex> lock(mutex_);
    RgbaImage copy = fb_;
    if (fbNeedsClear_) std::fill(copy.pixels.begin(), copy.pixels.end(), 0u);   // BeginFrame with no Submit
    return copy;
}

Result<RgbaImage> SoftwareGraphicsBackend::ReadbackMove() {
    std::lock_guard<std::mutex> lock(mutex_);
    ClearIfStale();
    // The caller owns this frame now; the next BeginFrame allocates a fresh one. No 8 MB copy.
    RgbaImage out = std::move(fb_);
    fb_ = RgbaImage{};
    return out;
}

RgbaImage SoftwareGraphicsBackend::ResampleToImage(const RgbaImage& src, const Rect& srcRect, int w,
                                                   int h, bool& opaqueOut) {
    RgbaImage out;
    opaqueOut = false;
    if (src.empty() || w <= 0 || h <= 0) return out;
    const float sw = std::max(1.0f, srcRect.width);
    const float sh = std::max(1.0f, srcRect.height);
    std::vector<ScaleTaps> xTaps(static_cast<size_t>(w));
    std::vector<ScaleTaps> yTaps(static_cast<size_t>(h));
    const double stepX = static_cast<double>(sw) / w;
    const double stepY = static_cast<double>(sh) / h;
    for (int dx = 0; dx < w; ++dx)
        BuildScaleTaps(srcRect.x + dx * stepX, stepX, src.width, xTaps[static_cast<size_t>(dx)]);
    for (int dy = 0; dy < h; ++dy)
        BuildScaleTaps(srcRect.y + dy * stepY, stepY, src.height, yTaps[static_cast<size_t>(dy)]);
    std::vector<FixedAxisTaps> xf(static_cast<size_t>(w));
    for (int dx = 0; dx < w; ++dx)
        xf[static_cast<size_t>(dx)] = ToFixedTaps(xTaps[static_cast<size_t>(dx)], src.width);

    bool opaque = true;
    for (const uint32_t px : src.pixels)
        if ((px >> 24) != 255u) { opaque = false; break; }

    out.width = w;
    out.height = h;
    out.pixels.resize(static_cast<size_t>(w) * static_cast<size_t>(h));

    // ---- OPAQUE, AT MOST TWO TAPS PER AXIS (a camera/video frame scaled up or down by under 2x) -------
    // The common live-video case, so it gets its own lean loops: 8.8 fixed-point weights (each pair
    // sums to exactly 256, so the horizontal pass is exact in 16 bits), planar 16-bit rows, and a
    // vertical pass that is three multiply-adds per pixel over contiguous arrays (the compiler
    // vectorises it). Within half a level of the general path below.
    bool twoTap = opaque;
    for (size_t i = 0; twoTap && i < xTaps.size(); ++i) twoTap = xTaps[i].count <= 2;
    for (size_t i = 0; twoTap && i < yTaps.size(); ++i) twoTap = yTaps[i].count <= 2;
    if (twoTap) {
        struct Tap2 { int i0 = 0, i1 = 0; uint32_t w0 = 256, w1 = 0; };
        const auto makeTap = [](const ScaleTaps& t, int extent) {
            Tap2 r;
            r.i0 = std::clamp(t.index[0], 0, extent - 1);
            r.i1 = r.i0;
            if (t.count >= 2) {
                r.i1 = std::clamp(t.index[1], 0, extent - 1);
                r.w0 = static_cast<uint32_t>(std::clamp(t.weight[0] * 256.0 + 0.5, 0.0, 256.0));
                r.w1 = 256u - r.w0;
            }
            return r;
        };
        std::vector<Tap2> xs(static_cast<size_t>(w)), ys(static_cast<size_t>(h));
        for (int dx = 0; dx < w; ++dx) xs[static_cast<size_t>(dx)] = makeTap(xTaps[static_cast<size_t>(dx)], src.width);
        for (int dy = 0; dy < h; ++dy) ys[static_cast<size_t>(dy)] = makeTap(yTaps[static_cast<size_t>(dy)], src.height);
        const size_t W = static_cast<size_t>(w);
#ifdef BPS_RASTER_X86
        const bool avx2 = HaveAvx2();
        // Gather inputs for the AVX2 pass: structure-of-arrays, padded to whole vectors of eight.
        std::vector<int32_t> xi0, xi1, xw0;
        if (avx2) {
            const size_t padded = (W + 7u) & ~size_t{7};
            xi0.assign(padded, 0); xi1.assign(padded, 0); xw0.assign(padded, 256);
            for (size_t x = 0; x < padded; ++x) {
                const Tap2& t = xs[std::min(x, W - 1)];
                xi0[x] = t.i0; xi1[x] = t.i1; xw0[x] = static_cast<int32_t>(t.w0);
            }
        }
#endif
        const auto band2 = [&](int bandY0, int bandY1) {
            constexpr int kSlots = 4;
            const size_t Wp = (W + 7u) & ~size_t{7};   // plane stride: whole vectors of eight
            std::vector<uint16_t> store(static_cast<size_t>(kSlots) * 3u * Wp);
            int rowOf[kSlots] = {-1, -1, -1, -1};
            int next = 0;
            const auto hrow = [&](int sy) -> const uint16_t* {
                for (int k = 0; k < kSlots; ++k)
                    if (rowOf[k] == sy) return store.data() + static_cast<size_t>(k) * 3u * Wp;
                const int k = next;
                next = (next + 1) % kSlots;
                rowOf[k] = sy;
                uint16_t* R = store.data() + static_cast<size_t>(k) * 3u * Wp;
                uint16_t* G = R + Wp;
                uint16_t* B = G + Wp;
                const uint32_t* s = src.pixels.data() + static_cast<size_t>(sy) * static_cast<size_t>(src.width);
#ifdef BPS_RASTER_X86
                if (avx2) {
                    HorizontalRowAvx2(s, xi0.data(), xi1.data(), xw0.data(), W, R, G, B);
                    return R;
                }
#endif
                for (size_t x = 0; x < W; ++x) {
                    const Tap2& t = xs[x];
                    const uint32_t p0 = s[t.i0], p1 = s[t.i1];
                    R[x] = static_cast<uint16_t>((p0 & 0xFFu) * t.w0 + (p1 & 0xFFu) * t.w1);
                    G[x] = static_cast<uint16_t>(((p0 >> 8) & 0xFFu) * t.w0 + ((p1 >> 8) & 0xFFu) * t.w1);
                    B[x] = static_cast<uint16_t>(((p0 >> 16) & 0xFFu) * t.w0 + ((p1 >> 16) & 0xFFu) * t.w1);
                }
                return R;
            };
            for (int yy = bandY0; yy < bandY1; ++yy) {
                const Tap2& t = ys[static_cast<size_t>(yy)];
                const uint16_t* a = hrow(t.i0);
                const uint16_t* b = hrow(t.i1);
                uint32_t* dstRow = out.pixels.data() + static_cast<size_t>(yy) * W;
#ifdef BPS_RASTER_X86
                if (avx2) {
                    VerticalBlendRowAvx2(dstRow, a, b, W, Wp, t.w0, t.w1);
                    continue;
                }
#endif
                VerticalBlendRow(dstRow, a, b, W, Wp, t.w0, t.w1);
            }
        };
        ForRowBands(0, h, W * static_cast<size_t>(h), band2);
        opaqueOut = true;
        return out;
    }

    // Per source row, the horizontally filtered sums: R,G,B (opaque) or A + premultiplied R,G,B.
    const size_t stride = opaque ? 3u : 4u;
    struct HRow { int srcRow = -1; std::vector<uint32_t> v; };
    constexpr size_t kRowCache = 8;
    const auto band = [&](int bandY0, int bandY1) {
        std::vector<HRow> cache(kRowCache);
        size_t nextSlot = 0;
        const auto hrowFor = [&](int sy) -> const uint32_t* {
            for (const HRow& c : cache)
                if (c.srcRow == sy) return c.v.data();
            HRow& slot = cache[nextSlot];
            nextSlot = (nextSlot + 1) % kRowCache;
            slot.srcRow = sy;
            slot.v.resize(static_cast<size_t>(w) * stride);
            const uint32_t* srow = src.pixels.data() + static_cast<size_t>(sy) * static_cast<size_t>(src.width);
            uint32_t* o = slot.v.data();
            if (opaque) {
                for (int dx = 0; dx < w; ++dx, o += 3) {
                    const FixedAxisTaps& t = xf[static_cast<size_t>(dx)];
                    uint32_t r = 0, g = 0, b = 0;
                    for (int k = 0; k < t.n; ++k) {
                        const uint32_t px = srow[t.idx[k]];
                        r += (px & 0xFFu) * t.w[k];
                        g += ((px >> 8) & 0xFFu) * t.w[k];
                        b += ((px >> 16) & 0xFFu) * t.w[k];
                    }
                    o[0] = r; o[1] = g; o[2] = b;
                }
            } else {
                for (int dx = 0; dx < w; ++dx, o += 4) {
                    const FixedAxisTaps& t = xf[static_cast<size_t>(dx)];
                    uint32_t a = 0, r = 0, g = 0, b = 0;
                    for (int k = 0; k < t.n; ++k) {
                        const uint32_t px = srow[t.idx[k]];
                        const uint32_t pa = px >> 24;
                        a += pa * t.w[k];
                        r += (px & 0xFFu) * pa * t.w[k];
                        g += ((px >> 8) & 0xFFu) * pa * t.w[k];
                        b += ((px >> 16) & 0xFFu) * pa * t.w[k];
                    }
                    o[0] = a; o[1] = r; o[2] = g; o[3] = b;
                }
            }
            return slot.v.data();
        };
        constexpr uint64_t kHalf = 1ull << 31;
        for (int yy = bandY0; yy < bandY1; ++yy) {
            const FixedAxisTaps yf = ToFixedTaps(yTaps[static_cast<size_t>(yy)], src.height);
            const uint32_t* rows[4] = {nullptr, nullptr, nullptr, nullptr};
            for (int k = 0; k < yf.n; ++k)
                rows[k] = hrowFor(yf.idx[k]);
            uint32_t* dst = out.pixels.data() + static_cast<size_t>(yy) * static_cast<size_t>(w);
            if (opaque) {
                for (int dx = 0; dx < w; ++dx) {
                    uint64_t r = 0, g = 0, b = 0;
                    const size_t o = static_cast<size_t>(dx) * 3u;
                    for (int k = 0; k < yf.n; ++k) {
                        const uint64_t wk = yf.w[k];
                        r += wk * rows[k][o];
                        g += wk * rows[k][o + 1];
                        b += wk * rows[k][o + 2];
                    }
                    const uint32_t rr = static_cast<uint32_t>(std::min<uint64_t>(255u, (r + kHalf) >> 32));
                    const uint32_t gg = static_cast<uint32_t>(std::min<uint64_t>(255u, (g + kHalf) >> 32));
                    const uint32_t bb = static_cast<uint32_t>(std::min<uint64_t>(255u, (b + kHalf) >> 32));
                    dst[dx] = 0xFF000000u | (bb << 16) | (gg << 8) | rr;
                }
            } else {
                for (int dx = 0; dx < w; ++dx) {
                    uint64_t a = 0, r = 0, g = 0, b = 0;
                    const size_t o = static_cast<size_t>(dx) * 4u;
                    for (int k = 0; k < yf.n; ++k) {
                        const uint64_t wk = yf.w[k];
                        a += wk * rows[k][o];
                        r += wk * rows[k][o + 1];
                        g += wk * rows[k][o + 2];
                        b += wk * rows[k][o + 3];
                    }
                    if (a == 0) { dst[dx] = 0u; continue; }
                    const uint32_t aa = static_cast<uint32_t>(std::min<uint64_t>(255u, (a + kHalf) >> 32));
                    const uint32_t rr = static_cast<uint32_t>(std::min<uint64_t>(255u, (r + a / 2) / a));
                    const uint32_t gg = static_cast<uint32_t>(std::min<uint64_t>(255u, (g + a / 2) / a));
                    const uint32_t bb = static_cast<uint32_t>(std::min<uint64_t>(255u, (b + a / 2) / a));
                    dst[dx] = (aa << 24) | (bb << 16) | (gg << 8) | rr;
                }
            }
        }
    };
    ForRowBands(0, h, static_cast<size_t>(w) * static_cast<size_t>(h), band);
    opaqueOut = opaque;
    return out;
}

Result<void> SoftwareGraphicsBackend::Shutdown() {
    std::lock_guard<std::mutex> lock(mutex_);
    textures_.clear();
    hashMemo_.clear();
    shaders_.clear();
    textureBytes_ = 0;
    fb_ = RgbaImage{};
    return Ok();
}

// ---------------------------------------------------------------------------
// Command execution
// ---------------------------------------------------------------------------

void SoftwareGraphicsBackend::Execute(const DrawCommand& cmd) {
    switch (cmd.type) {
        case DrawCommand::Type::Clear: {
            std::fill(fb_.pixels.begin(), fb_.pixels.end(), cmd.color.Pack());
            break;
        }
        case DrawCommand::Type::SetBlend:
            break;   // per-command blend stored in cmd.blend
        case DrawCommand::Type::FillRect: {
            const Rect r = SnapRectToPixelEdges(cmd.rect);
            FillRectBlended(static_cast<int>(r.x), static_cast<int>(r.y),
                            static_cast<int>(r.width),
                            static_cast<int>(r.height), cmd.color,
                            static_cast<BlendMode>(cmd.blend));
            break;
        }
        case DrawCommand::Type::BlitImage: {
            auto it = textures_.find(cmd.texture.id);
            if (it == textures_.end()) {
                static std::mutex m;
                static std::set<uint64_t> seen;
                bool first = false;
                {
                    std::lock_guard<std::mutex> lock(m);
                    first = seen.insert(cmd.texture.id).second;
                }
                if (first)
                    Logger::Instance().Warning(
                        std::format("blit: texture id {} not held by this backend — nothing drawn",
                                    cmd.texture.id),
                        "Graphics");
                break;
            }
            const RgbaImage& src = *it->second;
            const int bx = static_cast<int>(cmd.rect.x), by = static_cast<int>(cmd.rect.y);
            const int bw = static_cast<int>(cmd.rect.width), bh = static_cast<int>(cmd.rect.height);
            const float sw = std::max(1.0f, cmd.srcRect.width);
            const float sh = std::max(1.0f, cmd.srcRect.height);
            const bool unit = bw == static_cast<int>(sw) && bh == static_cast<int>(sh);
            // A SCALED blit goes through the scaled-image cache: the picture is resampled to its
            // destination size once, and every later request for the same picture at the same size
            // (the next frame's static background; the same video frame in the next output's scene)
            // is a plain copy. Bounded: a destination bigger than ~1.5x a 1080p frame is drawn directly.
            constexpr size_t kMaxCachedPixels = 3200000u;
            const size_t destPixels = bw > 0 && bh > 0 ? static_cast<size_t>(bw) * static_cast<size_t>(bh) : 0u;
            if (!unit && destPixels > 0 && destPixels <= kMaxCachedPixels
                && static_cast<BlendMode>(cmd.blend) == BlendMode::Alpha) {
                const auto hashIt = textureHash_.find(cmd.texture.id);
                const uint64_t contentHash = hashIt != textureHash_.end() ? hashIt->second : 0u;
                ScaledEntry* entry = nullptr;
                for (ScaledEntry& e : scaledCache_) {
                    if (e.hash == contentHash && e.dw == bw && e.dh == bh && e.sx == cmd.srcRect.x
                        && e.sy == cmd.srcRect.y && e.sw == cmd.srcRect.width && e.sh == cmd.srcRect.height) {
                        entry = &e;
                        break;
                    }
                }
                if (!entry) {
                    ScaledEntry fresh;
                    fresh.hash = contentHash;
                    fresh.dw = bw;
                    fresh.dh = bh;
                    fresh.sx = cmd.srcRect.x;
                    fresh.sy = cmd.srcRect.y;
                    fresh.sw = cmd.srcRect.width;
                    fresh.sh = cmd.srcRect.height;
                    static timing::StageTiming resampleTiming("software resample");
                    const auto tResample = std::chrono::steady_clock::now();
                    fresh.image = ResampleToImage(src, cmd.srcRect, bw, bh, fresh.opaque);
                    resampleTiming.Add("resample", timing::MsSince(tResample));
                    resampleTiming.EndUnit();
                    constexpr size_t kMaxScaledEntries = 8;
                    if (scaledCache_.size() >= kMaxScaledEntries) {
                        size_t oldest = 0;
                        for (size_t i = 1; i < scaledCache_.size(); ++i)
                            if (scaledCache_[i].used < scaledCache_[oldest].used) oldest = i;
                        scaledCache_.erase(scaledCache_.begin() + static_cast<std::ptrdiff_t>(oldest));
                    }
                    scaledCache_.push_back(std::move(fresh));
                    entry = &scaledCache_.back();
                }
                entry->used = ++scaledTick_;
                if (!entry->image.empty()) {
                    static timing::StageTiming copyTiming("software scaled copy");
                    const auto tCopy = std::chrono::steady_clock::now();
                    Blit(bx, by, bw, bh, entry->image,
                         Rect(0, 0, static_cast<float>(bw), static_cast<float>(bh)), cmd.color,
                         static_cast<BlendMode>(cmd.blend));
                    copyTiming.Add("copy", timing::MsSince(tCopy));
                    copyTiming.EndUnit();
                    break;
                }
            }
            Blit(bx, by, bw, bh, src, cmd.srcRect, cmd.color, static_cast<BlendMode>(cmd.blend));
            break;
        }
        case DrawCommand::Type::DrawGlyph: {
            auto it = textures_.find(cmd.texture.id);
            if (it == textures_.end()) break;
            const RgbaImage& src = *it->second;
            const Rect cell = (cmd.srcRect.width > 0.0f && cmd.srcRect.height > 0.0f)
                                  ? cmd.srcRect
                                  : Rect(0, 0, static_cast<float>(src.width),
                                         static_cast<float>(src.height));
            Blit(static_cast<int>(cmd.rect.x), static_cast<int>(cmd.rect.y),
                 static_cast<int>(cmd.rect.width), static_cast<int>(cmd.rect.height),
                 src, cell, cmd.color, static_cast<BlendMode>(cmd.blend));
            break;
        }
        case DrawCommand::Type::DrawShape:
            RasterizeShape(cmd);
            break;
        case DrawCommand::Type::PushEffect:
            pending_.active = true;
            pending_.region = cmd.rect;
            pending_.effect = cmd.effect;
            pending_.param = cmd.effectParam;
            break;
        case DrawCommand::Type::PopEffect:
            if (pending_.active) {
                ApplyEffect(fb_, pending_.region, pending_.effect, pending_.param);
                pending_ = PendingEffect{};
            }
            break;
    }
}

void SoftwareGraphicsBackend::FillRectBlended(int x, int y, int w, int h, Color c,
                                              BlendMode mode) {
    const int x0 = std::max(0, x);
    const int y0 = std::max(0, y);
    const int x1 = std::min(fbWidth_, x + w);
    const int y1 = std::min(fbHeight_, y + h);
    const uint32_t src = c.Pack();
    if (x1 <= x0 || y1 <= y0) return;
    if (mode == BlendMode::Alpha) {
        const uint32_t sa8 = src >> 24;
        if (sa8 == 0u) return;   // nothing to draw
        if (sa8 == 255u) {       // opaque: a plain span fill, no per-pixel blend
            for (int yy = y0; yy < y1; ++yy) {
                uint32_t* row = fb_.pixels.data() + static_cast<size_t>(yy) * fbWidth_;
                std::fill(row + x0, row + x1, src);
            }
            return;
        }
    }
    for (int yy = y0; yy < y1; ++yy)
        for (int xx = x0; xx < x1; ++xx) {
            uint32_t& dst = fb_.pixels[static_cast<size_t>(yy) * fbWidth_ + xx];
            dst = BlendPixel(dst, src, mode);
        }
}

void SoftwareGraphicsBackend::FillRoundedRectBlended(int x, int y, int w, int h,
                                                     float radius, Color c,
                                                     BlendMode mode) {
    if (w <= 0 || h <= 0) return;
    const float fw = static_cast<float>(w);
    const float fh = static_cast<float>(h);
    const float r = std::min(radius, 0.5f * std::min(fw, fh));
    if (r < 0.5f) {
        FillRectBlended(x, y, w, h, c, mode);
        return;
    }
    const int x1 = std::min(fbWidth_, x + w);
    const int y1 = std::min(fbHeight_, y + h);
    const int x0 = std::max(0, x);
    const int y0 = std::max(0, y);
    if (x1 <= x0 || y1 <= y0) return;
    const float fx = static_cast<float>(x);
    const float fy = static_cast<float>(y);
    const uint32_t src = c.Pack();
    // The pixels whose centre lies between the two corner arcs (px in [fx+r, fx+fw-r]) see no
    // horizontal distance to the arc centre, so their coverage depends on the ROW alone: one value
    // per row instead of a square root per pixel. Only the two corner columns are computed pixel by
    // pixel. (A big rounded box — a lower-third plate — was a full-screen sqrt loop every frame.)
    const int spanX0 = std::clamp(static_cast<int>(std::ceil(fx + r - 0.5f)), x0, x1);
    const int spanX1 = std::clamp(static_cast<int>(std::floor(fx + fw - r - 0.5f)) + 1, spanX0, x1);
    const auto corner = [&](int yy, int xx) {
        const float px = static_cast<float>(xx) + 0.5f;
        const float py = static_cast<float>(yy) + 0.5f;
        // The corner-arc centre this pixel's quadrant rounds around.
        const float cx = fx + std::clamp(px - fx, r, fw - r);
        const float cy = fy + std::clamp(py - fy, r, fh - r);
        const float dx = px - cx;
        const float dy = py - cy;
        const float dist = std::sqrt(dx * dx + dy * dy) - r;
        const float coverage = std::clamp(0.5f - dist, 0.0f, 1.0f);
        if (coverage <= 0.0f) return;
        uint32_t& dst = fb_.pixels[static_cast<size_t>(yy) * fbWidth_ + xx];
        dst = BlendPixel(dst, coverage >= 1.0f ? src : ScaleAlpha(src, coverage), mode);
    };
    for (int yy = y0; yy < y1; ++yy) {
        for (int xx = x0; xx < spanX0; ++xx) corner(yy, xx);
        if (spanX1 > spanX0) {
            const float py = static_cast<float>(yy) + 0.5f;
            const float cy = fy + std::clamp(py - fy, r, fh - r);
            const float dist = std::fabs(py - cy) - r;   // dx == 0 along the span
            const float coverage = std::clamp(0.5f - dist, 0.0f, 1.0f);
            if (coverage > 0.0f) {
                const uint32_t rowSrc = coverage >= 1.0f ? src : ScaleAlpha(src, coverage);
                uint32_t* row = fb_.pixels.data() + static_cast<size_t>(yy) * fbWidth_;
                if (mode == BlendMode::Alpha && (rowSrc >> 24) == 255u) {
                    std::fill(row + spanX0, row + spanX1, rowSrc);
                } else {
                    for (int xx = spanX0; xx < spanX1; ++xx)
                        row[xx] = BlendPixel(row[xx], rowSrc, mode);
                }
            }
        }
        for (int xx = spanX1; xx < x1; ++xx) corner(yy, xx);
    }
}

void SoftwareGraphicsBackend::Blit(int x, int y, int w, int h, const RgbaImage& src,
                                   const Rect& srcRect, Color tint, BlendMode mode) {
    if (src.empty() || w <= 0 || h <= 0) return;
    const float sx0 = srcRect.x, sy0 = srcRect.y;
    const float sw = std::max(1.0f, srcRect.width);
    const float sh = std::max(1.0f, srcRect.height);
    const int x0 = std::max(0, x), y0 = std::max(0, y);
    const int x1 = std::min(fbWidth_, x + w), y1 = std::min(fbHeight_, y + h);
    const bool tinted = tint.r < 1.0f || tint.g < 1.0f || tint.b < 1.0f || tint.a < 1.0f;
    const int tr = static_cast<int>(tint.r * 255.0f + 0.5f);
    const int tg = static_cast<int>(tint.g * 255.0f + 0.5f);
    const int tb = static_cast<int>(tint.b * 255.0f + 0.5f);
    const int ta = static_cast<int>(tint.a * 255.0f + 0.5f);

    // ---- THE FILTER -------------------------------------------------------
    const bool unitScale = w == static_cast<int>(sw) && h == static_cast<int>(sh);
    std::vector<ScaleTaps> xTaps, yTaps;
    if (!unitScale) {
        xTaps.resize(static_cast<size_t>(w));
        yTaps.resize(static_cast<size_t>(h));
        const double stepX = static_cast<double>(sw) / w;
        const double stepY = static_cast<double>(sh) / h;
        for (int dx = 0; dx < w; ++dx)
            BuildScaleTaps(sx0 + dx * stepX, stepX, src.width, xTaps[static_cast<size_t>(dx)]);
        for (int dy = 0; dy < h; ++dy)
            BuildScaleTaps(sy0 + dy * stepY, stepY, src.height, yTaps[static_cast<size_t>(dy)]);
    }

    // ---- SCALED ALPHA-OVER: SEPARABLE, IN FIXED POINT --------------------------------------------
    // The per-pixel path below builds up to 16 double-precision taps for EVERY destination pixel
    // (measured ~1.7 s for a 1080p frame holding a background and a video). The SAME filter — each
    // axis's taps, product weights, averaged in premultiplied space — is separable: filter each
    // source ROW horizontally once, then combine up to four filtered rows per destination row.
    // Integer weights (65536 per axis), no per-pixel division for an opaque image (video,
    // backgrounds), and the filtered source rows are reused by the destination rows that share them.
    if (!unitScale && mode == BlendMode::Alpha && x1 > x0 && y1 > y0) {
        struct FTaps { int n = 0; int idx[4] = {0, 0, 0, 0}; uint32_t w[4] = {0, 0, 0, 0}; };
        const auto toFixed = [](const ScaleTaps& t, int extent) {
            FTaps f;
            f.n = t.count;
            int64_t sum = 0;
            int heaviest = 0;
            for (int i = 0; i < t.count; ++i) {
                f.idx[i] = std::clamp(t.index[i], 0, extent - 1);
                const double wd = t.weight[i] * 65536.0 + 0.5;
                f.w[i] = static_cast<uint32_t>(wd < 0.0 ? 0.0 : wd);
                sum += f.w[i];
                if (f.w[i] > f.w[heaviest]) heaviest = i;
            }
            if (f.n > 0) {   // the weights sum to exactly 65536 (the rounding goes to the heaviest tap)
                const int64_t fixed = static_cast<int64_t>(f.w[heaviest]) + (65536 - sum);
                f.w[heaviest] = static_cast<uint32_t>(fixed < 0 ? 0 : fixed);
            }
            return f;
        };
        const int width = x1 - x0;
        std::vector<FTaps> xf(static_cast<size_t>(width));
        for (int dx = 0; dx < width; ++dx)
            xf[static_cast<size_t>(dx)] = toFixed(xTaps[static_cast<size_t>(x0 - x + dx)], src.width);

        bool opaque = true;
        for (const uint32_t px : src.pixels)
            if ((px >> 24) != 255u) { opaque = false; break; }

        // Horizontally filtered source rows, kept while the destination rows below still need them.
        // opaque: three sums per pixel (R, G, B, each 255*65536 at most);
        // otherwise: four (A, then R, G, B premultiplied by A: up to 65025*65536 each).
        const size_t stride = opaque ? 3u : 4u;
        struct HRow { int srcRow = -1; std::vector<uint32_t> v; };
        constexpr size_t kRowCache = 8;
        // One band of destination rows. Each band keeps its OWN filtered-row cache, so bands share
        // nothing but the read-only source, taps and flags, and write disjoint framebuffer rows.
        const auto renderBand = [&](int bandY0, int bandY1) {
        std::vector<HRow> cache(kRowCache);
        size_t nextSlot = 0;
        const auto hrowFor = [&](int sy) -> const uint32_t* {
            for (const HRow& c : cache)
                if (c.srcRow == sy) return c.v.data();
            HRow& slot = cache[nextSlot];
            nextSlot = (nextSlot + 1) % kRowCache;
            slot.srcRow = sy;
            slot.v.resize(static_cast<size_t>(width) * stride);
            const uint32_t* srow = src.pixels.data() + static_cast<size_t>(sy) * static_cast<size_t>(src.width);
            uint32_t* out = slot.v.data();
            if (opaque) {
                for (int dx = 0; dx < width; ++dx) {
                    const FTaps& t = xf[static_cast<size_t>(dx)];
                    uint32_t r = 0, g = 0, b = 0;
                    for (int k = 0; k < t.n; ++k) {
                        const uint32_t px = srow[t.idx[k]];
                        r += (px & 0xFFu) * t.w[k];
                        g += ((px >> 8) & 0xFFu) * t.w[k];
                        b += ((px >> 16) & 0xFFu) * t.w[k];
                    }
                    out[0] = r; out[1] = g; out[2] = b;
                    out += 3;
                }
            } else {
                for (int dx = 0; dx < width; ++dx) {
                    const FTaps& t = xf[static_cast<size_t>(dx)];
                    uint32_t a = 0, r = 0, g = 0, b = 0;
                    for (int k = 0; k < t.n; ++k) {
                        const uint32_t px = srow[t.idx[k]];
                        const uint32_t pa = px >> 24;
                        a += pa * t.w[k];
                        r += (px & 0xFFu) * pa * t.w[k];
                        g += ((px >> 8) & 0xFFu) * pa * t.w[k];
                        b += ((px >> 16) & 0xFFu) * pa * t.w[k];
                    }
                    out[0] = a; out[1] = r; out[2] = g; out[3] = b;
                    out += 4;
                }
            }
            return slot.v.data();
        };

        const auto tintPixel = [&](uint32_t sp) {
            if (!tinted) return sp;
            const int cr = static_cast<int>((sp >> 0) & 0xFF);
            const int cg = static_cast<int>((sp >> 8) & 0xFF);
            const int cb = static_cast<int>((sp >> 16) & 0xFF);
            const int ca = static_cast<int>((sp >> 24) & 0xFF);
            return (static_cast<uint32_t>(ca * ta / 255) << 24)
                 | (static_cast<uint32_t>(cb * tb / 255) << 16)
                 | (static_cast<uint32_t>(cg * tg / 255) << 8)
                 | static_cast<uint32_t>(cr * tr / 255);
        };

        for (int yy = bandY0; yy < bandY1; ++yy) {
            const FTaps yf = toFixed(yTaps[static_cast<size_t>(yy - y)], src.height);
            const uint32_t* rows[4] = {nullptr, nullptr, nullptr, nullptr};
            for (int k = 0; k < yf.n; ++k)
                rows[k] = hrowFor(yf.idx[k]);
            uint32_t* dstRow = fb_.pixels.data() + static_cast<size_t>(yy) * fbWidth_ + x0;
            if (opaque) {
                for (int dx = 0; dx < width; ++dx) {
                    uint64_t r = 0, g = 0, b = 0;
                    const size_t o = static_cast<size_t>(dx) * 3u;
                    for (int k = 0; k < yf.n; ++k) {
                        const uint64_t wk = yf.w[k];
                        r += wk * rows[k][o];
                        g += wk * rows[k][o + 1];
                        b += wk * rows[k][o + 2];
                    }
                    constexpr uint64_t kHalf = 1ull << 31;
                    const uint32_t rr = static_cast<uint32_t>(std::min<uint64_t>(255u, (r + kHalf) >> 32));
                    const uint32_t gg = static_cast<uint32_t>(std::min<uint64_t>(255u, (g + kHalf) >> 32));
                    const uint32_t bb = static_cast<uint32_t>(std::min<uint64_t>(255u, (b + kHalf) >> 32));
                    const uint32_t sp = tintPixel(0xFF000000u | (bb << 16) | (gg << 8) | rr);
                    dstRow[dx] = BlendPixel(dstRow[dx], sp, BlendMode::Alpha);
                }
            } else {
                for (int dx = 0; dx < width; ++dx) {
                    uint64_t a = 0, r = 0, g = 0, b = 0;
                    const size_t o = static_cast<size_t>(dx) * 4u;
                    for (int k = 0; k < yf.n; ++k) {
                        const uint64_t wk = yf.w[k];
                        a += wk * rows[k][o];
                        r += wk * rows[k][o + 1];
                        g += wk * rows[k][o + 2];
                        b += wk * rows[k][o + 3];
                    }
                    if (a == 0) continue;   // fully transparent: nothing to draw (the old path returned 0)
                    constexpr uint64_t kHalf = 1ull << 31;
                    const uint32_t aa = static_cast<uint32_t>(std::min<uint64_t>(255u, (a + kHalf) >> 32));
                    const uint32_t rr = static_cast<uint32_t>(std::min<uint64_t>(255u, (r + a / 2) / a));
                    const uint32_t gg = static_cast<uint32_t>(std::min<uint64_t>(255u, (g + a / 2) / a));
                    const uint32_t bb = static_cast<uint32_t>(std::min<uint64_t>(255u, (b + a / 2) / a));
                    const uint32_t sp = tintPixel((aa << 24) | (bb << 16) | (gg << 8) | rr);
                    dstRow[dx] = BlendPixel(dstRow[dx], sp, BlendMode::Alpha);
                }
            }
        }
        };   // renderBand

        // A 1080p frame is ~2 million destination pixels: split the rows over a few threads when the
        // blit is big enough to repay starting them (each band is independent, see renderBand).
        const int rowsTotal = y1 - y0;
        const size_t workPixels = static_cast<size_t>(width) * static_cast<size_t>(rowsTotal);
        ForRowBands(y0, y1, workPixels, renderBand);
        return;
    }
    // ---- 1:1 ALPHA-OVER, NO TINT: a row of opaque pixels is a straight copy ----------------------------
    // The scaled-image cache hands a video/background back at exactly its destination size, so this is
    // the path nearly every full-frame picture takes. Per-pixel blending (and its per-pixel branches) was
    // ~12 ms for a 1080p frame; an opaque row is one memcpy, and only a row holding translucent pixels
    // pays for blending.
    if (unitScale && mode == BlendMode::Alpha && !tinted && x1 > x0 && y1 > y0) {
        const int ox = static_cast<int>(sx0) - x;   // source column = destination column + ox
        const int oy = static_cast<int>(sy0) - y;
        const int cx0 = std::max(x0, -ox), cx1 = std::min(x1, src.width - ox);
        const int cy0 = std::max(y0, -oy), cy1 = std::min(y1, src.height - oy);
        if (cx1 <= cx0 || cy1 <= cy0) return;
        const size_t n = static_cast<size_t>(cx1 - cx0);
        ForRowBands(cy0, cy1, n * static_cast<size_t>(cy1 - cy0), [&](int bandY0, int bandY1) {
            for (int yy = bandY0; yy < bandY1; ++yy) {
                const uint32_t* s = src.pixels.data()
                                    + static_cast<size_t>(yy + oy) * static_cast<size_t>(src.width)
                                    + static_cast<size_t>(cx0 + ox);
                uint32_t* d = fb_.pixels.data() + static_cast<size_t>(yy) * static_cast<size_t>(fbWidth_)
                              + static_cast<size_t>(cx0);
                uint32_t acc = ~0u;   // alpha stays 255 only if every pixel's alpha is 255
                for (size_t i = 0; i < n; ++i) acc &= s[i];
                if ((acc >> 24) == 0xFFu) {
                    std::memcpy(d, s, n * sizeof(uint32_t));
                } else {
                    for (size_t i = 0; i < n; ++i)
                        d[i] = BlendPixel(d[i], s[i], BlendMode::Alpha);
                }
            }
        });
        return;
    }
    // ---- 1:1 ALPHA-OVER WITH A TINT (text: a coloured mask, mostly transparent) --------------------
    // The same walk without the per-pixel bounds checks, and a transparent source pixel — most of a
    // glyph or text-line bitmap — costs one load and one compare instead of a tint and a blend.
    if (unitScale && mode == BlendMode::Alpha && x1 > x0 && y1 > y0) {
        const int ox = static_cast<int>(sx0) - x;
        const int oy = static_cast<int>(sy0) - y;
        const int cx0 = std::max(x0, -ox), cx1 = std::min(x1, src.width - ox);
        const int cy0 = std::max(y0, -oy), cy1 = std::min(y1, src.height - oy);
        if (cx1 <= cx0 || cy1 <= cy0) return;
        for (int yy = cy0; yy < cy1; ++yy) {
            const uint32_t* s = src.pixels.data()
                                + static_cast<size_t>(yy + oy) * static_cast<size_t>(src.width)
                                + static_cast<size_t>(cx0 + ox);
            uint32_t* d = fb_.pixels.data() + static_cast<size_t>(yy) * static_cast<size_t>(fbWidth_)
                          + static_cast<size_t>(cx0);
            const int n = cx1 - cx0;
            for (int i = 0; i < n; ++i) {
                uint32_t sp = s[i];
                if ((sp >> 24) == 0u) continue;   // fully transparent: the destination is unchanged
                if (tinted) {
                    const int cr = static_cast<int>((sp >> 0) & 0xFF);
                    const int cg = static_cast<int>((sp >> 8) & 0xFF);
                    const int cb = static_cast<int>((sp >> 16) & 0xFF);
                    const int ca = static_cast<int>((sp >> 24) & 0xFF);
                    sp = (static_cast<uint32_t>(ca * ta / 255) << 24)
                       | (static_cast<uint32_t>(cb * tb / 255) << 16)
                       | (static_cast<uint32_t>(cg * tg / 255) << 8)
                       | static_cast<uint32_t>(cr * tr / 255);
                }
                d[i] = BlendPixel(d[i], sp, BlendMode::Alpha);
            }
        }
        return;
    }
    for (int yy = y0; yy < y1; ++yy)
        for (int xx = x0; xx < x1; ++xx) {
            uint32_t sp = 0;
            if (unitScale) {
                const int sx = static_cast<int>(sx0) + (xx - x);
                const int sy = static_cast<int>(sy0) + (yy - y);
                if (sx < 0 || sx >= src.width || sy < 0 || sy >= src.height) continue;
                sp = src.pixels[static_cast<size_t>(sy) * src.width + sx];
            } else {
                sp = CombineTaps(src, xTaps[static_cast<size_t>(xx - x)],
                                 yTaps[static_cast<size_t>(yy - y)]);
            }
            if (tinted) {
                // Pack is 0xAABBGGRR — R in the low byte.
                const int cr = static_cast<int>((sp >> 0) & 0xFF);
                const int cg = static_cast<int>((sp >> 8) & 0xFF);
                const int cb = static_cast<int>((sp >> 16) & 0xFF);
                const int ca = static_cast<int>((sp >> 24) & 0xFF);
                sp = (static_cast<uint32_t>(ca * ta / 255) << 24)
                   | (static_cast<uint32_t>(cb * tb / 255) << 16)
                   | (static_cast<uint32_t>(cg * tg / 255) << 8)
                   | static_cast<uint32_t>(cr * tr / 255);
            }
            uint32_t& dst = fb_.pixels[static_cast<size_t>(yy) * fbWidth_ + xx];
            dst = BlendPixel(dst, sp, mode);
        }
}

void SoftwareGraphicsBackend::RasterizeShape(const DrawCommand& cmd) {
    const Rect r = SnapRectToPixelEdges(cmd.rect);
    // Shapes are rasterized as filled rects/ellipses; polygons are handled by
    // the scene layer above as a list of rects in the common cases.
    if (cmd.color.a <= 0.0f) return;
    const auto mode = static_cast<BlendMode>(cmd.blend);
    if (cmd.cornerRadius >= 0.5f) {
        FillRoundedRectBlended(static_cast<int>(r.x), static_cast<int>(r.y),
                               static_cast<int>(r.width), static_cast<int>(r.height),
                               cmd.cornerRadius, cmd.color, mode);
        return;
    }
    FillRectBlended(static_cast<int>(r.x), static_cast<int>(r.y),
                    static_cast<int>(r.width), static_cast<int>(r.height), cmd.color, mode);
}
} // namespace bps::rendering