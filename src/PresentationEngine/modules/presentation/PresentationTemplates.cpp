#include "modules/presentation/PresentationTemplates.hpp"

#include "core/config/Json.hpp"
#include "modules/presentation/PresentationSerializer.hpp"
#include "modules/vgr/VgrFile.hpp"

#include <algorithm>
#include <chrono>
#include <format>

namespace bps::presentation {

namespace {

constexpr const char* kModule = "PresentationTemplates";

bool Unset(const std::string& background) {
    return background.empty() || background == "transparent";
}

bool ContainsOutput(const std::vector<std::string>& outputs, std::string_view outputId) {
    // Empty filter = every output; an empty request = no filtering.
    if (outputs.empty() || outputId.empty()) return true;
    return std::find(outputs.begin(), outputs.end(), outputId) != outputs.end();
}

bool OverlayApplies(const Overlay& overlay, const Slide& slide) {
    switch (overlay.scope) {
        case OverlayScope::All:      return true;
        case OverlayScope::Category: return !slide.categoryId.empty() && overlay.targetId == slide.categoryId;
        case OverlayScope::Slide:    return overlay.targetId == slide.id;
    }
    return false;
}

int64_t NowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch()).count();
}

} // namespace

const Category* SlideResolver::FindCategory(const Presentation& show, std::string_view id) {
    for (const auto& c : show.categories)
        if (c.id == id) return &c;
    return nullptr;
}

const SlideTemplate* SlideResolver::FindTemplate(const Presentation& show, std::string_view id) {
    for (const auto& t : show.templates)
        if (t.id == id) return &t;
    return nullptr;
}

const Slide* SlideResolver::FindSlide(const Presentation& show, std::string_view id) {
    for (const auto& s : show.slides)
        if (s.id == id) return &s;
    return nullptr;
}

const SlideTemplate* SlideResolver::TemplateFor(const Presentation& show, const Slide& slide) {
    if (slide.categoryId.empty()) return nullptr;
    const Category* category = FindCategory(show, slide.categoryId);
    if (!category || category->templateId.empty()) return nullptr;
    return FindTemplate(show, category->templateId);
}

std::string SlideResolver::BoundValue(const Slide& slide, std::string_view field) {
    if (field == "title") return slide.title;
    if (field == "text") return slide.text;
    if (field == "notes") return slide.notes;
    // Anything else is a frontend field kept in the slide's meta (line1, line2,
    // ref, ...).
    if (auto meta = json::Parse(slide.metaJson); meta.ok())
        if (const json::Value* v = meta.value().Find(field); v && v->type() == json::Value::Type::String)
            return std::string(v->asString());
    return {};
}

Result<ResolvedSlide> SlideResolver::Resolve(const Presentation& show, std::string_view slideId,
                                             std::string_view outputId) {
    const Slide* slide = FindSlide(show, slideId);
    if (!slide)
        return Error::Make(Err::NotFound, kModule, std::format("no slide '{}' in this show", slideId));

    ResolvedSlide out;
    out.slideId = slide->id;
    out.categoryId = slide->categoryId;

    const Category* category = slide->categoryId.empty() ? nullptr : FindCategory(show, slide->categoryId);
    if (category && !ContainsOutput(category->outputs, outputId)) {
        out.visible = false;
        return out;   // nothing to draw on this output
    }

    const SlideTemplate* tmpl = TemplateFor(show, *slide);
    out.background = slide->background;
    if (tmpl) {
        out.templateId = tmpl->id;
        if (Unset(out.background)) out.background = tmpl->background;
        for (const ContentBlock& tb : tmpl->blocks) {
            // A slide block with the same id overrides this template block.
            const auto mine = std::find_if(slide->blocks.begin(), slide->blocks.end(),
                                               [&](const ContentBlock& sb) { return sb.id == tb.id; });
            if (mine != slide->blocks.end()) {
                out.blocks.push_back(*mine);
                continue;
            }
            ContentBlock bound = tb;
            if (!tb.bind.empty()) bound.text = BoundValue(*slide, tb.bind);
            out.blocks.push_back(std::move(bound));
        }
    }
    // Slide-specific extras (and ALL blocks when there is no template).
    for (const ContentBlock& sb : slide->blocks) {
        const bool handled = tmpl && std::any_of(tmpl->blocks.begin(), tmpl->blocks.end(),
                                                 [&](const ContentBlock& tb) { return tb.id == sb.id; });
        if (!handled) out.blocks.push_back(sb);
    }

    for (const Overlay& overlay : show.overlays) {
        if (!overlay.enabled || !OverlayApplies(overlay, *slide)) continue;
        if (!ContainsOutput(overlay.outputs, outputId)) continue;
        out.overlayBlocks.insert(out.overlayBlocks.end(), overlay.blocks.begin(), overlay.blocks.end());
    }
    return out;
}

std::vector<std::string> SlideResolver::DanglingReferences(const Presentation& show) {
    std::vector<std::string> issues;
    for (const auto& c : show.categories)
        if (!c.templateId.empty() && !FindTemplate(show, c.templateId))
            issues.push_back(std::format("category '{}' uses missing template '{}'", c.id, c.templateId));
    for (const auto& s : show.slides)
        if (!s.categoryId.empty() && !FindCategory(show, s.categoryId))
            issues.push_back(std::format("slide '{}' is in missing category '{}'", s.id, s.categoryId));
    for (const auto& o : show.overlays) {
        if (o.scope == OverlayScope::Category && !FindCategory(show, o.targetId))
            issues.push_back(std::format("overlay '{}' targets missing category '{}'", o.id, o.targetId));
        if (o.scope == OverlayScope::Slide && !FindSlide(show, o.targetId))
            issues.push_back(std::format("overlay '{}' targets missing slide '{}'", o.id, o.targetId));
    }
    return issues;
}

Result<void> TemplateFile::Save(const SlideTemplate& tmpl, const std::string& path) {
    PresentationSerializer serializer;
    auto body = serializer.SerializeTemplate(tmpl);
    if (!body.ok()) return body.error();

    vgr::VgrDocument doc;
    doc.header.engineVersion = kEngineVersion.ToString();
    doc.header.type = vgr::DocumentType::Template;
    doc.header.uuid = tmpl.id;
    doc.header.name = tmpl.name;
    doc.header.createdMs = doc.header.modifiedMs = NowMs();
    doc.header.searchMetadata["contentType"] = tmpl.contentType;
    doc.documentJson = std::move(body.value());
    return vgr::VgrFile::Write(path, doc);
}

Result<SlideTemplate> TemplateFile::Load(const std::string& path) {
    auto doc = vgr::VgrFile::Read(path);
    if (!doc.ok()) return doc.error();
    if (doc.value().header.type != vgr::DocumentType::Template)
        return Error::Make(Err::InvalidArgument, kModule,
                           std::string("this .vgr file is a ") + vgr::ToString(doc.value().header.type) +
                           " document, not a template");
    PresentationSerializer serializer;
    return serializer.DeserializeTemplate(doc.value().documentJson);
}

} // namespace bps::presentation
