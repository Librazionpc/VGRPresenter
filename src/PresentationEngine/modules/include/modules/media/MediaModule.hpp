#pragma once

#include "core/common/Common.hpp"
#include "core/task_scheduler/TaskScheduler.hpp"
#include "interfaces/IModule.hpp"

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace bps::modules {

enum class MediaPlaybackState : int { Stopped = 0, Playing, Paused, Finished };
enum class MediaType : int { Unknown = 0, Video, Audio, Image };

struct MediaItem {
    std::string id;
    std::string title;
    MediaType type = MediaType::Unknown;
    double durationSec = 0.0;
};

// Media feature module (docs/specs/08): playlist playback with a timed
// position clock driven by the TaskScheduler's animation timer (07 §Animation).
// Publishes `media.state_changed` on play/pause/resume/stop/finish; the core
// has no knowledge of media formats.
class MediaModule final : public IModule {
public:
    static std::shared_ptr<MediaModule> Create();

    const ModuleManifest& Manifest() const override;

    Result<void> OnLoad() override;
    Result<void> OnUnload() override;
    Result<void> OnStart() override;
    Result<void> OnStop() override;

    // Playlist management.
    Result<void> Enqueue(MediaItem item);
    Result<void> Remove(std::string_view id);
    size_t PlaylistSize() const;
    std::vector<MediaItem> Playlist() const;

    // Playback (publishes media.state_changed).
    Result<void> Play(std::string_view id);
    Result<void> Pause();
    Result<void> Resume();
    Result<void> Stop();
    MediaPlaybackState PlaybackState() const noexcept {
        return static_cast<MediaPlaybackState>(playbackState_.load());
    }
    double PositionSec() const noexcept { return positionSec_.load(); }
    std::string CurrentTitle() const;
    std::string CurrentId() const;

private:
    MediaModule() = default;

    Result<void> StartClock(double durationSec, const std::string& itemId);
    void FinishPlayback(const std::string& itemId);   // playback budget spent
    Result<void> CancelPlaybackTask();

    mutable std::mutex mutex_;
    std::unordered_map<std::string, std::shared_ptr<MediaItem>> playlist_;  // guarded by mutex_
    std::shared_ptr<MediaItem> current_;                                    // guarded by mutex_
    std::atomic<int> playbackState_{static_cast<int>(MediaPlaybackState::Stopped)};
    std::atomic<double> positionSec_{0.0};
    TaskId playbackTask_ = 0;   // guarded by mutex_ (owned by TaskScheduler)
    std::atomic<bool> started_{false};
};

} // namespace bps::modules
