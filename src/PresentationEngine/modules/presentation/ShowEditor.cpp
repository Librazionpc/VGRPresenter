#include "modules/presentation/ShowEditor.hpp"

#include "core/config/Json.hpp"
#include "modules/presentation/BlockValidator.hpp"
#include "modules/presentation/PresentationTemplates.hpp"

#include <algorithm>
#include <format>
#include <set>

namespace bps::presentation {

namespace {

constexpr const char* kModule = "ShowEditor";

Error Invalid(std::string message) { return Error::Make(Err::InvalidArgument, kModule, std::move(message)); }
Error Missing(std::string_view what, std::string_view id) {
    return Error::Make(Err::NotFound, kModule, std::format("no {} '{}' in this show", what, id));
}

template <typename T>
typename std::vector<T>::iterator FindById(std::vector<T>& items, std::string_view id) {
    return std::find_if(items.begin(), items.end(), [&](const T& t) { return t.id == id; });
}

// "<prefix>-<n>" with the smallest n not already used. Ids are never derived from
// list position, so deleting an item never causes a later one to take its id.
template <typename T>
std::string NextId(const std::vector<T>& items, std::string_view prefix) {
    std::set<std::string> used;
    for (const auto& i : items) used.insert(i.id);
    for (size_t n = items.size() + 1;; ++n) {
        std::string candidate = std::format("{}-{}", prefix, n);
        if (!used.count(candidate)) return candidate;
    }
}

// Validates a caller-supplied id (must be free) or generates one.
template <typename T>
Result<std::string> ClaimId(const std::vector<T>& items, std::string requested,
                            std::string_view prefix, std::string_view what) {
    if (requested.empty()) return NextId(items, prefix);
    const bool taken = std::any_of(items.begin(), items.end(),
                                   [&](const T& t) { return t.id == requested; });
    if (taken) return Invalid(std::format("a {} with id '{}' already exists", what, requested));
    return requested;
}

// A list of blocks entering the engine (a slide's, a template's, an overlay's): blocks that have no id yet are given one - the
// engine issues ids - and then the whole list must pass BlockValidator, or nothing is changed.
Result<void> AcceptBlocks(std::vector<ContentBlock>& blocks) {
    for (ContentBlock& b : blocks)
        if (b.id.empty()) b.id = NextId(blocks, "item");
    return CheckBlocks(blocks);
}

template <typename T>
void MoveTo(std::vector<T>& items, size_t from, size_t to) {
    to = std::min(to, items.size() - 1);
    if (from == to) return;
    T item = std::move(items[from]);
    items.erase(items.begin() + static_cast<long>(from));
    items.insert(items.begin() + static_cast<long>(to), std::move(item));
}

} // namespace

// ============================================================================
// Categories
// ============================================================================

Result<std::string> ShowEditor::AddCategory(Presentation& show, Category category) {
    if (category.name.empty()) return Invalid("a category needs a name");
    if (!category.templateId.empty() && !SlideResolver::FindTemplate(show, category.templateId))
        return Invalid(std::format("template '{}' does not exist", category.templateId));
    auto id = ClaimId(show.categories, category.id, "category", "category");
    if (!id.ok()) return id.error();
    category.id = id.value();
    show.categories.push_back(std::move(category));
    return show.categories.back().id;
}

Result<void> ShowEditor::UpdateCategory(Presentation& show, std::string_view id, const CategoryPatch& patch) {
    auto it = FindById(show.categories, id);
    if (it == show.categories.end()) return Missing("category", id);
    if (patch.name && patch.name->empty()) return Invalid("a category needs a name");
    if (patch.templateId && !patch.templateId->empty() &&
        !SlideResolver::FindTemplate(show, *patch.templateId))
        return Invalid(std::format("template '{}' does not exist", *patch.templateId));
    // Validated above — everything below cannot fail, so the edit is atomic.
    if (patch.name) it->name = *patch.name;
    if (patch.contentType) it->contentType = *patch.contentType;
    if (patch.templateId) it->templateId = *patch.templateId;
    if (patch.outputs) it->outputs = *patch.outputs;
    if (patch.metaJson) it->metaJson = *patch.metaJson;
    return Ok();
}

Result<void> ShowEditor::RemoveCategory(Presentation& show, std::string_view id) {
    auto it = FindById(show.categories, id);
    if (it == show.categories.end()) return Missing("category", id);
    for (auto& s : show.slides)
        if (s.categoryId == id) s.categoryId.clear();
    show.overlays.erase(
        std::remove_if(show.overlays.begin(), show.overlays.end(),
                       [&](const Overlay& o) { return o.scope == OverlayScope::Category && o.targetId == id; }),
        show.overlays.end());
    show.categories.erase(it);
    return Ok();
}

Result<void> ShowEditor::MoveCategory(Presentation& show, std::string_view id, size_t newIndex) {
    auto it = FindById(show.categories, id);
    if (it == show.categories.end()) return Missing("category", id);
    MoveTo(show.categories, static_cast<size_t>(it - show.categories.begin()), newIndex);
    return Ok();
}

Result<void> ShowEditor::AssignTemplate(Presentation& show, std::string_view categoryId,
                                        std::string_view templateId) {
    CategoryPatch patch;
    patch.templateId = std::string(templateId);
    return UpdateCategory(show, categoryId, patch);
}

Result<void> ShowEditor::SetSlideCategory(Presentation& show, std::string_view slideId,
                                          std::string_view categoryId) {
    auto it = FindById(show.slides, slideId);
    if (it == show.slides.end()) return Missing("slide", slideId);
    if (!categoryId.empty() && !SlideResolver::FindCategory(show, categoryId))
        return Invalid(std::format("category '{}' does not exist", categoryId));
    it->categoryId = std::string(categoryId);
    return Ok();
}

// ============================================================================
// Templates
// ============================================================================

Result<std::string> ShowEditor::AddTemplate(Presentation& show, SlideTemplate tmpl) {
    if (tmpl.name.empty()) return Invalid("a template needs a name");
    if (auto ok = AcceptBlocks(tmpl.blocks); !ok.ok()) return ok.error();
    auto id = ClaimId(show.templates, tmpl.id, "template", "template");
    if (!id.ok()) return id.error();
    tmpl.id = id.value();
    show.templates.push_back(std::move(tmpl));
    return show.templates.back().id;
}

Result<void> ShowEditor::UpdateTemplate(Presentation& show, std::string_view id, SlideTemplate tmpl) {
    auto it = FindById(show.templates, id);
    if (it == show.templates.end()) return Missing("template", id);
    if (tmpl.name.empty()) return Invalid("a template needs a name");
    if (auto ok = AcceptBlocks(tmpl.blocks); !ok.ok()) return ok;
    tmpl.id = it->id;   // the id is the template's identity — never changed by an update
    *it = std::move(tmpl);
    return Ok();
}

Result<void> ShowEditor::RemoveTemplate(Presentation& show, std::string_view id, bool force) {
    auto it = FindById(show.templates, id);
    if (it == show.templates.end()) return Missing("template", id);
    std::vector<std::string> users;
    for (const auto& c : show.categories)
        if (c.templateId == id) users.push_back(c.name.empty() ? c.id : c.name);
    if (!users.empty() && !force) {
        std::string list;
        for (const auto& u : users) list += (list.empty() ? "" : ", ") + u;
        return Error::Make(Err::InvalidState, kModule,
                           std::format("template '{}' is still used by: {}", id, list));
    }
    for (auto& c : show.categories)
        if (c.templateId == id) c.templateId.clear();
    show.templates.erase(it);
    return Ok();
}

Result<std::string> ShowEditor::DuplicateTemplate(Presentation& show, std::string_view id) {
    auto it = FindById(show.templates, id);
    if (it == show.templates.end()) return Missing("template", id);
    SlideTemplate copy = *it;
    copy.id.clear();
    copy.name += " copy";
    return AddTemplate(show, std::move(copy));
}

// ============================================================================
// Overlays
// ============================================================================

namespace {
Result<void> CheckOverlayTarget(const Presentation& show, const Overlay& o) {
    if (o.scope == OverlayScope::Category && !SlideResolver::FindCategory(show, o.targetId))
        return Invalid(std::format("overlay targets category '{}' which does not exist", o.targetId));
    if (o.scope == OverlayScope::Slide && !SlideResolver::FindSlide(show, o.targetId))
        return Invalid(std::format("overlay targets slide '{}' which does not exist", o.targetId));
    return Ok();
}
} // namespace

Result<std::string> ShowEditor::AddOverlay(Presentation& show, Overlay overlay) {
    if (auto t = CheckOverlayTarget(show, overlay); !t.ok()) return t.error();
    if (auto ok = AcceptBlocks(overlay.blocks); !ok.ok()) return ok.error();
    auto id = ClaimId(show.overlays, overlay.id, "overlay", "overlay");
    if (!id.ok()) return id.error();
    overlay.id = id.value();
    show.overlays.push_back(std::move(overlay));
    return show.overlays.back().id;
}

Result<void> ShowEditor::UpdateOverlay(Presentation& show, std::string_view id, Overlay overlay) {
    auto it = FindById(show.overlays, id);
    if (it == show.overlays.end()) return Missing("overlay", id);
    if (auto t = CheckOverlayTarget(show, overlay); !t.ok()) return t;
    if (auto ok = AcceptBlocks(overlay.blocks); !ok.ok()) return ok;
    overlay.id = it->id;
    *it = std::move(overlay);
    return Ok();
}

Result<void> ShowEditor::RemoveOverlay(Presentation& show, std::string_view id) {
    auto it = FindById(show.overlays, id);
    if (it == show.overlays.end()) return Missing("overlay", id);
    show.overlays.erase(it);
    return Ok();
}

Result<void> ShowEditor::SetOverlayEnabled(Presentation& show, std::string_view id, bool enabled) {
    auto it = FindById(show.overlays, id);
    if (it == show.overlays.end()) return Missing("overlay", id);
    it->enabled = enabled;
    return Ok();
}

// ============================================================================
// Slides
// ============================================================================

Result<std::string> ShowEditor::AddSlide(Presentation& show, Slide slide, std::optional<size_t> index) {
    if (!slide.categoryId.empty() && !SlideResolver::FindCategory(show, slide.categoryId))
        return Invalid(std::format("category '{}' does not exist", slide.categoryId));
    if (auto ok = AcceptBlocks(slide.blocks); !ok.ok()) return ok.error();
    auto id = ClaimId(show.slides, slide.id, "slide", "slide");
    if (!id.ok()) return id.error();
    slide.id = id.value();
    const size_t at = index ? std::min(*index, show.slides.size()) : show.slides.size();
    show.slides.insert(show.slides.begin() + static_cast<long>(at), std::move(slide));
    return show.slides[at].id;
}

Result<void> ShowEditor::UpdateSlide(Presentation& show, std::string_view id, Slide slide) {
    auto it = FindById(show.slides, id);
    if (it == show.slides.end()) return Missing("slide", id);
    if (!slide.categoryId.empty() && !SlideResolver::FindCategory(show, slide.categoryId))
        return Invalid(std::format("category '{}' does not exist", slide.categoryId));
    if (auto ok = AcceptBlocks(slide.blocks); !ok.ok()) return ok;
    slide.id = it->id;
    *it = std::move(slide);
    return Ok();
}

Result<void> ShowEditor::RemoveSlide(Presentation& show, std::string_view id) {
    auto it = FindById(show.slides, id);
    if (it == show.slides.end()) return Missing("slide", id);
    show.overlays.erase(
        std::remove_if(show.overlays.begin(), show.overlays.end(),
                       [&](const Overlay& o) { return o.scope == OverlayScope::Slide && o.targetId == id; }),
        show.overlays.end());
    show.slides.erase(it);
    return Ok();
}

Result<void> ShowEditor::MoveSlide(Presentation& show, std::string_view id, size_t newIndex) {
    auto it = FindById(show.slides, id);
    if (it == show.slides.end()) return Missing("slide", id);
    MoveTo(show.slides, static_cast<size_t>(it - show.slides.begin()), newIndex);
    return Ok();
}

Result<std::string> ShowEditor::DuplicateSlide(Presentation& show, std::string_view id) {
    auto it = FindById(show.slides, id);
    if (it == show.slides.end()) return Missing("slide", id);
    Slide copy = *it;
    copy.id.clear();
    const size_t after = static_cast<size_t>(it - show.slides.begin()) + 1;
    return AddSlide(show, std::move(copy), after);
}

Result<void> ShowEditor::SetNextTimer(Presentation& show, double seconds, const std::vector<std::string>& ids) {
    if (!(seconds >= 0.0) || seconds > 3600.0)   // (also refuses NaN)
        return Invalid("a next timer is between 0 seconds and an hour");
    for (const std::string& id : ids)
        if (FindById(show.slides, id) == show.slides.end()) return Missing("slide", id);
    for (Slide& slide : show.slides) {
        const bool picked = ids.empty() ? !slide.hidden : std::find(ids.begin(), ids.end(), slide.id) != ids.end();
        if (picked) slide.durationMs = seconds * 1000.0;
    }
    return Ok();
}

double ShowEditor::TotalNextTimer(const Presentation& show) {
    double total = 0.0;
    for (const Slide& slide : show.slides)
        if (!slide.hidden) total += slide.durationMs / 1000.0;
    return total;
}

// ============================================================================
// The slide menu's operations
// ============================================================================

namespace {

Result<std::vector<std::string>> PickedIds(Presentation& show, const std::vector<std::string>& ids) {
    if (ids.empty()) return Invalid("no slide was chosen");
    std::vector<std::string> out;
    for (const std::string& id : ids) {
        if (FindById(show.slides, id) == show.slides.end()) return Missing("slide", id);
        if (std::find(out.begin(), out.end(), id) == out.end()) out.push_back(id);
    }
    return out;
}

// --- UTF-8 case: ASCII, Latin-1, Greek and Cyrillic (what the songs and Bibles here are written in) ---
uint32_t Upper(uint32_t c) {
    if (c >= 'a' && c <= 'z') return c - 32;
    if (c >= 0xE0 && c <= 0xFE && c != 0xF7) return c - 32;
    if (c == 0xFF) return 0x178;
    if (c >= 0x3B1 && c <= 0x3C9 && c != 0x3C2) return c - 32;
    if (c >= 0x430 && c <= 0x44F) return c - 32;
    if (c >= 0x450 && c <= 0x45F) return c - 80;
    return c;
}
uint32_t Lower(uint32_t c) {
    if (c >= 'A' && c <= 'Z') return c + 32;
    if (c >= 0xC0 && c <= 0xDE && c != 0xD7) return c + 32;
    if (c == 0x178) return 0xFF;
    if (c >= 0x391 && c <= 0x3A9 && c != 0x3A2) return c + 32;
    if (c >= 0x410 && c <= 0x42F) return c + 32;
    if (c >= 0x400 && c <= 0x40F) return c + 80;
    return c;
}
// Reads one character; a byte that is not valid UTF-8 comes back as itself and advances by one.
uint32_t Next(std::string_view s, size_t& i) {
    const unsigned char c = static_cast<unsigned char>(s[i]);
    if (c < 0x80) { ++i; return c; }
    const size_t extra = (c & 0xE0) == 0xC0 ? 1 : (c & 0xF0) == 0xE0 ? 2 : (c & 0xF8) == 0xF0 ? 3 : 0;
    if (extra == 0 || i + extra >= s.size()) { ++i; return c; }   // a stray or cut-off byte is kept as it is
    uint32_t cp = extra == 1 ? (c & 0x1F) : extra == 2 ? (c & 0x0F) : (c & 0x07);
    for (size_t k = 1; k <= extra; ++k) {
        const unsigned char cc = static_cast<unsigned char>(s[i + k]);
        if ((cc & 0xC0) != 0x80) { ++i; return c; }
        cp = (cp << 6) | (cc & 0x3F);
    }
    i += extra + 1;
    return cp;
}
void Put(std::string& out, uint32_t cp) {
    if (cp < 0x80) out += static_cast<char>(cp);
    else if (cp < 0x800) { out += static_cast<char>(0xC0 | (cp >> 6)); out += static_cast<char>(0x80 | (cp & 0x3F)); }
    else if (cp < 0x10000) { out += static_cast<char>(0xE0 | (cp >> 12)); out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F)); out += static_cast<char>(0x80 | (cp & 0x3F)); }
    else { out += static_cast<char>(0xF0 | (cp >> 18)); out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F)); out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F)); out += static_cast<char>(0x80 | (cp & 0x3F)); }
}
std::string MapCase(std::string_view s, bool upper) {
    std::string out;
    for (size_t i = 0; i < s.size();) { const uint32_t c = Next(s, i); Put(out, upper ? Upper(c) : Lower(c)); }
    return out;
}
std::string CapitalizeFirst(std::string_view s) {
    if (s.empty()) return {};
    size_t i = 0;
    const uint32_t first = Next(s, i);
    std::string out;
    Put(out, Upper(first));
    out.append(s.substr(i));
    return out;
}
std::string TrimLine(std::string_view s) {
    // FreeShow's trim: no spaces at the ends, one space where there were several, no . , ! at the end
    std::string out;
    bool pendingSpace = false;
    for (char c : s) {
        if (c == ' ' || c == '\t' || c == '\r') { pendingSpace = !out.empty(); continue; }
        if (pendingSpace) { out += ' '; pendingSpace = false; }
        out += c;
    }
    while (!out.empty() && (out.back() == '.' || out.back() == ',' || out.back() == '!')) out.pop_back();
    while (!out.empty() && out.back() == ' ') out.pop_back();
    return out;
}
std::vector<std::string> Lines(std::string_view text) {
    std::vector<std::string> out;
    size_t start = 0;
    while (start <= text.size()) {
        const size_t end = text.find('\n', start);
        out.emplace_back(text.substr(start, end == std::string_view::npos ? std::string_view::npos : end - start));
        if (end == std::string_view::npos) break;
        start = end + 1;
    }
    return out;
}
std::string Join(const std::vector<std::string>& lines, size_t from, size_t to) {
    std::string out;
    for (size_t i = from; i < to && i < lines.size(); ++i) { if (i > from) out += '\n'; out += lines[i]; }
    return out;
}
std::string FormatText(std::string_view text, ShowEditor::TextFormat f) {
    std::vector<std::string> lines = Lines(text);
    for (std::string& line : lines) {
        switch (f) {
            case ShowEditor::TextFormat::Uppercase: line = MapCase(line, true); break;
            case ShowEditor::TextFormat::Lowercase: line = MapCase(line, false); break;
            case ShowEditor::TextFormat::Capitalize: line = CapitalizeFirst(line); break;
            case ShowEditor::TextFormat::Trim: line = TrimLine(line); break;
        }
    }
    return Join(lines, 0, lines.size());
}

