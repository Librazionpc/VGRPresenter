#pragma once

#include "core/common/Common.hpp"
#include "modules/media/FrameSource.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <string_view>

namespace bps::media {
class ThumbnailCache {
public:
    static constexpr int kFrameSteps = 10;
    static constexpr std::array<int, 4> kSizes = {100, 250, 500, 900};

    // `cacheDir` is created when the first picture is written.
    ThumbnailCache(std::string cacheDir, std::shared_ptr<IFrameSource> source,
                   int maxConcurrent = 4);

    // The smallest cache size that is at least `requested` (the largest if none is).
    static int RoundSize(int requested);

    // Where a file's picture is (or would be) cached. step < 0 = the still.
    std::string CachePath(std::string_view mediaPath, int size, int step = -1) const;

    // The path of the cached still (PNG), making it first if needed. NotFound when the
    // source file is gone; Unsupported / IoError when it cannot be decoded.
    Result<std::string> Still(std::string_view mediaPath, MediaKind kind, int size = 250);

    // The path of the cached frame for `step` (0 .. kFrameSteps-1, i.e. at
    // (step+1)/kFrameSteps of the video). Videos only.
    Result<std::string> Frame(std::string_view mediaPath, int step, int size = 250);

    // Deletes every cached size and frame of a file (the file was removed or replaced).
    void Invalidate(std::string_view mediaPath);

    // How many pictures have been decoded so far (cache hits do not count).
    size_t GeneratedCount() const { return generated_.load(); }

private:
    Result<std::string> Ensure(std::string_view mediaPath, MediaKind kind, int size, int step);
    Result<void> Generate(const std::string& mediaPath, MediaKind kind, int size, int step,
                          const std::string& outPath);
    bool Fresh(const std::string& outPath, const std::string& mediaPath) const;

    std::string cacheDir_;
    std::shared_ptr<IFrameSource> source_;
    const int maxConcurrent_;

    std::mutex mutex_;
    std::condition_variable cv_;
    std::set<std::string> inFlight_;                 // cache paths being made right now
    int active_ = 0;                                 // decodes running (<= maxConcurrent_)
    std::map<std::string, std::chrono::steady_clock::time_point> failedAt_;
    std::atomic<size_t> generated_{0};
};
} // namespace bps::media