#include "modules/media/ThumbnailCache.hpp"

#include "modules/media/MediaLibrary.hpp"   // MediaPathHash
#include "modules/rendering/PngCodec.hpp"
#include "platform/PlatformAccessor.hpp"

#include <algorithm>
#include <format>

namespace bps::media {

namespace {

constexpr const char* kModule = "ThumbnailCache";

// A file that failed is not tried again for this long (FreeShow lets a failure be retried on
// the next request; a short pause stops a scrolling grid re-asking for the same bad file).
constexpr auto kRetryAfterFailure = std::chrono::seconds(3);

// The video frame a still is taken from: the middle (FreeShow's seek 0.5).
constexpr double kStillFraction = 0.5;

// The video position of moving-frame `step`: (step + 1) / kFrameSteps of the way through,
// held a little short of the very end where there is often no frame to decode.
double FrameFraction(int step) {
    return std::min(0.98, static_cast<double>(step + 1) / ThumbnailCache::kFrameSteps);
}

} // namespace

ThumbnailCache::ThumbnailCache(std::string cacheDir, std::shared_ptr<IFrameSource> source,
                               int maxConcurrent)
    : cacheDir_(std::move(cacheDir)),
      source_(std::move(source)),
      maxConcurrent_(std::max(1, maxConcurrent)) {}

int ThumbnailCache::RoundSize(int requested) {
    for (int size : kSizes)
        if (size >= requested) return size;
    return kSizes.back();
}

std::string ThumbnailCache::CachePath(std::string_view mediaPath, int size, int step) const {
    auto& fs = platform::PlatformAccessor::Get().Filesystem();
    const std::string name = step < 0
        ? std::format("{}-{}.png", MediaPathHash(mediaPath), size)
        : std::format("{}-{}-f{}.png", MediaPathHash(mediaPath), size, step);
    return fs.Join(cacheDir_, name);
}

bool ThumbnailCache::Fresh(const std::string& outPath, const std::string& mediaPath) const {
    auto& fs = platform::PlatformAccessor::Get().Filesystem();
    auto cached = fs.Metadata(outPath);
    if (!cached.ok() || !cached.value().isRegularFile || cached.value().sizeBytes == 0) return false;
    auto source = fs.Metadata(mediaPath);
    if (!source.ok()) return false;
    // FreeShow: a source newer than its thumbnail means the thumbnail is stale.
    return cached.value().modifiedEpochNs >= source.value().modifiedEpochNs;
}

Result<std::string> ThumbnailCache::Still(std::string_view mediaPath, MediaKind kind, int size) {
    return Ensure(mediaPath, kind, RoundSize(size), -1);
}

Result<std::string> ThumbnailCache::Frame(std::string_view mediaPath, int step, int size) {
    if (step < 0 || step >= kFrameSteps)
        return Error::Make(Err::InvalidArgument, kModule, std::format("no frame step {}", step));
    return Ensure(mediaPath, MediaKind::Video, RoundSize(size), step);
}

void ThumbnailCache::Invalidate(std::string_view mediaPath) {
    auto& fs = platform::PlatformAccessor::Get().Filesystem();
    for (int size : kSizes) {
        (void)fs.Remove(CachePath(mediaPath, size, -1));
        for (int step = 0; step < kFrameSteps; ++step)
            (void)fs.Remove(CachePath(mediaPath, size, step));
    }
}

Result<std::string> ThumbnailCache::Ensure(std::string_view mediaPathView, MediaKind kind, int size,
                                           int step) {
    auto& fs = platform::PlatformAccessor::Get().Filesystem();
    const std::string mediaPath(mediaPathView);
    if (!fs.Exists(mediaPath)) {
        Invalidate(mediaPath);   // FreeShow's doesMediaExist: a missing file loses its cache
        return Error::Make(Err::NotFound, kModule, std::format("'{}' does not exist", mediaPath));
    }

    const std::string outPath = CachePath(mediaPath, size, step);
    if (Fresh(outPath, mediaPath)) return outPath;

    std::unique_lock<std::mutex> lock(mutex_);
    // Someone else is already making this exact picture: wait for them, then use theirs.
    while (inFlight_.count(outPath)) cv_.wait(lock);
    if (Fresh(outPath, mediaPath)) return outPath;

    if (auto failed = failedAt_.find(outPath); failed != failedAt_.end()) {
        if (std::chrono::steady_clock::now() - failed->second < kRetryAfterFailure)
            return Error::Make(Err::IoError, kModule, "this picture could not be made a moment ago");
        failedAt_.erase(failed);
    }

    inFlight_.insert(outPath);
    cv_.wait(lock, [this] { return active_ < maxConcurrent_; });   // a free decode slot
    ++active_;
    lock.unlock();

    (void)fs.Remove(outPath);   // stale (older than its source), or absent
    Result<void> made = Generate(mediaPath, kind, size, step, outPath);

    lock.lock();
    --active_;
    inFlight_.erase(outPath);
    if (made.ok()) failedAt_.erase(outPath);
    else failedAt_[outPath] = std::chrono::steady_clock::now();
    cv_.notify_all();
    lock.unlock();

    if (!made.ok()) return made.error();
    return outPath;
}

Result<void> ThumbnailCache::Generate(const std::string& mediaPath, MediaKind kind, int size, int step,
                                      const std::string& outPath) {
    // Only a video has positions to look at; an image or an audio file's cover art is just itself.
    const double fraction = kind != MediaKind::Video ? 0.0
                            : (step < 0 ? kStillFraction : FrameFraction(step));
    auto frame = source_->Grab(mediaPath, kind, fraction, size);
    if (!frame.ok()) return frame.error();
    if (frame.value().empty())
        return Error::Make(Err::IoError, kModule, "the decoder returned an empty picture");

    auto png = rendering::EncodePngRgba8(frame.value().rgba.data(), frame.value().width,
                                         frame.value().height);
    if (!png.ok()) return png.error();

    auto& fs = platform::PlatformAccessor::Get().Filesystem();
    if (auto made = fs.CreateDirectories(cacheDir_); !made.ok()) return made.error();
    // Written beside the final name and renamed, so a half-written picture is never read.
    const std::string tmp = outPath + ".tmp";
    if (auto wrote = fs.WriteBinary(tmp, png.value()); !wrote.ok()) return wrote.error();
    if (auto moved = fs.Rename(tmp, outPath); !moved.ok()) {
        (void)fs.Remove(tmp);
        return moved.error();
    }
    ++generated_;
    return {};
}

} // namespace bps::media
