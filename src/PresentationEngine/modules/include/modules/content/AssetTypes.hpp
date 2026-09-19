#pragma once

// Asset taxonomy + lifecycle (docs/specs/13 §Asset Types / §Asset States).

#include <string>
#include <string_view>

namespace bps::content {

enum class AssetType : int {
    Unknown = 0,
    Presentation, Slide, Song, Bible,
    Image, Video, Audio, Font, Theme, Template,
    Transition, Animation, Plugin, Workspace, Profile, Project, Package,
    Background, Overlay, Countdown, Logo, Subtitle, CameraPreset,
    Text, Json, Data,
    Count
};

enum class AssetState : int {
    Unknown = 0, Discovered, Indexed, Imported, Validated,
    Cached, Loaded, Referenced, Modified, Saved, Exported,
    Archived, Deleted
};

inline const char* ToString(AssetType t) {
    switch (t) {
        case AssetType::Presentation:  return "Presentation";
        case AssetType::Slide:         return "Slide";
        case AssetType::Song:          return "Song";
        case AssetType::Bible:         return "Bible";
        case AssetType::Image:         return "Image";
        case AssetType::Video:         return "Video";
        case AssetType::Audio:         return "Audio";
        case AssetType::Font:          return "Font";
        case AssetType::Theme:         return "Theme";
        case AssetType::Template:      return "Template";
        case AssetType::Transition:    return "Transition";
        case AssetType::Animation:     return "Animation";
        case AssetType::Plugin:        return "Plugin";
        case AssetType::Workspace:     return "Workspace";
        case AssetType::Profile:       return "Profile";
        case AssetType::Project:       return "Project";
        case AssetType::Package:       return "Package";
        case AssetType::Background:    return "Background";
        case AssetType::Overlay:       return "Overlay";
        case AssetType::Countdown:     return "Countdown";
        case AssetType::Logo:          return "Logo";
        case AssetType::Subtitle:      return "Subtitle";
        case AssetType::CameraPreset:  return "CameraPreset";
        case AssetType::Text:          return "Text";
        case AssetType::Json:          return "Json";
        case AssetType::Data:          return "Data";
        case AssetType::Unknown:
        default:                       return "Unknown";
    }
}

inline AssetType AssetTypeFromString(std::string_view s) {
    for (int i = 1; i < static_cast<int>(AssetType::Count); ++i) {
        if (s == ToString(static_cast<AssetType>(i)))
            return static_cast<AssetType>(i);
    }
    return AssetType::Unknown;
}

// Map a file extension to a content type (docs/specs/13 §Asset Types).
inline AssetType TypeForExtension(std::string_view extLower) {
    if (extLower == "png" || extLower == "jpg" || extLower == "jpeg" ||
        extLower == "gif" || extLower == "svg" || extLower == "webp" ||
        extLower == "bmp")
        return AssetType::Image;
    if (extLower == "mp4" || extLower == "mkv" || extLower == "avi" ||
        extLower == "mov" || extLower == "webm")
        return AssetType::Video;
    if (extLower == "mp3" || extLower == "wav" || extLower == "flac" ||
        extLower == "ogg" || extLower == "m4a")
        return AssetType::Audio;
    if (extLower == "ttf" || extLower == "otf" || extLower == "woff" ||
        extLower == "woff2")
        return AssetType::Font;
    if (extLower == "json") return AssetType::Json;
    if (extLower == "txt" || extLower == "md" || extLower == "csv" ||
        extLower == "xml" || extLower == "yaml" || extLower == "yml")
        return AssetType::Text;
    if (extLower == "song" || extLower == "chordpro" || extLower == "cho")
        return AssetType::Song;
    if (extLower == "ppt" || extLower == "pptx" || extLower == "odp" ||
        extLower == "key")
        return AssetType::Presentation;
    if (extLower == "pdf") return AssetType::Template;
    if (extLower == "zip" || extLower == "bpkg") return AssetType::Package;
    return AssetType::Unknown;
}

inline const char* ToString(AssetState s) {
    switch (s) {
        case AssetState::Unknown:    return "Unknown";
        case AssetState::Discovered: return "Discovered";
        case AssetState::Indexed:    return "Indexed";
        case AssetState::Imported:   return "Imported";
        case AssetState::Validated:  return "Validated";
        case AssetState::Cached:     return "Cached";
        case AssetState::Loaded:     return "Loaded";
        case AssetState::Referenced: return "Referenced";
        case AssetState::Modified:   return "Modified";
        case AssetState::Saved:      return "Saved";
        case AssetState::Exported:   return "Exported";
        case AssetState::Archived:   return "Archived";
        case AssetState::Deleted:    return "Deleted";
    }
    return "Unknown";
}

} // namespace bps::content
