#include "modules/media/MediaModule.hpp"

#include "core/events/EventBus.hpp"
#include "core/events/Events.hpp"
#include "core/logging/Logger.hpp"

#include <chrono>

namespace bps::modules {

namespace {

constexpr int kStateStopped = static_cast<int>(MediaPlaybackState::Stopped);
constexpr int kStatePlaying = static_cast<int>(MediaPlaybackState::Playing);
constexpr int kStatePaused = static_cast<int>(MediaPlaybackState::Paused);
constexpr int kStateFinished = static_cast<int>(MediaPlaybackState::Finished);

} // namespace

std::shared_ptr<MediaModule> MediaModule::Create() {
    return std::shared_ptr<MediaModule>(new MediaModule());
}

const ModuleManifest& MediaModule::Manifest() const {
    static ModuleManifest m;
    m.id = "media";
    m.version = kEngineVersion;
    m.author = "BPS Core";
    m.requiredCoreVersion = kEngineVersion;
    m.capabilities = {"media", "playback"};
    return m;
}

Result<void> MediaModule::OnLoad() {
    Logger::Instance().Info("Media module loaded", "Media");
    return Ok();
}

Result<void> MediaModule::OnUnload() {
    (void)CancelPlaybackTask();
    std::lock_guard<std::mutex> lock(mutex_);
    current_.reset();
    playbackState_.store(kStateStopped);
    positionSec_.store(0.0);
    return Ok();
}

Result<void> MediaModule::OnStart() {
    started_.store(true);
    Logger::Instance().Info("Media module started", "Media");
    return Ok();
}

Result<void> MediaModule::OnStop() {
    started_.store(false);
    (void)CancelPlaybackTask();
    Logger::Instance().Info("Media module stopped", "Media");
    return Ok();
}

Result<void> MediaModule::Enqueue(MediaItem item) {
    if (item.id.empty() || item.durationSec <= 0.0)
        return Error::Make(Err::InvalidArgument, "MediaModule",
                           "a media item needs an id and a positive duration");
    // Copy the key BEFORE the move: `playlist_[item.id] = make_shared(move(item))`
    // leaves the subscript's key evaluation unsequenced with the move, which can
    // store the item under a moved-from (empty) key.
    std::string key = item.id;
    std::lock_guard<std::mutex> lock(mutex_);
    if (playlist_.count(key) > 0)
        return Error::Make(Err::AlreadyExists, "MediaModule",
                           "item already in playlist: " + key);
    playlist_[std::move(key)] = std::make_shared<MediaItem>(std::move(item));
    return Ok();
}

Result<void> MediaModule::Remove(std::string_view id) {
    bool removingCurrent = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = playlist_.find(std::string(id));
        if (it == playlist_.end())
            return Error::Make(Err::NotFound, "MediaModule",
                               "item not found: " + std::string(id));
        removingCurrent = (current_ == it->second);
        playlist_.erase(it);
    }
    if (removingCurrent) {
        (void)CancelPlaybackTask();   // outside the lock, like every other cancel
        std::lock_guard<std::mutex> lock(mutex_);
        current_.reset();
        playbackState_.store(kStateStopped);
        positionSec_.store(0.0);
    }
    return Ok();
}

size_t MediaModule::PlaylistSize() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return playlist_.size();
}

std::vector<MediaItem> MediaModule::Playlist() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<MediaItem> out;
    out.reserve(playlist_.size());
    for (const auto& [id, item] : playlist_) out.push_back(*item);
    return out;
}

Result<void> MediaModule::Play(std::string_view id) {
    std::shared_ptr<MediaItem> item;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = playlist_.find(std::string(id));
        if (it == playlist_.end())
            return Error::Make(Err::NotFound, "MediaModule",
                               "item not found: " + std::string(id));
        item = it->second;
    }
    (void)CancelPlaybackTask();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        current_ = item;
    }
    positionSec_.store(0.0);
    playbackState_.store(kStatePlaying);
    if (auto r = StartClock(item->durationSec, item->id); !r.ok()) {
        playbackState_.store(kStateStopped);
        return r;
    }
    (void)EventBus::Instance().Publish(events::MediaStateChanged{
        item->id, item->title, kStatePlaying, 0.0, item->durationSec});
    Logger::Instance().Info("Media playing: " + item->title, "Media");
    return Ok();
}