const char* GroupLabelOf(std::string_view group) {
    if (group == "verse") return "Verse";
    if (group == "chorus") return "Chorus";
    if (group == "pre_chorus") return "Pre-Chorus";
    if (group == "bridge") return "Bridge";
    if (group == "intro") return "Intro";
    if (group == "outro") return "Outro";
    if (group == "tag") return "Tag";
    if (group == "break") return "Break";
    return nullptr;
}

// Applies `fn` to the words of a slide: its text and each of its text boxes.
template <typename Fn>
void EachWords(Slide& slide, Fn fn) {
    slide.text = fn(slide.text);
    for (ContentBlock& b : slide.blocks)
        if (b.kind == "text") b.text = fn(b.text);
}

} // namespace

Result<void> ShowEditor::SetSlidesHidden(Presentation& show, const std::vector<std::string>& ids, bool hidden) {
    auto picked = PickedIds(show, ids);
    if (!picked.ok()) return picked.error();
    for (const std::string& id : picked.value()) FindById(show.slides, id)->hidden = hidden;
    return Ok();
}

Result<void> ShowEditor::SetSlidesGroup(Presentation& show, const std::vector<std::string>& ids, std::string_view group) {
    const char* label = GroupLabelOf(group);
    if (!group.empty() && !label) return Invalid(std::format("'{}' is not a group", group));
    auto picked = PickedIds(show, ids);
    if (!picked.ok()) return picked.error();
    for (const std::string& id : picked.value()) {
        Slide& s = *FindById(show.slides, id);
        if (group.empty()) {
            if (!s.tags.empty()) s.tags.erase(s.tags.begin());
            s.title.clear();
        } else {
            if (s.tags.empty()) s.tags.push_back(std::string(group)); else s.tags.front() = std::string(group);
        }
    }
    if (!group.empty()) {
        // "Verse 1", "Verse 2": the number is the slide's place among the slides of that group
        size_t total = 0;
        for (const Slide& s : show.slides) if (!s.tags.empty() && s.tags.front() == group) ++total;
        size_t seen = 0;
        for (Slide& s : show.slides) {
            if (s.tags.empty() || s.tags.front() != group) continue;
            ++seen;
            if (std::find(picked.value().begin(), picked.value().end(), s.id) != picked.value().end())
                s.title = total == 1 ? std::string(label) : std::format("{} {}", label, seen);
        }
    }
    return Ok();
}

