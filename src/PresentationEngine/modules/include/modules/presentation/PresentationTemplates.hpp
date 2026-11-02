#pragma once

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
    static const SlideTemplate* TemplateFor(const Presentation& show, const Slide& slide);

    // The value a template block's `bind` field reads from a slide: "title",
    // "text", "notes", or "line1"/"line2"/"ref"/… from the slide's meta.
    static std::string BoundValue(const Slide& slide, std::string_view field);

    static Result<ResolvedSlide> Resolve(const Presentation& show, std::string_view slideId,
                                         std::string_view outputId = {});

    static std::vector<std::string> DanglingReferences(const Presentation& show);
};

// A slide template as a standalone .vgr document.
class TemplateFile {
public:
    static Result<void> Save(const SlideTemplate& tmpl, const std::string& path);
    static Result<SlideTemplate> Load(const std::string& path);
};
} // namespace bps::presentation