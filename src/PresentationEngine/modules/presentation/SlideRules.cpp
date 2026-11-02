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
        if (b.kind == "camera" || b.kind == "media" || b.kind == "audio"
            || b.kind == "image")
            return true;
    }
    return false;
}

StyleLayoutScope LayoutScope(const OutputStyleSpec& style) {
    const std::size_t shows = kFamilySlotShows;
    if (!style.familyTemplateKeys[shows].empty())
        return StyleLayoutScope::Shows;
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