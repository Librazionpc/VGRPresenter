#pragma once

// Templates, categories and overlays at work (docs/specs/19, docs/specs/23):
//
//   SlideResolver — turns a slide + its show into what is actually drawn:
//       category  -> assigned template -> template blocks bound to the slide's
//       content, the slide's own block overrides, and the overlays that apply.
//   TemplateFile  — a slide template as its own .vgr document (type Template), so
//       templates are files: shareable, versionable, kept in a library.
//
// Pure functions over the model — no rendering, no engine state, thread-safe.

#include "core/common/Common.hpp"
#include "modules/presentation/PresentationTypes.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace bps::presentation {

// A slide as it should be drawn on one output.
struct ResolvedSlide {
    std::string slideId;
    std::string categoryId;
    std::string templateId;                 // "" when the slide uses no template
    // False when the slide's category excludes the requested output (e.g. pastor
    // notes on the audience output) — the frontend/renderer skips it there.
    bool visible = true;
    std::string background;
    std::vector<ContentBlock> blocks;       // bound template blocks + slide extras
    std::vector<ContentBlock> overlayBlocks;// overlays drawn ABOVE, in show order
};

class SlideResolver {
public:
    static const Category* FindCategory(const Presentation& show, std::string_view id);
    static const SlideTemplate* FindTemplate(const Presentation& show, std::string_view id);
    static const Slide* FindSlide(const Presentation& show, std::string_view id);
    // The template a slide uses: its category's assigned template (null when the
    // slide is uncategorised, the category has none, or it names a template the
    // show does not contain).
    static const SlideTemplate* TemplateFor(const Presentation& show, const Slide& slide);

    // The value a template block's `bind` field reads from a slide: "title",
    // "text", "notes", or "line1"/"line2"/"ref"/… from the slide's meta.
    static std::string BoundValue(const Slide& slide, std::string_view field);

    // Resolves one slide. `outputId` = which output it is being drawn for; ""
    // means "no output filtering" (the editor view). Rules:
    //   * template blocks come first, each with `bind` replaced by the slide's
    //     value for that field;
    //   * a slide block with the SAME id as a template block REPLACES it (a
    //     per-slide tweak: moved, resized, restyled or retyped);
    //   * other slide blocks are appended (slide-specific extras);
    //   * the slide's own background wins unless it is unset ("" / "transparent"),
    //     then the template's applies;
    //   * overlays (enabled, in scope, matching the output) are collected into
    //     overlayBlocks.
    // Errors only when `slideId` is not in the show.
    static Result<ResolvedSlide> Resolve(const Presentation& show, std::string_view slideId,
                                         std::string_view outputId = {});

    // Ids the show refers to that it does not contain — categories -> templates,
    // slides -> categories, overlays -> targets. Empty when consistent. (The
    // validator reports these as warnings.)
    static std::vector<std::string> DanglingReferences(const Presentation& show);
};

// A slide template as a standalone .vgr document.
class TemplateFile {
public:
    static Result<void> Save(const SlideTemplate& tmpl, const std::string& path);
    static Result<SlideTemplate> Load(const std::string& path);
};

} // namespace bps::presentation
