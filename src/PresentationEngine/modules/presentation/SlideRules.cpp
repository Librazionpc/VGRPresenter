#include "modules/presentation/SlideRules.hpp"

#include "core/config/Json.hpp"

namespace bps::presentation::SlideRules {

std::string ContentType(const Slide& slide) {
    if (auto meta = json::Parse(slide.metaJson); meta.ok())
        if (const json::Value* v = meta.value().Find("contentType");
            v && v->type() == json::Value::Type::String)
            return std::string(v->asString());
    return {};
}

std::size_t FamilyTemplateSlot(const std::string& contentType) {
    // The named indices (PresentationTypes), never literals: this function is the
    // one place a content tag becomes a slot, and the numbers themselves live
    // beside the arrays they index.
    if (contentType == "media")     return kFamilySlotMedia;
    if (contentType == "scripture") return kFamilySlotScripture;
    if (contentType == "table")     return kFamilySlotTable;
    return kFamilySlotShows;   // "shows" — and untagged slides, which are shows content
}

std::size_t ContentFamilySlot(ContentFamily family) {
    // The filler's two families have a slot each by definition; "shows" and
    // "media" are page families the filler does not serve.
    return family == ContentFamily::Table ? kFamilySlotTable : kFamilySlotScripture;
}

std::string ContentText(const Slide& slide) {
    if (!slide.text.empty())
        return slide.text;
    // THE DESIGN'S OWN BODY FIRST. A design can carry SEVERAL slots now that the
    // metadata palette inserts one per field, so "whichever text-bearing block
    // comes first" is the author's canvas ORDER, not a statement about content:
    // a table design with the date code in its top corner put "47-0412 4" first,
    // and every consumer of "the body" inherited the code — the bind="text"
    // block and {table_text}/{scripture_text} all rendered the code where the
    // sermon's text belonged (the reported output), while the preview pane, which
    // fills from the design and never asks this rule, showed the text. The design
    // DOES name its body: the palette's Text field and the shipped layouts' own
    // Text() both carry bind "text", and so does the reference line's "ref".
    for (const ContentBlock& b : slide.blocks)
        if (b.kind == "text" && b.bind == "text" && !b.text.empty() && b.text.find('{') != 0)
            return b.text;
    for (const ContentBlock& b : slide.blocks)
        if (b.kind == "text" && !b.text.empty() && b.text.find('{') != 0)
            return b.text;
    return slide.title;
}

bool NeedsDistributedFrame(const std::vector<ContentBlock>& blocks) {
    for (const ContentBlock& b : blocks) {
        // The kinds a frontend can only stand in for: a camera card, a media or
        // audio source's icon-and-name tile, an image whose file it has no
        // pixels for. Everything else draws for real.
        if (b.kind == "camera" || b.kind == "media" || b.kind == "audio"
            || b.kind == "image")
            return true;
    }
    return false;
}

StyleLayoutScope LayoutScope(const OutputStyleSpec& style) {
    const std::size_t shows = kFamilySlotShows;
    // EXPLICITLY SHOWS: a template PICKED in the style's own Shows slot (the
    // one act that says "lay my shows out"). This is the case where a show's
    // own layout steps aside — the same test ComposeSlide makes, so the row's
    // sentence and the picture on air cannot disagree.
    //
    // content type alone is NOT that act: a style's contentType is its own
    // type (which picker lists it in), and "shows" is merely the default, so
    // it must not claim an authored show's layout — nor may the whole-style
    // template, which names no family at all (the report this boundary came
    // from: "I chose Lower Third for style 2 and it does the opposite").
    // The KEY is the pick: blocks alone can be the front-end's shows fallback
    // (the Scripture design baked into an unpicked slot), which is not a
    // choice — see ComposeSlide's stylePickedShowsLayout.
    if (!style.familyTemplateKeys[shows].empty())
        return StyleLayoutScope::Shows;
    // ANY OTHER TEMPLATE: the whole-style one (which is not keyed to a family,
    // so it restyles every CONTENT tab) or one of theirs. A show slide keeps its
    // own blocks under these — see ComposeSlide's shows branch.
    if (!style.templateBlocks.empty())
        return StyleLayoutScope::ContentFamilies;
    for (std::size_t i = 0; i < kFamilySlotCount; ++i) {
        if (i == shows) continue;
        if (!style.familyTemplateBlocks[i].empty() || !style.familyTemplateKeys[i].empty())
            return StyleLayoutScope::ContentFamilies;
    }
    return StyleLayoutScope::None;
}

bool StyleAdmitsFamily(const OutputStyleSpec& style, const std::string& contentType) {
    if (!style.showShows && !style.showMedia && !style.showScripture
        && !style.showTable)
        return true;
    if (contentType.empty())        return true;
    if (contentType == "shows")     return style.showShows;
    if (contentType == "media")     return style.showMedia;
    if (contentType == "scripture") return style.showScripture;
    if (contentType == "table")     return style.showTable;
    return true;
}

bool StyleAdmits(const OutputStyleSpec& style, const Slide& slide) {
    return StyleAdmitsFamily(style, ContentType(slide));
}

} // namespace bps::presentation::SlideRules
