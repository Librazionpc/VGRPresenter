#include "modules/media/MediaEngine.hpp"

#include "core/logging/Logger.hpp"
#include "platform/IPlatform.hpp"
#include "platform/PlatformAccessor.hpp"

#include <algorithm>
#include <chrono>
#include <format>

namespace bps::media {

namespace {

// Default provider: detects the media type + metadata from the extension and
// the file metadata (PAL). No external codecs — real decoding is provider-
// plugin territory; playback state machine + pipeline live in the engine.
class DefaultMediaProvider final : public IMediaProvider {
public:
    const char* Name() const noexcept override { return "Default"; }

    std::vector<MediaFormat> SupportedFormats() const override {
        return {
            {"png", MediaType::Image, true},     {"jpg", MediaType::Image, true},
            {"jpeg", MediaType::Image, true},    {"webp", MediaType::Image, true},
            {"gif", MediaType::AnimatedImage, true}, {"bmp", MediaType::Image, true},
            {"tiff", MediaType::Image, true},    {"svg", MediaType::Svg, true},
            {"ico", MediaType::Icon, true},      {"ttf", MediaType::Font, true},
            {"otf", MediaType::Font, true},      {"mp4", MediaType::Video, true},
            {"mov", MediaType::Video, true},     {"mkv", MediaType::Video, true},
            {"avi", MediaType::Video, true},     {"webm", MediaType::Video, true},
            {"mp3", MediaType::Audio, true},     {"wav", MediaType::Audio, true},
            {"flac", MediaType::Audio, true},    {"ogg", MediaType::Audio, true},
            {"aac", MediaType::Audio, true},     {"m4a", MediaType::Audio, true},
        };
    }

    Result<MediaMetadata> ExtractMetadata(std::string_view path) const override {
        MediaMetadata meta;
        meta.name = std::string(path);
        try {
            auto& fs = platform::PlatformAccessor::Get().Filesystem();
            auto size = fs.FileSize(path);
            if (size.ok()) meta.sizeBytes = size.value();
            auto abs = fs.Absolute(path);
            if (abs.ok()) meta.name = abs.value();
        } catch (...) {
        }
        return meta;
    }

    bool SupportsHardwareDecode(const MediaFormat& format) const override {
        // Default: hardware decode for video when the Adaptive Runtime says so;
        // the engine asks the runtime rather than deciding here.
        return format.type == MediaType::Video;
    }
};

std::string ExtensionOf(std::string_view path) {
    auto pos = path.find_last_of('.');
    if (pos == std::string_view::npos) return {};
    std::string ext;
    for (char c : path.substr(pos + 1))
        ext.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    return ext;
}

// Deterministic testable thumbnails: solid color derived from the id + a size.
std::vector<uint8_t> SolidThumbnail(const std::string& id, int w, int h) {
    std::vector<uint8_t> px(static_cast<size_t>(w) * h * 4, 0);
    uint8_t r = static_cast<uint8_t>((id.size() * 37) & 0xFF);
    uint8_t g = static_cast<uint8_t>((id.size() * 91 + 40) & 0xFF);
    uint8_t b = static_cast<uint8_t>((id.size() * 53 + 120) & 0xFF);
    for (size_t i = 0; i < static_cast<size_t>(w) * h; ++i) {
        px[i * 4 + 0] = r;
        px[i * 4 + 1] = g;
        px[i * 4 + 2] = b;
        px[i * 4 + 3] = 255;
    }
    return px;
}

} // namespace

MediaEngine& MediaEngine::Instance() {
    static MediaEngine instance;
    return instance;
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------
Result<void> MediaEngine::Initialize() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (initialized_.load()) return Ok();
    initialized_.store(true);
    providers_.push_back(std::make_shared<DefaultMediaProvider>());
    WireEvents();
    return Ok();
}

Result<void> MediaEngine::Start() {
    running_.store(true);
    return Ok();
}

Result<void> MediaEngine::Stop() {
    running_.store(false);
    return Ok();
}

Result<void> MediaEngine::Shutdown() {
    if (!initialized_.load()) return Ok();
    UnwireEvents();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        providers_.clear();
        library_.clear();
        playback_.clear();
        cache_.clear();
        pending_.clear();
        initialized_.store(false);
    }
    return Ok();
}

