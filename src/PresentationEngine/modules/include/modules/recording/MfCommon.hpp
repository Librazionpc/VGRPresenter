#pragma once

#include "core/common/Common.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <objbase.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfobjects.h>
#include <mfreadwrite.h>
#include <mftransform.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace bps::recording::mf {
// Owns one COM interface pointer and releases it once.
template <typename T>
class ComRef {
public:
    ComRef() = default;
    ComRef(const ComRef&) = delete;
    ComRef& operator=(const ComRef&) = delete;
    ComRef(ComRef&& other) noexcept : p_(other.p_) { other.p_ = nullptr; }
    ComRef& operator=(ComRef&& other) noexcept {
        if (this != &other) {
            Reset();
            p_ = other.p_;
            other.p_ = nullptr;
        }
        return *this;
    }
    ~ComRef() { Reset(); }

    T* get() const { return p_; }
    T* operator->() const { return p_; }
    explicit operator bool() const { return p_ != nullptr; }
    // For the calls that fill an out-pointer.
    T** Out() {
        Reset();
        return &p_;
    }
    void Reset() {
        if (p_) {
            p_->Release();
            p_ = nullptr;
        }
    }

private:
    T* p_ = nullptr;
};

// Media Foundation is started once per user of it (reference counted, like the MP4
// container's own count), so MFShutdown runs only when the last user lets go.
class Session {
public:
    Session() = default;
    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;
    ~Session() { Release(); }

    bool Acquire() {
        if (held_) return true;
        std::lock_guard<std::mutex> lock(Mutex());
        if (Users() == 0 && FAILED(MFStartup(MF_VERSION, MFSTARTUP_FULL))) return false;
        ++Users();
        held_ = true;
        return true;
    }
    void Release() {
        if (!held_) return;
        std::lock_guard<std::mutex> lock(Mutex());
        if (Users() > 0 && --Users() == 0) MFShutdown();
        held_ = false;
    }

private:
    static std::mutex& Mutex() {
        static std::mutex m;
        return m;
    }
    static int& Users() {
        static int n = 0;
        return n;
    }
    bool held_ = false;
};

inline std::string HrText(std::string_view what, HRESULT hr) {
    char code[16];
    std::snprintf(code, sizeof code, "0x%08lX", static_cast<unsigned long>(hr));
    return std::string(what) + " failed (" + code + ")";
}

inline uint8_t Clamp8(int v) { return static_cast<uint8_t>(std::clamp(v, 0, 255)); }

inline uint32_t SampleAt(const uint32_t* src, int srcW, int srcH, int x, int y, int w, int h) {
    const int sx = std::min(srcW - 1, static_cast<int>((static_cast<int64_t>(x) * srcW) / w));
    const int sy = std::min(srcH - 1, static_cast<int>((static_cast<int64_t>(y) * srcH) / h));
    return src[static_cast<size_t>(sy) * static_cast<size_t>(srcW) + static_cast<size_t>(sx)];
}

// Packed RGBA to NV12 (BT.601, limited range): a full-resolution Y plane, then one
// interleaved UV plane at half resolution. `w` and `h` must be even.
inline void PackedToNv12(const uint32_t* src, int srcW, int srcH, uint8_t* dst, int w, int h) {
    uint8_t* yPlane = dst;
    uint8_t* uvPlane = dst + static_cast<size_t>(w) * static_cast<size_t>(h);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const uint32_t px = SampleAt(src, srcW, srcH, x, y, w, h);
            const int r = static_cast<int>(px & 0xFF);
            const int g = static_cast<int>((px >> 8) & 0xFF);
            const int b = static_cast<int>((px >> 16) & 0xFF);
            yPlane[static_cast<size_t>(y) * w + x] = Clamp8(((66 * r + 129 * g + 25 * b + 128) >> 8) + 16);
        }
    }
    for (int cy = 0; cy < h / 2; ++cy) {
        for (int cx = 0; cx < w / 2; ++cx) {
            int sr = 0, sg = 0, sb = 0;
            for (int dy = 0; dy < 2; ++dy) {
                for (int dx = 0; dx < 2; ++dx) {
                    const uint32_t px = SampleAt(src, srcW, srcH, cx * 2 + dx, cy * 2 + dy, w, h);
                    sr += static_cast<int>(px & 0xFF);
                    sg += static_cast<int>((px >> 8) & 0xFF);
                    sb += static_cast<int>((px >> 16) & 0xFF);
                }
            }
            const int r = sr / 4, g = sg / 4, b = sb / 4;
            uint8_t* uv = uvPlane + static_cast<size_t>(cy) * w + static_cast<size_t>(cx) * 2;
            uv[0] = Clamp8(((-38 * r - 74 * g + 112 * b + 128) >> 8) + 128);
            uv[1] = Clamp8(((112 * r - 94 * g - 18 * b + 128) >> 8) + 128);
        }
    }
}

inline int16_t ToPcm16(float sample) {
    const float clamped = std::clamp(sample, -1.0f, 1.0f);
    return static_cast<int16_t>(std::lrint(clamped * 32767.0f));
}

// Media Foundation time is in 100 ns units.
inline int64_t MsToHns(double milliseconds) {
    return static_cast<int64_t>(std::llround(milliseconds * 10000.0));
}
} // namespace bps::recording::mf