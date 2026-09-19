#pragma once

// MediaEngine (docs/specs/21): the Phase 10 facade. Owns the media library
// (assets + metadata + thumbnails), the format registry, the background
// pipeline, the playback engine, and the caches. Providers add formats/codecs/
// decoders without engine changes.

#include "core/common/Common.hpp"
#include "core/events/EventBus.hpp"
#include "core/events/Events.hpp"
#include "interfaces/IService.hpp"
#include "modules/media/MediaTypes.hpp"
#include "modules/rendering/RenderTypes.hpp"

#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace bps::media {

class IMediaProvider {
public:
    virtual ~IMediaProvider() = default;
    virtual const char* Name() const noexcept = 0;
    // Formats this provider can decode.
    virtual std::vector<MediaFormat> SupportedFormats() const = 0;
    // Extracts metadata from a file (extension + raw bytes).
    virtual Result<MediaMetadata> ExtractMetadata(std::string_view path) const = 0;
    // Returns true when hardware acceleration is available for this format.
    virtual bool SupportsHardwareDecode(const MediaFormat& format) const = 0;
};

class MediaEngine final : public IService {
public:
    static MediaEngine& Instance();

    // --- Lifecycle ---
    Result<void> Initialize() override;
    Result<void> Start() override;
    Result<void> Stop() override;
    Result<void> Shutdown() override;
    Result<void> Reload() override;
    Result<void> Reset() override;
    HealthReport GetHealth() const override;
    Metrics MetricsSnapshot() const override;
    const char* ServiceName() const noexcept override { return "MediaEngine"; }

    // --- Format registry + providers ---
    Result<void> RegisterProvider(std::shared_ptr<IMediaProvider> provider);
    Result<MediaFormat> ResolveFormat(std::string_view extension) const;
    bool IsFormatSupported(std::string_view extension) const;

    // --- Library ---
    // Imports a media file: registers the asset, extracts metadata, generates a
    // thumbnail, publishes media.imported/media.ready.
    Result<std::string> Import(std::string_view path, std::string_view name = {});
    Result<void> Remove(std::string_view mediaId);
    Result<MediaAsset> Get(std::string_view mediaId) const;
    std::vector<std::string> MediaIds() const;
    size_t MediaCount() const;

    // --- Thumbnails ---
    Result<void> GenerateThumbnail(std::string_view mediaId, int width = 128, int height = 128);
    bool HasThumbnail(std::string_view mediaId) const;

    // --- Playback (docs/specs/21 §Playback) ---
    Result<void> Play(std::string_view mediaId, DecodePath path = DecodePath::Auto);
    Result<void> Pause(std::string_view mediaId);
    Result<void> Resume(std::string_view mediaId);
    Result<void> Stop(std::string_view mediaId);
    Result<void> Seek(std::string_view mediaId, double seconds);
    Result<void> SetPlaybackRate(std::string_view mediaId, double rate);
    Result<void> SetLoop(std::string_view mediaId, bool loop);
    PlaybackState GetPlaybackState(std::string_view mediaId) const;
    double GetPosition(std::string_view mediaId) const;
    // Frame step (video): advances one frame in Paused state.
    Result<void> StepFrame(std::string_view mediaId);

    // --- Cache ---
    size_t CacheEntries() const;
    void TrimCache(size_t maxEntries);   // LRU eviction

    // --- Background processing hook (called by the Kernel's scheduler tick) ---
    Result<void> ProcessQueue(size_t batch = 4);

    // --- Events ---
    void WireEvents();
    void UnwireEvents();
    void OnConfigReload(const events::ConfigHotReload& e);
    void OnPressure(const events::ResourcePressureChanged& e);

private:
    MediaEngine() = default;

    mutable std::mutex mutex_;
    std::vector<std::shared_ptr<IMediaProvider>> providers_;
    std::map<std::string, MediaAsset, std::less<>> library_;
    std::vector<std::string> pending_;              // background queue
    struct Playback {
        PlaybackState state = PlaybackState::Idle;
        double positionSec = 0.0;
        double rate = 1.0;
        bool loop = false;
        DecodePath path = DecodePath::Auto;
        uint64_t frames = 0;
    };
    std::map<std::string, Playback, std::less<>> playback_;
    struct CacheEntry {
        std::string mediaId;
        uint64_t lastUsedMs = 0;
    };
    std::vector<CacheEntry> cache_;
    size_t cacheMax_ = 256;
    std::vector<Subscription> subscriptions_;
    std::atomic<bool> initialized_{false};
    std::atomic<bool> running_{false};
    std::atomic<uint64_t> errorCount_{0};
    std::atomic<uint64_t> processedCount_{0};
};

} // namespace bps::media
