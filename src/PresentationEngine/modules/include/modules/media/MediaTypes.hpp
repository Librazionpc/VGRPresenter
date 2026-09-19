#pragma once

// Media Engine (Phase 10, docs/specs/21). The single system for managing every
// type of media. Only understands Media Assets — never Songs/Bibles/
// Presentations/Slides/Themes.

#include "core/common/Common.hpp"
#include "core/config/Json.hpp"
#include "modules/rendering/RenderTypes.hpp"

#include <chrono>
#include <map>
#include <string>
#include <vector>

namespace bps::media {

// --- Media types ---------------------------------------------------------------
enum class MediaType : int {
    Image = 0,
    Video,
    Audio,
    AnimatedImage,
    Svg,
    Vector,
    Icon,
    Font,
    Background,
    Unknown,
};

inline const char* ToString(MediaType t) {
    switch (t) {
        case MediaType::Image:         return "Image";
        case MediaType::Video:         return "Video";
        case MediaType::Audio:         return "Audio";
        case MediaType::AnimatedImage: return "AnimatedImage";
        case MediaType::Svg:           return "Svg";
        case MediaType::Vector:        return "Vector";
        case MediaType::Icon:          return "Icon";
        case MediaType::Font:          return "Font";
        case MediaType::Background:    return "Background";
        case MediaType::Unknown:       return "Unknown";
    }
    return "Unknown";
}

// --- Supported formats (docs/specs/21 §Supported formats) ----------------------
struct MediaFormat {
    std::string extension;     // "png", "mp4", "mp3"...
    MediaType type = MediaType::Unknown;
    bool supported = false;    // codec/decoder available
};

// --- Metadata (docs/specs/21 §Metadata extraction) ------------------------------
struct MediaMetadata {
    std::string name;
    uint64_t sizeBytes = 0;
    int width = 0;
    int height = 0;
    double durationSec = 0.0;
    std::string codec;
    uint64_t bitrate = 0;
    double frameRate = 0.0;
    int audioChannels = 0;
    int orientation = 0;
    std::string colorSpace;
    int64_t createdMs = 0;
    int64_t modifiedMs = 0;
    std::vector<std::string> tags;
    std::string author;
    std::string copyright;
    std::map<std::string, std::string, std::less<>> extra;   // extensible
};

// --- A managed media asset ------------------------------------------------------
struct MediaAsset {
    std::string id;                 // uuid
    std::string name;
    std::string path;
    MediaType type = MediaType::Unknown;
    std::string extension;
    MediaMetadata metadata;
    bool hasThumbnail = false;
    bool hasPreview = false;
    bool hasWaveform = false;
    std::vector<uint8_t> thumbnailPixels;   // RGBA8 (generated)
    int thumbnailW = 0;
    int thumbnailH = 0;
    std::chrono::system_clock::time_point importedAt;
};

// --- Playback state (docs/specs/21 §Playback) -----------------------------------
enum class PlaybackState : int {
    Idle = 0,
    Playing,
    Paused,
    Buffering,
    Stopped,
    Failed,
};

inline const char* ToString(PlaybackState s) {
    switch (s) {
        case PlaybackState::Idle:      return "Idle";
        case PlaybackState::Playing:   return "Playing";
        case PlaybackState::Paused:    return "Paused";
        case PlaybackState::Buffering: return "Buffering";
        case PlaybackState::Stopped:   return "Stopped";
        case PlaybackState::Failed:    return "Failed";
    }
    return "Unknown";
}

// --- Hardware acceleration intent (from the Adaptive Runtime) -------------------
enum class DecodePath : int {
    Hardware = 0,
    Software,
    Auto,
};

inline const char* ToString(DecodePath p) {
    switch (p) {
        case DecodePath::Hardware: return "Hardware";
        case DecodePath::Software: return "Software";
        case DecodePath::Auto:     return "Auto";
    }
    return "Unknown";
}

} // namespace bps::media
