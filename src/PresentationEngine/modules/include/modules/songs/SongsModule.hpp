#pragma once

#include "core/common/Common.hpp"
#include "interfaces/IModule.hpp"

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace bps::modules {

struct Song {
    std::string id;
    std::string title;
    std::string author;
    std::vector<std::string> verses;   // 0-based lyric blocks
};

// Songs feature module (docs/specs/08): a lyric library with verse navigation.
// Publishes `songs.selected` and `songs.verse_changed`; lyric output systems
// (display, remote, lower-third) subscribe independently — the core has no
// knowledge of songs.
class SongsModule final : public IModule {
public:
    static std::shared_ptr<SongsModule> Create();

    const ModuleManifest& Manifest() const override;

    Result<void> OnLoad() override;
    Result<void> OnUnload() override;
    Result<void> OnStart() override;
    Result<void> OnStop() override;

    // Library management.
    Result<void> RegisterSong(Song song);
    Result<void> RemoveSong(std::string_view id);
    size_t SongCount() const;
    std::vector<Song> Library() const;

    // Verse navigation (publishes songs.selected / songs.verse_changed).
    Result<void> Select(std::string_view id);
    Result<void> NextVerse();
    Result<void> PreviousVerse();
    bool HasSelection() const;
    int CurrentVerse() const noexcept { return currentVerse_.load(); }
    std::string CurrentTitle() const;

private:
    SongsModule() = default;
    Result<void> PublishVerse();   // publishes the current verse

    mutable std::mutex mutex_;
    std::unordered_map<std::string, std::shared_ptr<Song>> library_;  // guarded by mutex_
    std::shared_ptr<Song> currentSong_;                               // guarded by mutex_
    std::atomic<int> currentVerse_{0};
    std::atomic<bool> started_{false};
};

} // namespace bps::modules
