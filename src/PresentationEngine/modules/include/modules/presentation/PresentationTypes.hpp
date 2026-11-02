#pragma once

#include "core/common/Common.hpp"
#include "modules/rendering/Effects.hpp"
#include "modules/rendering/RenderTypes.hpp"

#include <chrono>
#include <cstddef>
#include <string>
#include <map>
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

struct ContentStyle {
    double padding = 0;
    std::string backgroundColor = "transparent";   // "#aarrggbb" / "#rrggbb" / "transparent"
    double cornerRadius = 0;
    bool borderEnabled = false;
    double borderWidth = 2;
    std::string borderStyle = "line";
    std::string borderColor = "#ffffff";

    bool operator==(const ContentStyle&) const = default;
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
    std::string metaJson = "{}";
    std::string bind;

    bool operator==(const ContentBlock&) const = default;
};

// --- Slide (docs/specs/19 §Slide system) --------------------------------------
// Slides have STABLE IDs (UUIDs), never array indexes.
struct Slide {
    std::string id;                    // stable uuid
    std::string title;
    std::string text;                  // primary content (lyrics / verse / notes)
    // ---- THE READING FLOW (the Alt+Down / Alt+Up teleprompter step) ---------
    std::string flowText;
    float flowChar = -1.0f;
    // The line breaks of `flowText` for each output style (keyed by OutputStyleSpec::readingStyleKey),
    // cut once at the stage's own size by the service. They travel WITH the text they were cut from,
    // so a scene never draws one passage with another's lines. Absent = the output wraps by itself.
    std::map<std::string, std::vector<int>> flowBreaks;
    // ...and the font size (stage px) each output style fits THIS window to, so the page fills its text
    // area (see LiveOutputService::readingWindowSizeFor). Absent = the output's solved column size.
    std::map<std::string, float> flowSizes;
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
    rendering::SlideFilterValues filters;
    // Frontend-specific slide fields as a JSON object string (a slide list's
    // label colour, preview lines, scripture reference...). Opaque to the engine.
    std::string metaJson = "{}";
    // Which category of the show this slide belongs to ("" = uncategorised).
    // Its category's template decides how the slide looks.
    std::string categoryId;
    bool clearStage = false;
};

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

inline constexpr std::size_t kFamilySlotShows = 0;
inline constexpr std::size_t kFamilySlotMedia = 1;
inline constexpr std::size_t kFamilySlotScripture = 2;
inline constexpr std::size_t kFamilySlotTable = 3;
inline constexpr std::size_t kFamilySlotCount = 4;

struct OutputStyleSpec {
    std::string name;                        // diagnostics only
    // "shows" | "media" | "scripture" | "table" — which content the style is
    // meant for (informational; layout follows templateKey).
    std::string contentType;
    std::string templateKey;
    // "#aarrggbb" / "#rrggbb" / "transparent" — the on-air background. Empty
    // string = the spec carries no background (leave the slide's own).
    std::string backgroundColor;
    std::string backgroundImage;
    std::string mediaFit;
    float readingFontPx = 0.0f;
    // Which key this output's line breaks carry in Slide::flowBreaks (the output style's id).
    std::string readingStyleKey;
    int videoBus = 0;
    bool gateVideoBuses = false;
    std::vector<int> allowedVideoBuses;   // bus NUMBERS (CompositorState's key)
    int templateBus = 0;
    std::vector<ContentBlock> templateBusBlocks;
    // The template design's background ("#rrggbb" / ""), painted under the
    // windows exactly like a style's colour is painted under a slide.
    std::string templateBusBackground;
    // FreeShow's clearStyleBackgroundOnText: when the slide itself carries a
    // background colour, the style's background steps aside for it.
    bool clearBackgroundOnText = false;
    bool showShows = true;
    bool showMedia = true;
    bool showScripture = true;
    bool showTable = true;
    bool showOverlay = true;
    bool FamilyGate(std::size_t slot) const {
        if (slot == kFamilySlotMedia)     return showMedia;
        if (slot == kFamilySlotScripture) return showScripture;
        if (slot == kFamilySlotTable)     return showTable;
        return showShows;
    }
    void SetFamilyGate(std::size_t slot, bool on) {
        if (slot == kFamilySlotMedia)          showMedia = on;
        else if (slot == kFamilySlotScripture) showScripture = on;
        else if (slot == kFamilySlotTable)     showTable = on;
        else                                   showShows = on;
    }
    std::string familyTemplateKeys[kFamilySlotCount];
    std::vector<ContentBlock> familyTemplateBlocks[kFamilySlotCount];
    struct ShowCategoryTemplate {
        std::string category;
        std::string templateKey;
        bool overrideEnabled = false;
        std::vector<ContentBlock> blocks;
    };
    std::vector<ShowCategoryTemplate> showCategoryTemplates;
    // Shows-only category label (diagnostic display for now).
    std::string category;
    std::vector<ContentBlock> templateBlocks;
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