Result<void> ShowEditor::FormatSlidesText(Presentation& show, const std::vector<std::string>& ids, TextFormat format) {
    auto picked = PickedIds(show, ids);
    if (!picked.ok()) return picked.error();
    for (const std::string& id : picked.value())
        EachWords(*FindById(show.slides, id), [&](const std::string& t) { return FormatText(t, format); });
    return Ok();
}

Result<size_t> ShowEditor::ReplaceInSlides(Presentation& show, const std::vector<std::string>& ids, std::string_view find,
                                           std::string_view replacement, bool caseSensitive) {
    if (find.empty()) return Invalid("there is nothing to find");
    auto picked = PickedIds(show, ids);
    if (!picked.ok()) return picked.error();
    size_t count = 0;
    auto lowerAscii = [](std::string_view s) { std::string o(s); for (char& c : o) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + 32); return o; };
    const std::string needle = caseSensitive ? std::string(find) : lowerAscii(find);
    auto replaceAll = [&](const std::string& text) {
        const std::string hay = caseSensitive ? text : lowerAscii(text);
        std::string out;
        size_t at = 0;
        while (true) {
            const size_t hit = hay.find(needle, at);
            if (hit == std::string::npos) { out.append(text, at, std::string::npos); break; }
            out.append(text, at, hit - at);
            out.append(replacement);
            at = hit + needle.size();
            ++count;
        }
        return out;
    };
    for (const std::string& id : picked.value()) {
        Slide& s = *FindById(show.slides, id);
        const std::string original = s.text;
        s.text = replaceAll(s.text);
        for (ContentBlock& b : s.blocks) {
            if (b.kind != "text") continue;
            if (b.text == original) b.text = s.text;   // a text box that mirrors the slide's words is one replacement, not two
            else b.text = replaceAll(b.text);
        }
    }
    return count;
}

