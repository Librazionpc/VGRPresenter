#include "modules/presentation/ShowEditor.hpp"

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
