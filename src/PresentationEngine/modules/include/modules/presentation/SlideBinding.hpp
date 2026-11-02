#pragma once

// ---- SlideBinding: "what does this slide SHOW", as data ----------------------

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "modules/presentation/BookFill.hpp"          // BookValues
#include "modules/presentation/PresentationTypes.hpp" // Slide, ContentBlock
#include "modules/presentation/SceneBuilder.hpp"      // StyleBuilder::LayoutRect
#include "modules/rendering/RenderTypes.hpp"          // Rect, Size

namespace bps::presentation {
constexpr float kDesignW = 1920.0f;
constexpr float kDesignH = 1080.0f;

// A layout slot scaled onto the real stage.
rendering::Rect Scaled(const StyleBuilder::LayoutRect& r, const rendering::Size& stage);

// A stable 64-bit FNV-1a over a set of blocks — the scene-id fingerprint.
uint64_t HashBlocks(const std::vector<ContentBlock>& blocks);

// The placeholder values a SLIDE carries, in BookFill's one vocabulary.
BookValues SlideValues(const Slide& slide);

// Fills every bound/placeholder text block of a baked template from `slide`.
std::vector<ContentBlock> BindTemplateBlocks(const std::vector<ContentBlock>& tmpl,
                                             const Slide& slide);

// Does this composition paint anything at all? (A layout whose every text slot
// came out empty paints a blank frame.)
bool CompositionPaints(const std::vector<ContentBlock>& blocks);

// Returns true only for the first request to report a blank slide id.
bool FirstBlankSlideReport(std::string_view slideId);

// Can this slide feed a built-in PRESET? (The preset path draws only the slide's
// own title and body.)
bool PresetHasContent(const Slide& slide);
} // namespace bps::presentation