Result<std::vector<std::string>> ShowEditor::SplitSlidesInHalf(Presentation& show, const std::vector<std::string>& ids) {
    auto picked = PickedIds(show, ids);
    if (!picked.ok()) return picked.error();
    std::vector<std::string> made;
    for (const std::string& id : picked.value()) {
        auto it = FindById(show.slides, id);
        std::vector<std::string> lines = Lines(it->text);
        lines.erase(std::remove_if(lines.begin(), lines.end(), [](const std::string& l) { return l.empty(); }), lines.end());
        bool splittable = lines.size() > 1;
        for (const ContentBlock& b : it->blocks)
            if (b.kind == "text" && Lines(b.text).size() > 1) splittable = true;
        if (!splittable) continue;

        Slide second = *it;
        second.id.clear();
        auto halve = [](const std::string& text, std::string& first, std::string& rest) {
            std::vector<std::string> l = Lines(text);
            l.erase(std::remove_if(l.begin(), l.end(), [](const std::string& x) { return x.empty(); }), l.end());
            if (l.size() < 2) { first = text; rest = text; return; }   // too short to cut: both keep it
            const size_t half = (l.size() + 1) / 2;
            first = Join(l, 0, half);
            rest = Join(l, half, l.size());
        };
        halve(it->text, it->text, second.text);
        for (size_t b = 0; b < it->blocks.size(); ++b)
            if (it->blocks[b].kind == "text") halve(it->blocks[b].text, it->blocks[b].text, second.blocks[b].text);
        const size_t after = static_cast<size_t>(it - show.slides.begin()) + 1;
        auto added = AddSlide(show, std::move(second), after);
        if (!added.ok()) return added.error();
        made.push_back(added.value());
    }
    return made;
}

