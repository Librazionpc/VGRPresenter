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

bool StyleAdmits(const OutputStyleSpec& style, const Slide& slide) {
    if (!style.showShows && !style.showMedia && !style.showScripture
        && !style.showTable)
        return true;
    const std::string fam = ContentType(slide);
    if (fam.empty())        return true;
    if (fam == "shows")     return style.showShows;
    if (fam == "media")     return style.showMedia;
    if (fam == "scripture") return style.showScripture;
    if (fam == "table")     return style.showTable;
    return true;
}

} // namespace bps::presentation::SlideRules