Result<void> MediaEngine::Reload() {
    return Ok();
}

Result<void> MediaEngine::Reset() {
    std::lock_guard<std::mutex> lock(mutex_);
    playback_.clear();
    cache_.clear();
    return Ok();
}

HealthReport MediaEngine::GetHealth() const {
    HealthReport r;
    r.state = HealthState::Healthy;
    r.detail = std::format("media={} providers={}", MediaCount(), providers_.size());
    return r;
}

Metrics MediaEngine::MetricsSnapshot() const {
    Metrics m;
    m.queueLength = pending_.size();
    m.errorCount = errorCount_.load();
    m.health = HealthState::Healthy;
    return m;
}

// ---------------------------------------------------------------------------
// Providers + formats
// ---------------------------------------------------------------------------
Result<void> MediaEngine::RegisterProvider(std::shared_ptr<IMediaProvider> provider) {
    if (!provider) return Error::Make(Err::InvalidArgument, "MediaEngine", "null provider");
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& p : providers_)
        if (std::string_view(p->Name()) == provider->Name())
            return Error::Make(Err::Media_AlreadyExists, "MediaEngine",
                               "provider '" + std::string(provider->Name()) + "' already registered");
    providers_.push_back(std::move(provider));
    return Ok();
}

Result<MediaFormat> MediaEngine::ResolveFormat(std::string_view extension) const {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& p : providers_) {
        for (const auto& f : p->SupportedFormats()) {
            if (f.extension == extension) return f;
        }
    }
    return Error::Make(Err::Media_UnsupportedFormat, "MediaEngine",
                       "unsupported format '" + std::string(extension) + "'");
}

bool MediaEngine::IsFormatSupported(std::string_view extension) const {
    return ResolveFormat(extension).ok();
}

// ---------------------------------------------------------------------------
// Library
// ---------------------------------------------------------------------------
Result<std::string> MediaEngine::Import(std::string_view path, std::string_view name) {
    std::string ext = ExtensionOf(path);
    auto format = ResolveFormat(ext);
    if (!format.ok()) return format.error();

    std::string id = std::format("media-{}", library_.size() + 1);
    MediaAsset asset;
    asset.id = id;
    asset.name = name.empty() ? std::string(path) : std::string(name);
    asset.path = std::string(path);
    asset.type = format.value().type;
    asset.extension = ext;
    asset.importedAt = std::chrono::system_clock::now();

    // Metadata extraction through the first provider that supports the format.
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& p : providers_) {
            for (const auto& f : p->SupportedFormats()) {
                if (f.extension == ext) {
                    auto meta = p->ExtractMetadata(path);
                    if (meta.ok()) {
                        asset.metadata = meta.value();
                        asset.metadata.name = asset.name;
                    }
                    break;
                }
            }
        }
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        library_[id] = asset;
        pending_.push_back(id);
    }
    (void)EventBus::Instance().Publish(events::MediaImported{id, asset.name, ToString(asset.type)});
    return id;
}

Result<void> MediaEngine::Remove(std::string_view mediaId) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = library_.find(std::string(mediaId));
    if (it == library_.end())
        return Error::Make(Err::Media_NotFound, "MediaEngine",
                           "media '" + std::string(mediaId) + "' not found");
    library_.erase(it);
    playback_.erase(std::string(mediaId));
    (void)EventBus::Instance().Publish(events::MediaRemoved{std::string(mediaId)});
    return Ok();
}

Result<MediaAsset> MediaEngine::Get(std::string_view mediaId) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = library_.find(std::string(mediaId));
    if (it == library_.end())
        return Error::Make(Err::Media_NotFound, "MediaEngine",
                           "media '" + std::string(mediaId) + "' not found");
    return it->second;
}