Result<void> ShowEditor::MergeSlides(Presentation& show, const std::vector<std::string>& ids) {
    auto picked = PickedIds(show, ids);
    if (!picked.ok()) return picked.error();
    if (picked.value().size() < 2) return Invalid("merging needs two or more slides");
    // by their place in the show: the earliest keeps the words of the others
    std::vector<std::string> ordered;
    for (const Slide& s : show.slides)
        if (std::find(picked.value().begin(), picked.value().end(), s.id) != picked.value().end()) ordered.push_back(s.id);
    Slide& first = *FindById(show.slides, ordered.front());
    auto firstText = [&]() -> ContentBlock* {
        for (ContentBlock& b : first.blocks) if (b.kind == "text") return &b;
        return nullptr;
    };
    for (size_t i = 1; i < ordered.size(); ++i) {
        const Slide other = *FindById(show.slides, ordered[i]);
        if (!other.text.empty()) first.text += (first.text.empty() ? "" : "\n") + other.text;
        for (const ContentBlock& b : other.blocks) {
            if (b.kind != "text" || b.text.empty()) continue;
            if (ContentBlock* into = firstText()) into->text += (into->text.empty() ? "" : "\n") + b.text;
        }
    }
    for (size_t i = 1; i < ordered.size(); ++i)
        if (auto gone = RemoveSlide(show, ordered[i]); !gone.ok()) return gone;
    return Ok();
}

