#pragma once

// SceneBuilder (docs/specs/19 §Scene Builder). Converts presentation data into
// renderable scenes. The Presentation Engine never manipulates GPU objects
// directly — it builds rendering::Scene objects (Phase 6) and hands them to the
// Render Engine.

#include "core/common/Common.hpp"
#include "modules/presentation/PresentationTypes.hpp"
#include "modules/rendering/RenderTypes.hpp"

namespace bps::rendering {
class RenderEngine;
}

namespace bps::presentation {

class SceneBuilder {
public:
    // Builds (or returns the cached) rendering scene for one slide. The scene
    // is registered with the Render Engine and its id returned.
    Result<std::string> BuildSlideScene(const Presentation& presentation,
                                        const Slide& slide,
                                        rendering::RenderEngine& engine,
                                        rendering::Size size = rendering::Size(1920, 1080));

    // Deterministic scene id for a slide ("pres:<presentationId>:<slideId>").
    static std::string SceneIdFor(const Presentation& p, const Slide& s);
};

} // namespace bps::presentation
