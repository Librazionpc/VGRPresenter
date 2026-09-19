#pragma once

// Presentation Engine (Phase 8, docs/specs/19). The conductor of the orchestra:
// orchestrates shows, slides, timelines, transitions, cues, playback and
// interaction. It never renders pixels (Phase 6) and never routes outputs
// (Phase 7). Contains presentation logic only — no WinUI/XAML/HWND.

#include "core/common/Common.hpp"
#include "modules/rendering/RenderTypes.hpp"

#include <chrono>
#include <string>
#include <vector>

namespace bps::presentation {

// --- State machine (docs/specs/19 §State machine) ----------------------------
enum class PresentationState : int {
    Created = 0,
    Loaded,
    Validated,
    Compiled,
    Prepared,
    Ready,
    Live,
    Paused,
    Stopped,
    Finished,
    Recovering,
    Closed,
};

inline const char* ToString(PresentationState s) {
    switch (s) {
        case PresentationState::Created:    return "Created";
        case PresentationState::Loaded:     return "Loaded";
        case PresentationState::Validated:  return "Validated";
        case PresentationState::Compiled:   return "Compiled";
        case PresentationState::Prepared:   return "Prepared";
        case PresentationState::Ready:      return "Ready";
        case PresentationState::Live:       return "Live";
        case PresentationState::Paused:     return "Paused";
        case PresentationState::Stopped:    return "Stopped";
        case PresentationState::Finished:   return "Finished";
        case PresentationState::Recovering: return "Recovering";
        case PresentationState::Closed:     return "Closed";
    }
    return "Unknown";
}

// --- Playback modes (docs/specs/19 §Playback modes) ---------------------------
enum class PlaybackMode : int {
    Manual = 0,
    Automatic,
    Timed,
    Loop,
    Repeat,
    Playlist,
};

inline const char* ToString(PlaybackMode m) {
    switch (m) {
        case PlaybackMode::Manual:    return "Manual";
        case PlaybackMode::Automatic: return "Automatic";
        case PlaybackMode::Timed:     return "Timed";
        case PlaybackMode::Loop:      return "Loop";
        case PlaybackMode::Repeat:    return "Repeat";
        case PlaybackMode::Playlist:  return "Playlist";
    }
    return "Unknown";
}

// --- Transitions (evaluated by the Phase 6 TransitionEngine) ------------------
enum class TransitionKind : int {
    Fade = 0,
    Slide,
    Push,
    Zoom,
    Wipe,
    Crossfade,
    Custom,
};

inline const char* ToString(TransitionKind t) {
    switch (t) {
        case TransitionKind::Fade:      return "Fade";
        case TransitionKind::Slide:     return "Slide";
        case TransitionKind::Push:      return "Push";
        case TransitionKind::Zoom:      return "Zoom";
        case TransitionKind::Wipe:      return "Wipe";
        case TransitionKind::Crossfade: return "Crossfade";
        case TransitionKind::Custom:    return "Custom";
    }
    return "Unknown";
}

// --- Cue kinds (docs/specs/19 §Cue system) ------------------------------------
enum class CueKind : int {
    Slide = 0,
    Media,
    Audio,
    Timer,
    Countdown,
    Script,
    Plugin,
};

inline const char* ToString(CueKind k) {
    switch (k) {
        case CueKind::Slide:     return "Slide";
        case CueKind::Media:     return "Media";
        case CueKind::Audio:     return "Audio";
        case CueKind::Timer:     return "Timer";
        case CueKind::Countdown: return "Countdown";
        case CueKind::Script:    return "Script";
        case CueKind::Plugin:    return "Plugin";
    }
    return "Unknown";
}

// --- Slide (docs/specs/19 §Slide system) --------------------------------------
// Slides have STABLE IDs (UUIDs), never array indexes.
struct Slide {
    std::string id;                    // stable uuid
    std::string title;
    std::string text;                  // primary content (lyrics / verse / notes)
    std::string notes;
    std::vector<std::string> tags;
    std::vector<std::string> sections;
    std::vector<std::string> assetIds; // referenced media/font assets
    bool hidden = false;
    TransitionKind transitionIn = TransitionKind::Fade;
    double transitionMs = 500.0;
    double durationMs = 0.0;           // auto-advance in Timed/Automatic modes (0 = manual)
};

// --- Presentation (a document: show / song / notes / countdown) ---------------
struct Presentation {
    std::string id;                    // stable uuid
    std::string name;
    std::string path;                  // source path (imported files)
    std::vector<Slide> slides;
    PlaybackMode mode = PlaybackMode::Manual;
    double defaultTransitionMs = 500.0;
    PresentationState state = PresentationState::Created;
    std::chrono::system_clock::time_point createdAt;
    std::chrono::system_clock::time_point modifiedAt;
};

// --- Validation issues (docs/specs/19 §Validator) -----------------------------
struct ValidationIssue {
    std::string slideId;
    std::string message;
    int severity = 0;   // 0 = warning, 1 = error
};

// --- Session snapshot (docs/specs/19 §Recovery) -------------------------------
struct SessionSnapshot {
    std::string presentationId;
    std::string presentationName;
    int slideIndex = 0;
    PresentationState state = PresentationState::Ready;
    double timelineSec = 0.0;
    PlaybackMode mode = PlaybackMode::Manual;
    std::chrono::system_clock::time_point savedAt;
};

} // namespace bps::presentation
