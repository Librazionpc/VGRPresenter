#include "modules/presentation/SlideRules.hpp"

#include "core/config/Json.hpp"

#include <array>
#include <mutex>

namespace bps::presentation::SlideRules {
// THE CONTENT TYPE IS READ OFF THE SLIDE'S META JSON — which for a reading slide carries the passage itself (line1/line2
// are the verse text, several KB). Every frame of the live loop asks for it several times per scene (the scene id, the
// family slot, the composer, the table check) for the program and for each output's own pass, and each ask parsed the
// whole document: a profile of the audience render thread put roughly half of its busy time in that parser. The answer
// depends only on the text of the JSON, so the last few distinct documents are remembered (compared by value: a few KB
// of memcmp against a parse of every character). One process-wide cache behind a mutex — not thread_local: this runs on
// the live loop AND the interface thread, and a thread_local holding strings runs its destructor at thread exit, which
// on this toolchain corrupted the heap when a worker thread ended. The storage is deliberately never destroyed.
std::string ContentType(const Slide& slide) {
    struct Entry {
        std::string meta;
        std::string type;
        bool used = false;
    };
    struct Cache {
        std::mutex mutex;
        std::array<Entry, 8> entries;
        std::size_t next = 0;
    };
    static Cache* const cache = new Cache;
    {
        std::lock_guard<std::mutex> lock(cache->mutex);
        for (const Entry& e : cache->entries)
            if (e.used && e.meta == slide.metaJson)
                return e.type;
    }
    std::string type;
    if (auto meta = json::Parse(slide.metaJson); meta.ok())
        if (const json::Value* v = meta.value().Find("contentType");
            v && v->type() == json::Value::Type::String)
            type = std::string(v->asString());
    {
        std::lock_guard<std::mutex> lock(cache->mutex);
        Entry& slot = cache->entries[cache->next++ % cache->entries.size()];
        slot.meta = slide.metaJson;
        slot.type = type;
        slot.used = true;
    }
    return type;
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