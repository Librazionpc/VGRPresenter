#pragma once

#include "core/common/Common.hpp"
#include "modules/presentation/PresentationTypes.hpp"
#include "modules/rendering/RenderTypes.hpp"

namespace bps::rendering {
class RenderEngine;
}

namespace bps::presentation {
class StyleBuilder {
public:
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

    Result<std::string> BuildSlideScene(const Presentation& presentation,
                                        const Slide& slide,
                                        const OutputStyleSpec& style,
                                        rendering::RenderEngine& engine,
                                        rendering::Size size = rendering::Size(1920, 1080));

    Result<std::string> BuildGatedSlideScene(const Presentation& presentation,
                                             const Slide& slide,
                                             const OutputStyleSpec& style,
                                             rendering::RenderEngine& engine,
                                             rendering::Size size = rendering::Size(1920, 1080));

    bool RefreshReadingFlow(rendering::RenderEngine& engine, const std::string& sceneId,
                            const Slide& slide);

    Result<std::string> BuildBusFeedScene(int busId, rendering::RenderEngine& engine,
                                          rendering::Size size = rendering::Size(1920, 1080));

    Result<std::string> BuildTemplateBusScene(int busNumber,
                                              const std::vector<ContentBlock>& blocks,
                                              const std::string& background,
                                              rendering::RenderEngine& engine,
                                              rendering::Size size = rendering::Size(1920, 1080));

    struct BlockLayerPlan {
        std::vector<bool> underVideo;
        bool stageCovered = false;
    };
    static BlockLayerPlan PlanBlockLayers(const std::vector<ContentBlock>& blocks);

    struct SlideComposition {
        enum class Source {
            Cleared,          // a CLEAR carrier (Slide::clearStage): the frame paints NOTHING
            Refused,          // the style's gates refuse this slide's family
            FamilyTemplate,   // the style's per-family engine template (blocks)
            StyleTemplate,    // the style's whole-style template (blocks)
            FamilyPreset,     // per-family built-in layout preset (title + body)
            StylePreset,      // the style's own built-in layout preset (same)
            SlideBlocks,      // the slide's own blocks carry the layout
        };
        Source source = Source::SlideBlocks;
        std::string contentType;      // the slide's family ("" = untagged = shows)
        size_t familyIdx = 0;         // the style slot that family styles through

        std::vector<ContentBlock> blocks;
        std::string presetKey;        // the two preset sources
        StyleBuilder::Layout preset;

        bool paintBackground = true;
        std::string background;
        bool explicitTransparent = false;
        bool slideOwnsBackground = false;

        bool styleImageSuppressed = false;

        bool clearedByTake = false;

        bool frameTransparent = true;
    };
    static SlideComposition ComposeSlide(const Slide& slide, const OutputStyleSpec& style,
                                         bool applyGates = false);

    // Deterministic scene id for a slide ("pres:<presentationId>:<slideId>").
    static std::string SceneIdFor(const Presentation& p, const Slide& s);
    static std::string StyledSceneIdFor(const Presentation& p, const Slide& s,
                                        const OutputStyleSpec& style,
                                        rendering::Size sceneSize = rendering::Size(1920, 1080));
};
} // namespace bps::presentation