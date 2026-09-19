#include "modules/songs/SongsModule.hpp"

#include "core/events/EventBus.hpp"
#include "core/events/Events.hpp"
#include "core/logging/Logger.hpp"

#include <algorithm>
#include <format>

namespace bps::modules {

std::shared_ptr<SongsModule> SongsModule::Create() {
    return std::shared_ptr<SongsModule>(new SongsModule());
}

const ModuleManifest& SongsModule::Manifest() const {
    static ModuleManifest m;
    m.id = "songs";
    m.version = kEngineVersion;
    m.author = "BPS Core";
    m.requiredCoreVersion = kEngineVersion;
    m.capabilities = {"songs", "lyrics"};
    return m;
}

Result<void> SongsModule::OnLoad() {
    Logger::Instance().Info("Songs module loaded", "Songs");
    return Ok();
}

Result<void> SongsModule::OnUnload() {
    std::lock_guard<std::mutex> lock(mutex_);
    currentSong_.reset();
    currentVerse_.store(0);
    return Ok();
}

Result<void> SongsModule::OnStart() {
    started_.store(true);
    Logger::Instance().Info("Songs module started", "Songs");
    return Ok();
}

Result<void> SongsModule::OnStop() {
    started_.store(false);
    Logger::Instance().Info("Songs module stopped", "Songs");
    return Ok();
}

Result<void> SongsModule::RegisterSong(Song song) {
    if (song.id.empty() || song.verses.empty())
        return Error::Make(Err::InvalidArgument, "SongsModule",
                           "a song needs an id and at least one verse");
    // Copy the key BEFORE the move: `library_[song.id] = make_shared(move(song))`
    // leaves the subscript's key evaluation unsequenced with the move, which can
    // store the song under a moved-from (empty) key.
    std::string key = song.id;
    std::lock_guard<std::mutex> lock(mutex_);
    if (library_.count(key) > 0)
        return Error::Make(Err::AlreadyExists, "SongsModule",
                           "song already registered: " + key);
    library_[std::move(key)] = std::make_shared<Song>(std::move(song));
    return Ok();
}

Result<void> SongsModule::RemoveSong(std::string_view id) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = library_.find(std::string(id));
    if (it == library_.end())
        return Error::Make(Err::NotFound, "SongsModule", "song not found: " + std::string(id));
    if (currentSong_ == it->second) currentSong_.reset();
    library_.erase(it);
    return Ok();
}

size_t SongsModule::SongCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return library_.size();
}

std::vector<Song> SongsModule::Library() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<Song> out;
    out.reserve(library_.size());
    for (const auto& [id, song] : library_) out.push_back(*song);
    return out;
}

Result<void> SongsModule::Select(std::string_view id) {
    std::shared_ptr<Song> song;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = library_.find(std::string(id));
        if (it == library_.end())
            return Error::Make(Err::NotFound, "SongsModule", "song not found: " + std::string(id));
        song = it->second;
        currentSong_ = song;
        currentVerse_.store(0);
    }
    (void)EventBus::Instance().Publish(events::SongSelected{song->id, song->title});
    return PublishVerse();
}

Result<void> SongsModule::NextVerse() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!currentSong_)
            return Error::Make(Err::InvalidState, "SongsModule", "no song selected");
        if (currentVerse_.load() + 1 >= static_cast<int>(currentSong_->verses.size()))
            return Error::Make(Err::InvalidState, "SongsModule", "already at the last verse");
        currentVerse_.fetch_add(1);
    }
    return PublishVerse();
}

Result<void> SongsModule::PreviousVerse() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!currentSong_)
            return Error::Make(Err::InvalidState, "SongsModule", "no song selected");
        if (currentVerse_.load() <= 0)
            return Error::Make(Err::InvalidState, "SongsModule", "already at the first verse");
        currentVerse_.fetch_sub(1);
    }
    return PublishVerse();
}

bool SongsModule::HasSelection() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return currentSong_ != nullptr;
}

std::string SongsModule::CurrentTitle() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return currentSong_ ? currentSong_->title : std::string();
}

Result<void> SongsModule::PublishVerse() {
    std::shared_ptr<Song> song;
    int verse = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        song = currentSong_;
        verse = currentVerse_.load();
    }
    if (!song)
        return Error::Make(Err::InvalidState, "SongsModule", "no song selected");
    if (verse < 0 || verse >= static_cast<int>(song->verses.size()))
        return Error::Make(Err::InvalidState, "SongsModule", "verse out of range");
    // Publish outside the lock: a subscriber may call back into the module.
    (void)EventBus::Instance().Publish(events::SongVerseChanged{
        song->id, song->title, verse, static_cast<int>(song->verses.size()),
        song->verses[static_cast<size_t>(verse)]});
    Logger::Instance().Info(std::format("Song verse: \"{}\" [{}/{}]", song->title,
                                        verse + 1, song->verses.size()),
                            "Songs");
    return Ok();
}

} // namespace bps::modules