std::vector<std::string> MediaEngine::MediaIds() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::string> ids;
    for (const auto& [id, _] : library_) ids.push_back(id);
    return ids;
}

size_t MediaEngine::MediaCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return library_.size();
}

// ---------------------------------------------------------------------------
// Thumbnails
// ---------------------------------------------------------------------------
Result<void> MediaEngine::GenerateThumbnail(std::string_view mediaId, int width, int height) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = library_.find(std::string(mediaId));
    if (it == library_.end())
        return Error::Make(Err::Media_NotFound, "MediaEngine",
                           "media '" + std::string(mediaId) + "' not found");
    it->second.thumbnailPixels = SolidThumbnail(std::string(mediaId), width, height);
    it->second.thumbnailW = width;
    it->second.thumbnailH = height;
    it->second.hasThumbnail = true;
    (void)EventBus::Instance().Publish(
        events::MediaThumbnailGenerated{std::string(mediaId), width, height});
    return Ok();
}

bool MediaEngine::HasThumbnail(std::string_view mediaId) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = library_.find(std::string(mediaId));
    return it != library_.end() && it->second.hasThumbnail;
}

// ---------------------------------------------------------------------------
// Playback
// ---------------------------------------------------------------------------
Result<void> MediaEngine::Play(std::string_view mediaId, DecodePath path) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = library_.find(std::string(mediaId));
    if (it == library_.end())
        return Error::Make(Err::Media_NotFound, "MediaEngine",
                           "media '" + std::string(mediaId) + "' not found");
    auto& pb = playback_[std::string(mediaId)];
    pb.state = PlaybackState::Playing;
    pb.path = path;
    if (path == DecodePath::Hardware) {
        // Hardware decode unavailable → graceful software fallback.
        bool hw = false;
        for (const auto& p : providers_)
            for (const auto& f : p->SupportedFormats())
                if (f.extension == it->second.extension && p->SupportsHardwareDecode(f)) hw = true;
        if (!hw) {
            pb.path = DecodePath::Software;
            Logger::Instance().Debug("Hardware decode unavailable — software fallback for '" +
                                         it->second.name + "'",
                                     "MediaEngine");
        }
    }
    (void)EventBus::Instance().Publish(
        events::MediaPlaybackStarted{std::string(mediaId), ToString(pb.path)});
    return Ok();
}

Result<void> MediaEngine::Pause(std::string_view mediaId) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = playback_.find(std::string(mediaId));
    if (it == playback_.end())
        return Error::Make(Err::Media_NotFound, "MediaEngine",
                           "media '" + std::string(mediaId) + "' not playing");
    if (it->second.state == PlaybackState::Playing) it->second.state = PlaybackState::Paused;
    return Ok();
}

Result<void> MediaEngine::Resume(std::string_view mediaId) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = playback_.find(std::string(mediaId));
    if (it == playback_.end())
        return Error::Make(Err::Media_NotFound, "MediaEngine",
                           "media '" + std::string(mediaId) + "' not found");
    if (it->second.state == PlaybackState::Paused) it->second.state = PlaybackState::Playing;
    return Ok();
}

Result<void> MediaEngine::Stop(std::string_view mediaId) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = playback_.find(std::string(mediaId));
    if (it == playback_.end()) return Ok();
    it->second.state = PlaybackState::Stopped;
    it->second.positionSec = 0.0;
    (void)EventBus::Instance().Publish(events::MediaPlaybackStopped{std::string(mediaId)});
    return Ok();
}

Result<void> MediaEngine::Seek(std::string_view mediaId, double seconds) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = playback_.find(std::string(mediaId));
    if (it == playback_.end())
        return Error::Make(Err::Media_NotFound, "MediaEngine",
                           "media '" + std::string(mediaId) + "' not found");
    it->second.positionSec = std::max(0.0, seconds);
    return Ok();
}

Result<void> MediaEngine::SetPlaybackRate(std::string_view mediaId, double rate) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = playback_.find(std::string(mediaId));
    if (it == playback_.end())
        return Error::Make(Err::Media_NotFound, "MediaEngine",
                           "media '" + std::string(mediaId) + "' not found");
    it->second.rate = std::max(0.0, rate);
    return Ok();
}

