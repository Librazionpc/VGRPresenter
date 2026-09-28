#pragma once

// SceneBuilder (docs/specs/19 §Scene Builder). Converts presentation data into
// renderable scenes. The Presentation Engine never manipulates GPU objects
// directly — it builds rendering::Scene objects (Phase 6) and hands them to the
// Render Engine.
//
// Output styles (Settings · Styles): an overload of BuildSlideScene takes the
// on-air output's OutputStyleSpec and lets it override the scene's background
// and text layout. Scenes built under a style are keyed with a style
// fingerprint suffix so a style edit (or a style being cleared) rebuilds them
// instead of hitting the old cached scene.

#include "core/common/Common.hpp"
#include "modules/presentation/PresentationTypes.hpp"
#include "modules/rendering/RenderTypes.hpp"

namespace bps::rendering {
class RenderEngine;
}

namespace bps::presentation {

class StyleBuilder {
public:
    // Layout presets keyed by the style's templateKey (StyleListModel's
    // stored key: "lowerThird" | "title" | "sidebar" | "bottomBar" |
    // "fullscreen", or an engine design id; unknown keys fall back to the
    // default full layout). Values are Fractions Of the 1920×1080 design
    // grid — scaled onto the actual stage below, so any output resolution
    // renders the same composition.
    struct LayoutRect { float x, y, w, h; };
    struct Layout {
        LayoutRect title;
        LayoutRect body;
        // false = only the body block renders (FreeShow's lower-third /
        // sidebar compositions show just the one region).
        bool showTitle = true;
    };
    static Layout LayoutFor(std::string_view templateKey);

    // "#aarrggbb" / "#rrggbb" / "transparent" → render colour ("" → a+1 so
    // callers can detect "no colour given" separately from transparent).
    static rendering::Color ParseColor(std::string_view css);
};

class SceneBuilder {
public:
    // Builds (or returns the cached) rendering scene for one slide. The scene
    // is registered with the Render Engine and its id returned.
    Result<std::string> BuildSlideScene(const Presentation& presentation,
                                        const Slide& slide,
                                        rendering::RenderEngine& engine,
                                        rendering::Size size = rendering::Size(1920, 1080));

    // Style-aware build: the spec's background and layout preset override the
    // slide's own. Scenes get a style-fingerprint suffix on their id, so
    // editing a style (or assigning a different one) rebuilds instead of
    // serving the unstyled cache.
    Result<std::string> BuildSlideScene(const Presentation& presentation,
                                        const Slide& slide,
                                        const OutputStyleSpec& style,
                                        rendering::RenderEngine& engine,
                                        rendering::Size size = rendering::Size(1920, 1080));

    // GATE-AWARE build — the per-output rule pass: the slide's content family
    // is checked against the spec's show* gates and a REFUSED family renders
    // the style's background ONLY (colour + image, no content, no text) — the
    // output wearing this style shows its look, never the forbidden content.
    // Allowed families render exactly like BuildSlideScene. Scene ids carry a
    // "gated" marker so the gated and ungated versions of one slide coexist
    // in the render cache (different outputs, different rules, same slide).
    Result<std::string> BuildGatedSlideScene(const Presentation& presentation,
                                             const Slide& slide,
                                             const OutputStyleSpec& style,
                                             rendering::RenderEngine& engine,
                                             rendering::Size size = rendering::Size(1920, 1080));

    // Deterministic scene id for a slide ("pres:<presentationId>:<slideId>").
    static std::string SceneIdFor(const Presentation& p, const Slide& s);
    // Style-fingerprinted scene id: "<baseId>@s:<bg>-<key>-<clear>" — a change
    // to any styled field yields a different id (and a rebuild).
    static std::string StyledSceneIdFor(const Presentation& p, const Slide& s,
                                        const OutputStyleSpec& style);
};

} // namespace bps::presentation