Result<void> MediaModule::Pause() {
    if (playbackState_.load() != kStatePlaying)
        return Error::Make(Err::InvalidState, "MediaModule", "nothing is playing");
    (void)CancelPlaybackTask();
    playbackState_.store(kStatePaused);
    std::shared_ptr<MediaItem> item;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        item = current_;
    }
    if (item)
        (void)EventBus::Instance().Publish(events::MediaStateChanged{
            item->id, item->title, kStatePaused, positionSec_.load(), item->durationSec});
    Logger::Instance().Info("Media paused at " + std::to_string(positionSec_.load()) + "s", "Media");
    return Ok();
}

Result<void> MediaModule::Resume() {
    if (playbackState_.load() != kStatePaused)
        return Error::Make(Err::InvalidState, "MediaModule", "nothing is paused");
    std::shared_ptr<MediaItem> item;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        item = current_;
    }
    if (!item) return Error::Make(Err::InvalidState, "MediaModule", "no current item");
    playbackState_.store(kStatePlaying);
    double remaining = item->durationSec - positionSec_.load();
    if (remaining <= 0.0) {
        FinishPlayback(item->id);
        return Ok();
    }
    if (auto r = StartClock(remaining, item->id); !r.ok()) {
        playbackState_.store(kStatePaused);
        return r;
    }
    (void)EventBus::Instance().Publish(events::MediaStateChanged{
        item->id, item->title, kStatePlaying, positionSec_.load(), item->durationSec});
    return Ok();
}

Result<void> MediaModule::Stop() {
    (void)CancelPlaybackTask();
    std::shared_ptr<MediaItem> item;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        item = current_;
    }
    positionSec_.store(0.0);
    playbackState_.store(kStateStopped);
    if (item)
        (void)EventBus::Instance().Publish(events::MediaStateChanged{
            item->id, item->title, kStateStopped, 0.0, item->durationSec});
    Logger::Instance().Info("Media stopped", "Media");
    return Ok();
}

std::string MediaModule::CurrentTitle() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return current_ ? current_->title : std::string();
}

std::string MediaModule::CurrentId() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return current_ ? current_->id : std::string();
}

Result<void> MediaModule::StartClock(double durationSec, const std::string& itemId) {
    if (durationSec <= 0.0) {
        FinishPlayback(itemId);
        return Ok();
    }
    auto total = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::duration<double>(durationSec));
    // Animation timer (07 §Animation): a 100 ms progress tick updates the
    // position; `done` fires when the playback budget is spent. Both callbacks
    // capture the item id and bail if a newer item started in the meantime —
    // otherwise a stale callback from item A could mark item B finished and
    // orphan B's clock.
    auto res = TaskScheduler::Instance().ScheduleAnimation(
        [this, durationSec, itemId](double p) {
            if (CurrentId() != itemId) return;   // a newer item started: drop stale ticks
            positionSec_.store(p * durationSec);
        },
        std::chrono::milliseconds(100), total,
        [this, itemId]() { FinishPlayback(itemId); });
    if (!res.ok()) {
        Logger::Instance().Error("Media: playback clock failed to start: " +
                                     res.error().message, "Media");
        return res.error();
    }
    std::lock_guard<std::mutex> lock(mutex_);
    playbackTask_ = res.value();
    return Ok();
}

void MediaModule::FinishPlayback(const std::string& itemId) {
    if (playbackState_.load() != kStatePlaying) return;   // paused/stopped meanwhile
    std::shared_ptr<MediaItem> item;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        item = current_;
        if (!item || item->id != itemId) return;   // a newer item is playing now
        playbackTask_ = 0;
    }
    positionSec_.store(item->durationSec);
    playbackState_.store(kStateFinished);
    (void)EventBus::Instance().Publish(events::MediaStateChanged{
        item->id, item->title, kStateFinished, item->durationSec, item->durationSec});
    Logger::Instance().Info("Media finished: " + item->title, "Media");
}

Result<void> MediaModule::CancelPlaybackTask() {
    TaskId task = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        task = playbackTask_;
        playbackTask_ = 0;
    }
    if (task != 0) (void)TaskScheduler::Instance().Cancel(task);
    return Ok();
}

} // namespace bps::modules