Result<void> ShowEditor::SetSlidesTransition(Presentation& show, const std::vector<std::string>& ids, TransitionKind kind, double milliseconds) {
    if (!(milliseconds >= 0.0) || milliseconds > 10000.0) return Invalid("a transition lasts between 0 and 10 seconds");
    if (static_cast<int>(kind) < 0 || static_cast<int>(kind) > static_cast<int>(TransitionKind::Custom)) return Invalid("unknown transition");
    auto picked = PickedIds(show, ids);
    if (!picked.ok()) return picked.error();
    for (const std::string& id : picked.value()) {
        Slide& s = *FindById(show.slides, id);
        s.transitionIn = kind;
        s.transitionMs = milliseconds;
    }
    return Ok();
}

Result<void> ShowEditor::SetSlidesOutputs(Presentation& show, const std::vector<std::string>& ids, const std::vector<std::string>& outputIds) {
    auto picked = PickedIds(show, ids);
    if (!picked.ok()) return picked.error();
    for (const std::string& id : picked.value()) {
        Slide& s = *FindById(show.slides, id);
        json::Value::Object meta;
        if (auto parsed = json::Parse(s.metaJson); parsed.ok() && parsed.value().asObject()) meta = *parsed.value().asObject();
        if (outputIds.empty()) {
            meta.erase("outputs");
        } else {
            json::Value::Array list;
            for (const std::string& o : outputIds) list.push_back(json::Value::String(o));
            meta["outputs"] = json::Value(std::move(list));
        }
        s.metaJson = json::Value(std::move(meta)).ToString();
    }
    return Ok();
}

