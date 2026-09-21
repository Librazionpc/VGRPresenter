#pragma once

// ShowEditor (docs/architecture/PresentationEngine.md §Show structure): the engine
// owns every create / update / delete on a show — categories, templates,
// overlays and slides. A frontend asks for an operation; the engine validates it,
// generates ids, applies the cascades, and refuses anything that would leave the
// show inconsistent. Frontends never hand-edit these lists.
//
// Every operation works on a Presentation and is ATOMIC: it either applies fully
// and returns Ok / the new id, or returns an error and leaves the presentation
// exactly as it was. (PresentationDocument::Edit runs them on a copy and only
// commits on success, so even a multi-step edit is all-or-nothing.)
//
// Referential rules (what "consistent" means):
//   category.templateId  -> a template in the show, or "" (none)
//   slide.categoryId     -> a category in the show, or "" (uncategorised)
//   overlay.targetId     -> a category / slide in the show (for those scopes)
// Deleting something that others point at follows an explicit rule, documented on
// each Remove* — never a dangling id.
//
// Ids are stable and generated here ("category-3", "template-2", "overlay-1",
// "slide-7"): unique within the show when created and never derived from list
// position, so reordering or deleting never renames anything.

#include "core/common/Common.hpp"
#include "modules/presentation/PresentationTypes.hpp"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace bps::presentation {

// A partial update: only the fields that are set change.
struct CategoryPatch {
    std::optional<std::string> name;
    std::optional<std::string> contentType;
    std::optional<std::string> templateId;             // "" clears
    std::optional<std::vector<std::string>> outputs;   // {} = every output
    std::optional<std::string> metaJson;
};

class ShowEditor {
public:
    // ---- Categories ---------------------------------------------------------
    // Requires a non-empty name and, when templateId is set, an existing
    // template. An empty id is generated; a given id must be unused.
    static Result<std::string> AddCategory(Presentation& show, Category category);
    static Result<void> UpdateCategory(Presentation& show, std::string_view id, const CategoryPatch& patch);
    // Slides in the category become uncategorised (their content is untouched);
    // overlays scoped to the category are removed with it.
    static Result<void> RemoveCategory(Presentation& show, std::string_view id);
    static Result<void> MoveCategory(Presentation& show, std::string_view id, size_t newIndex);
    // Assigns (or with "" clears) the template every slide of the category uses.
    static Result<void> AssignTemplate(Presentation& show, std::string_view categoryId,
                                       std::string_view templateId);
    // Puts a slide in a category ("" = uncategorised).
    static Result<void> SetSlideCategory(Presentation& show, std::string_view slideId,
                                         std::string_view categoryId);

    // ---- Templates ----------------------------------------------------------
    static Result<std::string> AddTemplate(Presentation& show, SlideTemplate tmpl);
    // Replaces the template's content (name, blocks, background...), keeping its id.
    static Result<void> UpdateTemplate(Presentation& show, std::string_view id, SlideTemplate tmpl);
    // A template still assigned to categories is refused (InvalidState, naming
    // them) unless `force`: then those categories lose their template.
    static Result<void> RemoveTemplate(Presentation& show, std::string_view id, bool force = false);
    static Result<std::string> DuplicateTemplate(Presentation& show, std::string_view id);

    // ---- Overlays -----------------------------------------------------------
    // Category/Slide scopes require an existing target.
    static Result<std::string> AddOverlay(Presentation& show, Overlay overlay);
    static Result<void> UpdateOverlay(Presentation& show, std::string_view id, Overlay overlay);
    static Result<void> RemoveOverlay(Presentation& show, std::string_view id);
    static Result<void> SetOverlayEnabled(Presentation& show, std::string_view id, bool enabled);

    // ---- Slides -------------------------------------------------------------
    // Inserts at `index` (clamped; std::nullopt = append). Its categoryId, if set,
    // must exist.
    static Result<std::string> AddSlide(Presentation& show, Slide slide,
                                        std::optional<size_t> index = std::nullopt);
    // Replaces a slide's content, keeping its id and position.
    static Result<void> UpdateSlide(Presentation& show, std::string_view id, Slide slide);
    // Overlays scoped to the slide are removed with it.
    static Result<void> RemoveSlide(Presentation& show, std::string_view id);
    static Result<void> MoveSlide(Presentation& show, std::string_view id, size_t newIndex);
    static Result<std::string> DuplicateSlide(Presentation& show, std::string_view id);

    // ---- Content blocks on a slide (what a canvas edits) ----------------------
    // Block ids are unique within their slide; an empty id is generated ("item-N").
    static Result<std::string> AddBlock(Presentation& show, std::string_view slideId, ContentBlock block,
                                        std::optional<size_t> index = std::nullopt);
    // Replaces the block's content, keeping its id and stacking position.
    static Result<void> UpdateBlock(Presentation& show, std::string_view slideId,
                                    std::string_view blockId, ContentBlock block);
    static Result<void> RemoveBlock(Presentation& show, std::string_view slideId, std::string_view blockId);
    // Stacking order: index 0 is the back-most block.
    static Result<void> MoveBlock(Presentation& show, std::string_view slideId,
                                  std::string_view blockId, size_t newIndex);
    // A copy offset by (dx, dy), placed directly above the original; returns its id.
    static Result<std::string> DuplicateBlock(Presentation& show, std::string_view slideId,
                                              std::string_view blockId, double dx = 20, double dy = 20);
};

} // namespace bps::presentation
