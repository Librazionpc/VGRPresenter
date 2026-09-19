#include "modules/presentation/PresentationCompiler.hpp"

#include <utility>

namespace bps::presentation {

namespace {

std::string SceneIdFor(const Presentation& p, const Slide& s) {
    return "pres:" + p.id + ":" + s.id;
}

} // namespace

CompiledPresentation PresentationCompiler::Compile(
    const Presentation& presentation, const std::vector<ValidationIssue>& validation,
    const std::map<std::string, std::string, std::less<>>* sceneIds) const {
    CompiledPresentation out;
    out.presentationId = presentation.id;

    for (const auto& issue : validation) {
        if (issue.severity >= 1) ++out.errors; else ++out.warnings;
    }
    if (presentation.slides.empty()) {
        out.ok = false;
        return out;
    }

    size_t visibleIndex = 0;
    for (const auto& slide : presentation.slides) {
        if (slide.hidden) continue;   // hidden slides are skipped in the compiled order
        CompiledSlide cs;
        cs.slideId = slide.id;
        cs.index = visibleIndex;
        cs.transitionIn = slide.transitionIn;
        cs.transitionMs = slide.transitionMs > 0 ? slide.transitionMs
                                                 : presentation.defaultTransitionMs;
        cs.durationMs = slide.durationMs;
        if (sceneIds) {
            auto it = sceneIds->find(slide.id);
            cs.sceneId = it != sceneIds->end() ? it->second : SceneIdFor(presentation, slide);
        } else {
            cs.sceneId = SceneIdFor(presentation, slide);
        }
        out.indexById[slide.id] = visibleIndex;
        out.slides.push_back(std::move(cs));
        ++visibleIndex;
    }

    out.ok = true;
    return out;
}

} // namespace bps::presentation