// ============================================================================
// Content blocks
// ============================================================================

namespace {
// Blocks have no `id` member named like Slide's, but the same field — reuse the
// helpers by treating them uniformly.
Result<Slide*> SlideOrError(Presentation& show, std::string_view id) {
    auto it = FindById(show.slides, id);
    if (it == show.slides.end()) return Missing("slide", id);
    return &*it;
}
} // namespace

Result<std::string> ShowEditor::AddBlock(Presentation& show, std::string_view slideId, ContentBlock block,
                                         std::optional<size_t> index) {
    auto slide = SlideOrError(show, slideId);
    if (!slide.ok()) return slide.error();
    Slide& s = *slide.value();
    if (auto ok = CheckBlock(block, /*requireId=*/false); !ok.ok()) return ok.error();
    if (s.blocks.size() >= kMaxBlocksPerList) return Invalid(std::format("a slide holds at most {} blocks", kMaxBlocksPerList));
    auto id = ClaimId(s.blocks, block.id, "item", "block");
    if (!id.ok()) return id.error();
    block.id = id.value();
    const size_t at = index ? std::min(*index, s.blocks.size()) : s.blocks.size();
    s.blocks.insert(s.blocks.begin() + static_cast<long>(at), std::move(block));
    return s.blocks[at].id;
}

