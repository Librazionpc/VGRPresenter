#pragma once

// Presentation Engine (Phase 8, docs/specs/19). The conductor of the orchestra:
// orchestrates shows, slides, timelines, transitions, cues, playback and
// interaction. It never renders pixels (Phase 6) and never routes outputs
// (Phase 7). Contains presentation logic only — no WinUI/XAML/HWND.

#include "core/common/Common.hpp"
#include "modules/rendering/Effects.hpp"
#include "modules/rendering/RenderTypes.hpp"

#include <chrono>
#include <cstddef>
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

    // Defaulted so a pushed OutputStyleSpec's templateBlocks can be compared
    // against the stored spec (a block-only template edit must read as a
    // change, not a no-op push).
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
    // Free-form per-kind configuration as a JSON object string (a clock's
    // format, a timer's duration...). Kept opaque here so the engine core
    // types don't depend on any one block kind's schema.
    std::string metaJson = "{}";
    // Template placeholders only: which slide field feeds this block when a
    // template is applied — "title", "text", "line1", "line2", "ref", "notes"
    // ("" = static content, shown as-is).
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
    //
    // `text` above is a PAGE of a passage; these two describe the passage it was
    // cut from, so the outputs can scroll it instead of replacing it:
    //   flowText    the continuous run of words this page begins in (this page
    //               and the one after it, joined by the page break). Empty = an
    //               ordinary slide, which renders exactly as it always has.
    //   flowChar    WHERE THE READER IS IN THAT PASSAGE, as a CHARACTER OFFSET into
    //               flowText (0 = its first character at the top of the box).
    //               Character, not line, because every output draws this passage
    //               with ITS OWN style — its own type size, its own wrap — so
    //               "line 7" is a different WORD in each of them. Measured on the
    //               outputs' own hardware: two outputs styled at different sizes
    //               stood a line or two apart, showing different sentences at the
    //               same instant ("the left is still showing while the right has
    //               finished"). A character offset is the one position they can all
    //               agree on: each converts it into its own line grid (see
    //               TextObject::FlowLineForChar), so the same words are at the top
    //               of every box. -1 = no flow position (an ordinary slide).
    //               It belongs HERE and not only in the frame's render options
    //               because it has to survive a rebuild: a step that lands on the
    //               next page commits a NEW slide, and the scene for it is built
    //               from this number — one atomic fact, so the boundary cannot
    //               show the new page at the old offset for a frame.
    //
    // Why on the slide and not inside `text`: a step must show the line arriving
    // from below the page, and only the passage knows it. They travel as their
    // own fields (ShowConverter), never inside metaJson, because a passage is
    // full of the very characters a JSON meta value has to escape.
    std::string flowText;
    float flowChar = -1.0f;
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
    // Slide-WIDE filters (FreeShow's edit-panel filter set; see
    // rendering::SlideFilterValues). Applied by the live path to the composed
    // frame for this slide, not per block, so every output — and the saved
    // show — carries one set of numbers per slide. Identity by default, which
    // is also what lets the live path skip the pass entirely.
    rendering::SlideFilterValues filters;
    // Frontend-specific slide fields as a JSON object string (a slide list's
    // label colour, preview lines, scripture reference...). Opaque to the engine.
    std::string metaJson = "{}";
    // Which category of the show this slide belongs to ("" = uncategorised).
    // Its category's template decides how the slide looks.
    std::string categoryId;
    // A CLEAR CARRIER — the slide the wall's Clear all (and the per-layer slide
    // clear) sends. It carries no content and asks every output to paint
    // NOTHING: not the style's colour, not its background image. Without this a
    // clear fell back to the style's own look, so the screens went from the
    // slide straight to the style's poster — reported as "clear all doesn't
    // clear: it brings the background image back and clears the slide".
    //
    // This is NOT the GO-LIVE-with-nothing carrier (LiveOutputService's
    // goLiveBlank): that one deliberately keeps the style painting, because
    // going live with nothing staged is meant to show the output's own look.
    bool clearStage = false;
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
// THE FAMILY → SLOT INDICES, NAMED. The order is the show* gates' order (shows |
// media | scripture | table) and these constants are the ONLY place it is
// written: the arrays below are sized by the count, and every index that was a
// bare 0/1/2/3 (the QML style model's load/save/toVariant, the output model's
// "familyTemplate" + capitalised-tag key) reads one of these or asks
// SlideRules::FamilyTemplateSlot, which is the one place a page's content TAG
// becomes a slot.
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
    // Layout preset key — StyleBuilder::LayoutFor knows these ("lowerThird",
    // "title", "sidebar", "bottomBar", "fullscreen", or an engine design id;
    // unknown keys fall back to the builder's default full layout).
    std::string templateKey;
    // "#aarrggbb" / "#rrggbb" / "transparent" — the on-air background. Empty
    // string = the spec carries no background (leave the slide's own).
    std::string backgroundColor;
    // The style's background IMAGE (absolute file path, png/jpg/webp/...) —
    // painted cover-fit over the colour, under all content (FreeShow's
    // styles[...].backgroundImage). Empty = no image.
    std::string backgroundImage;
    // LEGACY — NO LONGER COMPOSED FROM. How a taken media file sits on an output
    // is the FILE's own fit (Media right-click → Fill, riding the frame as
    // CompositorState::MediaFrame::fit), else contain: one picture, one shape on
    // every screen. This per-output value was the fallback, and two outputs left
    // on different values showed one take as two pictures on the monitor wall.
    // Kept so the roster round-trips; see SceneBuilder::AddCompositorLayers.
    std::string mediaFit;
    // THE WIRED VIDEO BUS (Settings · Routing) — this output's own picture.
    //
    // The bus's STABLE number as the routing canvas's video wire names it
    // (CompositorState keys its per-bus frames the same way), 0 = nothing
    // wired. Until this field existed the wires were stored and validated and
    // then went nowhere at render time: `AssignOutputBuses` wrote an
    // OutputConfig no composer ever read, so every screen, every NDI sender and
    // every recording showed the program feed no matter what the canvas said.
    // A wired output now composes THAT bus's live frame full-frame
    // (SceneBuilder::BuildBusFeedScene) instead of the on-air slide, and 0 keeps
    // exactly the behaviour every existing setup has. It rides the spec rather
    // than a separate push because the spec is already the per-output channel
    // the live loop reads every frame.
    int videoBus = 0;
    // WHICH BUS WINDOWS THIS OUTPUT MAY PAINT. A design block of kind "bus" is a
    // live feed drawn inside the design's own rectangle (SceneBuilder's bus
    // planes), and it used to paint on EVERY output that composed that slide — an
    // NDI or HDMI row with no wire to that bus included, because the block lives
    // in the shared program composition. THE WIRES ARE THE ALLOW-LIST: when
    // `gateVideoBuses` is set, a bus block paints only if its bus number is in
    // `allowedVideoBuses` — the row's own wired video set, read from the routing
    // edges. An unwired row carries no picture, so it paints none: "it is not
    // wired, it must not come active there", on screen.
    //
    // OFF by default, and deliberately: the program/preview paths and every caller
    // that does not know a roster row (the app's own preview tiles, the unit
    // tests) keep painting bus windows exactly as before. A gated build is also a
    // different scene id (StyledSceneIdFor), so the shared program scene can never
    // be served to a gated output as a cache hit.
    bool gateVideoBuses = false;
    std::vector<int> allowedVideoBuses;   // bus NUMBERS (CompositorState's key)
    // THE TEMPLATE BUS THIS OUTPUT SHOWS (Settings · Routing), 0 = none.
    //
    // A bus created from a template is a DESIGN with windows, so an output wired
    // to one composes that design — the template's own blocks with each window
    // filled — instead of a single full-frame picture
    // (SceneBuilder::BuildTemplateBusScene). It is a field of its own rather than
    // a meaning of `videoBus` because the two are different pictures: `videoBus`
    // names ONE feed, this names a layout with several.
    int templateBus = 0;
    // The template's blocks, baked by the UI on every push (the same transient
    // bake a style's template gets, StyleSpecBuilder): the engine never reads the
    // template library itself, and a template edit re-pushes through the same
    // path. Empty = the template is gone, and the output falls back to the
    // program picture rather than rendering a blank design.
    std::vector<ContentBlock> templateBusBlocks;
    // The template design's background ("#rrggbb" / ""), painted under the
    // windows exactly like a style's colour is painted under a slide.
    std::string templateBusBackground;
    // FreeShow's clearStyleBackgroundOnText: when the slide itself carries a
    // background colour, the style's background steps aside for it.
    bool clearBackgroundOnText = false;
    // Per-content-type gates (the Edit dialog's four pills): a tab toggled OFF
    // greys out — go-live REFUSES that tab's content for this style. Fingerprint
    // fields below ride the spec so any pill flip re-keys the scenes.
    bool showShows = true;
    bool showMedia = true;
    bool showScripture = true;
    bool showTable = true;
    // A fifth pill, same on/off shape as the four above but gating a
    // DIFFERENT axis: not a slide content family (overlays aren't slides),
    // the Overlays pane's own compositor layer (SceneBuilder's
    // AddCompositorLayers) — an output wearing a style with this off shows
    // no overlay, even while the same overlay is live for every other
    // output. No per-family template: an overlay always renders from its
    // own design's blocks, never a style template.
    bool showOverlay = true;
    // PER-FAMILY TEMPLATE KEYS ("" = no template for that family — NO
    // inherit; the family renders plain, and SHOWS alone falls back to the
    // Bible template the UI bakes into its slot): one style renders Shows,
    // Media, Scripture and The Table EACH through its own template —
    // SceneBuilder picks by the slide's content family (untagged slides are
    // shows content). Index order = shows | media | scripture | table (the
    // show* gates' order).
    // The four family gates by SLOT: what lets a caller that already holds the
    // family table set them by walking it, instead of naming each field (the QML
    // style layer builds a spec from a style's variant map key by key).
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
    // BAKED BLOCKS per family (same transient bake as templateBlocks: the UI
    // copies the family design's blocks in at push time; empty vector = the
    // family has no template — the legacy LayoutFor layout, or templateBlocks
    // for a slide matching the style's own contentType).
    std::vector<ContentBlock> familyTemplateBlocks[kFamilySlotCount];
    // Shows-only category label (diagnostic display for now).
    std::string category;
    // BAKED TEMPLATE LAYOUT (Settings · Styles ← the Template library): when
    // templateKey names an ENGINE TEMPLATE DESIGN ("tpl-…", the picker's
    // catalog) the UI copies that design's blocks here at push time. The
    // engine never reaches back into the library — the spec is the whole
    // truth — and a template edit re-pushes through the same path. Blocks
    // are positioned on the 754×428 Edit stage exactly like slide blocks;
    // SceneBuilder fills each block's `bind` ("text" ← slide text, "ref" ←
    // slide title, "title"/"notes" … per SlideResolver::BoundValue) and
    // renders them instead of the legacy title/body presets. TRANSIENT: not
    // persisted (StyleStore keeps only templateKey; the roster is the
    // durable copy), so an unknown id at boot degrades to the plain layout
    // until the roster's next push.
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