Result<void> MediaEngine::SetLoop(std::string_view mediaId, bool loop) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = playback_.find(std::string(mediaId));
    if (it == playback_.end())
        return Error::Make(Err::Media_NotFound, "MediaEngine",
                           "media '" + std::string(mediaId) + "' not found");
    it->second.loop = loop;
    return Ok();
}

PlaybackState MediaEngine::GetPlaybackState(std::string_view mediaId) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = playback_.find(std::string(mediaId));
    return it == playback_.end() ? PlaybackState::Idle : it->second.state;
}

double MediaEngine::GetPosition(std::string_view mediaId) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = playback_.find(std::string(mediaId));
    return it == playback_.end() ? 0.0 : it->second.positionSec;
}

Result<void> MediaEngine::StepFrame(std::string_view mediaId) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = playback_.find(std::string(mediaId));
    if (it == playback_.end())
        return Error::Make(Err::Media_NotFound, "MediaEngine",
                           "media '" + std::string(mediaId) + "' not found");
    if (it->second.state != PlaybackState::Paused)
        return Error::Make(Err::Media_DecodeFailed, "MediaEngine",
                           "frame stepping requires Paused state");
    it->second.frames++;
    return Ok();
}

// ---------------------------------------------------------------------------
// Cache
// ---------------------------------------------------------------------------
size_t MediaEngine::CacheEntries() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return cache_.size();
}

void MediaEngine::TrimCache(size_t maxEntries) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (cache_.size() <= maxEntries) return;
    std::sort(cache_.begin(), cache_.end(),
              [](const auto& a, const auto& b) { return a.lastUsedMs < b.lastUsedMs; });
    cache_.resize(maxEntries);
}

// ---------------------------------------------------------------------------
// Background processing
// ---------------------------------------------------------------------------
Result<void> MediaEngine::ProcessQueue(size_t batch) {
    std::vector<std::string> take;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        size_t n = std::min(batch, pending_.size());
        for (size_t i = 0; i < n; ++i) take.push_back(pending_[i]);
        pending_.erase(pending_.begin(), pending_.begin() + static_cast<long>(n));
    }
    for (const auto& id : take) {
        // Thumbnail generation for imported assets (background).
        auto asset = Get(id);
        if (asset.ok() && !asset.value().hasThumbnail) (void)GenerateThumbnail(id, 128, 128);
        // Cache bookkeeping.
        std::lock_guard<std::mutex> lock(mutex_);
        auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::steady_clock::now().time_since_epoch())
                       .count();
        cache_.push_back(CacheEntry{id, static_cast<uint64_t>(now)});
        if (cache_.size() > cacheMax_) cache_.erase(cache_.begin());
        processedCount_.fetch_add(1);
    }
    return Ok();
}

// ---------------------------------------------------------------------------
// Events
// ---------------------------------------------------------------------------
void MediaEngine::WireEvents() {
    auto& bus = EventBus::Instance();
    subscriptions_.push_back(bus.Subscribe<events::ConfigHotReload>(
        [this](const events::ConfigHotReload& e) { OnConfigReload(e); }, 0));
    subscriptions_.push_back(bus.Subscribe<events::ResourcePressureChanged>(
        [this](const events::ResourcePressureChanged& e) { OnPressure(e); }, 0));
}

void MediaEngine::UnwireEvents() {
    auto& bus = EventBus::Instance();
    for (auto& s : subscriptions_)
        if (s.Valid()) (void)bus.Unsubscribe(s);
    subscriptions_.clear();
}

void MediaEngine::OnConfigReload(const events::ConfigHotReload&) {
    // Nothing to reconfigure at runtime level today.
}

void MediaEngine::OnPressure(const events::ResourcePressureChanged& e) {
    if (e.to == PressureLevel::High || e.to == PressureLevel::Critical)
        TrimCache(64);
}

} // namespace bps::media
