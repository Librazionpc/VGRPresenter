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

// --- Content blocks (docs/architecture/SystemArchitecture.md §3.12) --------------
// A slide is a set of positioned content blocks — the model a frontend's canvas
// edits (text boxes, images, clocks, timers...). Blocks carry LAYOUT and STYLE
// only; rendering stays with the Rendering Engine (SceneBuilder turns blocks
// into scene objects). Kind is an open string ("text", "image", "clock",
// "timer"...) so new block kinds need no engine change.
struct ContentStyle {
    double padding = 0;
    std::string backgroundColor = "transparent";   // "#aarrggbb" / "#rrggbb" / "transparent"
    double cornerRadius = 0;
    bool borderEnabled = false;
    double borderWidth = 2;
    std::string borderStyle = "line";
    std::string borderColor = "#ffffff";
};

struct ContentBlock {
    std::string id;                    // stable within the slide
    std::string kind = "text";
    std::string text;
    double x = 0;
    double y = 0;
    double width = 160;
    double height = 40;
    ContentStyle style;
    // Free-form per-kind configuration as a JSON object string (a clock's
    // format, a timer's duration...). Kept opaque here so the engine core
    // types don't depend on any one block kind's schema.
    std::string metaJson = "{}";
    // Template placeholders only: which slide field feeds this block when a
    // template is applied — "title", "text", "line1", "line2", "ref", "notes"
    // ("" = static content, shown as-is).
    std::string bind;
};

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
    std::vector<ContentBlock> blocks;  // positioned content (see ContentBlock)
    std::string background = "transparent";   // slide background colour
    // Frontend-specific slide fields as a JSON object string (a slide list's
    // label colour, preview lines, scripture reference...). Opaque to the engine.
    std::string metaJson = "{}";
    // Which category of the show this slide belongs to ("" = uncategorised).
    // Its category's template decides how the slide looks.
    std::string categoryId;
};

// --- Show structure: categories, templates, overlays ------------------------------
// A show groups its slides into CATEGORIES (songs, notes, pastor notes,
// scripture...). Each category is assigned a TEMPLATE, so every slide in it
// looks the same and restyling the template restyles them all — content never
// owns styling (docs/specs/23 §Themes). Slide ORDER stays one flat sequence, so a
// show can still interleave song / notes / song; categories are a grouping and
// styling axis, not containers.
struct Category {
    std::string id;
    std::string name;                  // "Songs", "Pastor Notes"...
    std::string contentType;           // "song" | "notes" | "pastor-notes" | "scripture" | custom
    std::string templateId;            // the SlideTemplate its slides use ("" = none)
    // Outputs this category is shown on ("audience", "stage"...). Empty = every
    // output. (Pastor notes are typically stage-only — that is a per-show choice.)
    std::vector<std::string> outputs;
    std::string metaJson = "{}";       // frontend extras (accent colour, icon...)
};

// A reusable slide layout: content blocks whose `bind` names the slide field they
// display. Stored inside the show (so it opens anywhere) and as standalone .vgr
// Template documents (PresentationTemplates.hpp).
struct SlideTemplate {
    std::string id;
    std::string name;
    std::string contentType;           // the kind of category it is meant for
    std::string background = "transparent";
    std::vector<ContentBlock> blocks;
    std::string metaJson = "{}";
};

// What an overlay applies to.
enum class OverlayScope : int {
    All = 0,        // every slide
    Category,       // slides of one category (targetId = category id)
    Slide,          // one slide (targetId = slide id)
};

// A layer drawn OVER slides — lower third, logo, clock, watermark. Lives on the
// show, not on slides, so one overlay covers a whole scope.
struct Overlay {
    std::string id;
    std::string name;
    OverlayScope scope = OverlayScope::All;
    std::string targetId;              // category/slide id for the narrower scopes
    std::vector<std::string> outputs;  // outputs it renders on; empty = every output
    bool enabled = true;
    std::vector<ContentBlock> blocks;
    std::string metaJson = "{}";
};

// --- Presentation (a document: show / song / notes / countdown) ---------------
struct Presentation {
    std::string id;                    // stable uuid
    std::string name;
    std::string path;                  // source path (imported files)
    std::vector<Slide> slides;
    PlaybackMode mode = PlaybackMode::Manual;
    double defaultTransitionMs = 500.0;
    std::vector<Category> categories;        // ordered; slides reference by id
    std::vector<SlideTemplate> templates;    // templates used by this show
    std::vector<Overlay> overlays;           // show-wide layers over slides
    // About the show itself, as a JSON object string: a song's title / author / CCLI / copyright / key (what an import found),
    // notes. Opaque to the engine core, kept in the file so it travels with the show.
    std::string metaJson = "{}";
    PresentationState state = PresentationState::Created;
    std::chrono::system_clock::time_point createdAt;
    std::chrono::system_clock::time_point modifiedAt;
};

// --- Output style (Settings · Styles applied to the on-air output) -------------
// FreeShow's Styles (src/types/Settings.ts): a named "theme" an OUTPUT renders
// with — a background colour and a forced text layout that override whatever the
// slide alone would do. The UI owns the roster (StyleListModel, persisted
// through the kernel's StyleStore); the engine only ever sees the ONE spec for
// the output that is on air, so live-editing a style restyles every frame the
// moment the UI pushes it (PresentationEngine::SetActiveOutputStyle).
struct OutputStyleSpec {
    std::string name;                        // diagnostics only
    // "shows" | "media" | "scripture" | "table" — which content the style is
    // meant for (informational; layout follows templateKey).
    std::string contentType;
    // Layout preset key — StyleBuilder::LayoutFor knows these ("lowerThird",
    // "title", "sidebar", "bottomBar", "fullscreen", or an engine design id;
    // unknown keys fall back to the builder's default full layout).
    std::string templateKey;
    // "#aarrggbb" / "#rrggbb" / "transparent" — the on-air background. Empty
    // string = the spec carries no background (leave the slide's own).
    std::string backgroundColor;
    // FreeShow's clearStyleBackgroundOnText: when the slide itself carries a
    // background colour, the style's background steps aside for it.
    bool clearBackgroundOnText = false;
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