Result<void> ShowEditor::UpdateBlock(Presentation& show, std::string_view slideId,
                                     std::string_view blockId, ContentBlock block) {
    auto slide = SlideOrError(show, slideId);
    if (!slide.ok()) return slide.error();
    auto it = FindById(slide.value()->blocks, blockId);
    if (it == slide.value()->blocks.end()) return Missing("block", blockId);
    if (auto ok = CheckBlock(block, /*requireId=*/false); !ok.ok()) return ok;
    block.id = it->id;
    *it = std::move(block);
    return Ok();
}

Result<void> ShowEditor::RemoveBlock(Presentation& show, std::string_view slideId, std::string_view blockId) {
    auto slide = SlideOrError(show, slideId);
    if (!slide.ok()) return slide.error();
    auto& blocks = slide.value()->blocks;
    auto it = FindById(blocks, blockId);
    if (it == blocks.end()) return Missing("block", blockId);
    blocks.erase(it);
    return Ok();
}

Result<void> ShowEditor::MoveBlock(Presentation& show, std::string_view slideId,
                                   std::string_view blockId, size_t newIndex) {
    auto slide = SlideOrError(show, slideId);
    if (!slide.ok()) return slide.error();
    auto& blocks = slide.value()->blocks;
    auto it = FindById(blocks, blockId);
    if (it == blocks.end()) return Missing("block", blockId);
    MoveTo(blocks, static_cast<size_t>(it - blocks.begin()), newIndex);
    return Ok();
}

Result<std::string> ShowEditor::DuplicateBlock(Presentation& show, std::string_view slideId,
                                               std::string_view blockId, double dx, double dy) {
    auto slide = SlideOrError(show, slideId);
    if (!slide.ok()) return slide.error();
    auto& blocks = slide.value()->blocks;
    auto it = FindById(blocks, blockId);
    if (it == blocks.end()) return Missing("block", blockId);
    ContentBlock copy = *it;
    copy.id.clear();
    copy.x += dx;
    copy.y += dy;
    const size_t above = static_cast<size_t>(it - blocks.begin()) + 1;
    return AddBlock(show, slideId, std::move(copy), above);
}

} // namespace bps::presentation
