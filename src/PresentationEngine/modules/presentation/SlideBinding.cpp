// SlideBinding — the data half of a scene build. See SlideBinding.hpp.
// Moved out of SceneBuilder.cpp unchanged (same behaviour, one file).
#include "modules/presentation/SlideBinding.hpp"

#include "modules/presentation/BlockMeta.hpp"
#include "modules/presentation/PresentationTemplates.hpp"
#include "modules/presentation/SlideRules.hpp"

#include <algorithm>
#include <mutex>
#include <set>

namespace bps::presentation {
rendering::Rect Scaled(const StyleBuilder::LayoutRect& r, const rendering::Size& stage) {
    return rendering::Rect(
        static_cast<int>(r.x * stage.width),
        static_cast<int>(r.y * stage.height),
        static_cast<int>(r.w * stage.width),
        static_cast<int>(r.h * stage.height));
}

// ---- template-blocks fingerprint -----------------------------------------
uint64_t HashBlocks(const std::vector<ContentBlock>& blocks) {
    uint64_t h = 1469598103934665603ull;   // FNV offset basis
    const auto mix = [&h](uint64_t v) {
        h ^= v;
        h *= 1099511628211ull;             // FNV prime
    };
    const auto mixStr = [&h, &mix](std::string_view s) {
        mix(s.size());
        for (const char c : s) mix(static_cast<unsigned char>(c));
    };
    mix(blocks.size());
    for (const ContentBlock& b : blocks) {
        mixStr(b.id);
        mixStr(b.kind);
        mixStr(b.text);
        mixStr(b.bind);
        mixStr(b.metaJson);
        const auto mixCoord = [&mix](double d) {
            mix(static_cast<uint64_t>(static_cast<int64_t>(d * 1000.0)));
        };
        mixCoord(b.x);
        mixCoord(b.y);
        mixCoord(b.width);
        mixCoord(b.height);
        mixStr(b.style.backgroundColor);
        mix(b.style.borderEnabled ? 1ull : 0ull);
        mixCoord(b.style.padding);
        mixCoord(b.style.cornerRadius);
        mixCoord(b.style.borderWidth);
        mixStr(b.style.borderStyle);
        mixStr(b.style.borderColor);
    }
    return h;
}

BookValues SlideValues(const Slide& slide) {
    BookValues v;
    v.family = FromTag(MetaString(slide.metaJson, "family"));
    v.text = SlideRules::ContentText(slide);
    v.reference = MetaString(slide.metaJson, "ref");
    if (v.reference.empty())
        v.reference = slide.title;
    v.referenceFull = MetaHas(slide.metaJson, "refFull") ? MetaString(slide.metaJson, "refFull") : v.reference;
    v.referenceLast = MetaHas(slide.metaJson, "refLast") ? MetaString(slide.metaJson, "refLast") : v.reference;
    v.title = MetaString(slide.metaJson, "tableTitle");
    if (v.title.empty())
        v.title = slide.title;
    v.code = MetaString(slide.metaJson, "tableCode");
    v.year = MetaString(slide.metaJson, "tableYear");
    v.numbers = MetaString(slide.metaJson, "tableNumbers");
    v.book = MetaString(slide.metaJson, "book");
    v.bookAbbr = MetaString(slide.metaJson, "bookAbbr");
    v.name = MetaString(slide.metaJson, "bible");
    v.chapter = MetaString(slide.metaJson, "chapter");
    v.copyright = MetaString(slide.metaJson, "copyright");
    v.bibles.clear();
    v.bibles.push_back({ v.name, v.bookAbbr, v.text });
    for (int n = 2; n <= 4; ++n) {
        const std::string prefix = "bible" + std::to_string(n);
        const std::string name = MetaString(slide.metaJson, prefix + "Name");
        const std::string abbr = MetaString(slide.metaJson, prefix + "Abbr");
        const std::string text = MetaString(slide.metaJson, prefix + "Text");
        if (name.empty() && abbr.empty() && text.empty())
            break;   // no further version on this slide: the vocabulary's slots stay empty
        v.bibles.push_back({ name, abbr, text });
    }
    return v;
}

std::vector<ContentBlock> BindTemplateBlocks(const std::vector<ContentBlock>& tmpl,
                                             const Slide& slide) {
    std::vector<ContentBlock> out = tmpl;
    const BookValues values = SlideValues(slide);
    for (ContentBlock& b : out) {
        if (b.kind != "text")
            continue;
        if (!b.bind.empty()) {
            std::string value = SlideResolver::BoundValue(slide, b.bind);
            if (value.empty()) {
                if (b.bind == "text")
                    value = SlideRules::ContentText(slide);
                else if (b.bind == "ref")
                    value = slide.title;
            }
            b.text = std::move(value);
        } else if (b.text.find('{') != std::string::npos) {
            b.text = HasTokens(b.text) ? FillTokens(b.text, values) : std::string();
            if (b.text.find_first_not_of(" \t\r\n") == std::string::npos)
                b.text.clear();
        }
    }
    return out;
}

static std::mutex g_blankSlideMutex;
static std::set<std::string> g_blankSlideLogged;

bool FirstBlankSlideReport(std::string_view slideId) {
    std::lock_guard<std::mutex> lock(g_blankSlideMutex);
    return g_blankSlideLogged.insert(std::string(slideId)).second;
}

bool CompositionPaints(const std::vector<ContentBlock>& blocks) {
    for (const ContentBlock& b : blocks) {
        if (b.kind != "text") return true;
        if (b.text.find_first_not_of(" \t\r\n") != std::string::npos) return true;
    }
    return false;
}

bool PresetHasContent(const Slide& slide) {
    return !slide.title.empty() || !slide.text.empty();
}
} // namespace bps::presentation