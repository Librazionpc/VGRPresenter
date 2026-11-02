#include "modules/presentation/SceneBuilder.hpp"

#include "modules/presentation/BlockMeta.hpp"
#include "modules/presentation/MediaPixels.hpp"
#include "modules/presentation/ScenePaint.hpp"
#include "modules/presentation/SlideBinding.hpp"
#include "modules/presentation/SlideRules.hpp"

#include "modules/presentation/BookFill.hpp"
#include "modules/presentation/CompositorState.hpp"
#include "modules/presentation/PresentationTemplates.hpp"
#include "modules/presentation/TextFormat.hpp"
#include "core/config/Json.hpp"
#include "core/logging/Logger.hpp"

#include <algorithm>
#include <format>
#include <string>
#include <vector>

namespace bps::presentation {
SceneBuilder::SlideComposition SceneBuilder::ComposeSlide(const Slide& slide,
                                                         const OutputStyleSpec& style,
                                                         bool applyGates) {
    SlideComposition comp;
    comp.contentType = SlideRules::ContentType(slide);
    comp.familyIdx = SlideRules::FamilyTemplateSlot(comp.contentType);

    // ---- A CLEAR PAINTS NOTHING AT ALL -------------------------------------
    if (slide.clearStage) {
        comp.source = SlideComposition::Source::Cleared;
        comp.paintBackground = false;
        comp.explicitTransparent = true;
        comp.styleImageSuppressed = true;
        return comp;
    }

    // ---- The background the output paints ---------------------------------
    comp.slideOwnsBackground = !slide.background.empty() && slide.background != "transparent";
    const rendering::Color styleBg = StyleBuilder::ParseColor(style.backgroundColor);
    if (styleBg.a > 0.0f && !(style.clearBackgroundOnText && comp.slideOwnsBackground)) {
        comp.background = style.backgroundColor;
    } else if (style.backgroundColor == "transparent") {
        comp.paintBackground = false;
        comp.explicitTransparent = true;
    } else if (comp.slideOwnsBackground
               && StyleBuilder::ParseColor(slide.background).a > 0.0f) {
        comp.background = slide.background;
    } else {
        comp.paintBackground = false;
    }

    comp.styleImageSuppressed = style.clearBackgroundOnText && !slide.blocks.empty();

    // ---- IS THIS FRAME TRANSPARENT WHERE NOTHING PAINTS? --------------------
    comp.frameTransparent = comp.explicitTransparent || !comp.paintBackground;

    // ---- A TAKE CLEARS THE STYLE'S BACKGROUND -------------------------------
    if (presentation::CompositorState::Instance().HasMediaFrame()) {
        comp.paintBackground = false;
        comp.background.clear();
        comp.explicitTransparent = true;
        comp.styleImageSuppressed = true;
        comp.clearedByTake = true;
        comp.frameTransparent = presentation::CompositorState::Instance().MediaFrameIsTransparent();
    } else if (!presentation::CompositorState::Instance().GetTakenInput().deviceId.empty()) {
        comp.frameTransparent = false;
    }

    // ---- Which layout owns the slide -------------------------------------
    if (applyGates && !SlideRules::StyleAdmits(style, slide)) {
        comp.source = SlideComposition::Source::Refused;
        return comp;
    }
    const bool slideHasContent = !slide.blocks.empty() || !slide.title.empty()
        || !slide.text.empty();

    // ---- A SHOW'S SLIDE IS ITS OWN DESIGN ---------------------------------
    const bool slideIsShowsContent =
        comp.contentType.empty() || comp.contentType == "shows";
    const auto slideMeta = json::Parse(slide.metaJson);
    const json::Value* templateOverride = slideMeta.ok()
        ? slideMeta.value().Find("templateOverride") : nullptr;
    const bool explicitSlideTemplate = templateOverride && templateOverride->asBool();
    const std::string showCategory = MetaString(slide.metaJson, "showCategory");
    const OutputStyleSpec::ShowCategoryTemplate* categoryTemplate = nullptr;
    if (slideIsShowsContent && !showCategory.empty()) {
        for (const auto& candidate : style.showCategoryTemplates) {
            if (candidate.overrideEnabled && candidate.category == showCategory
                && !candidate.blocks.empty()) {
                categoryTemplate = &candidate;
                break;
            }
        }
    }
    const bool stylePickedShowsLayout =
        categoryTemplate != nullptr
        || (!explicitSlideTemplate && comp.familyIdx < 4
            && !style.familyTemplateKeys[comp.familyIdx].empty());
    if (slideIsShowsContent && !slide.blocks.empty() && !stylePickedShowsLayout) {
        comp.source = SlideComposition::Source::SlideBlocks;
        return comp;   // `blocks` stays empty: the caller draws the slide's own
    }

    const auto fallBackToSlideBlocks = [&comp, &slide, &style](bool layoutPaints,
                                                              std::string_view what) {
        if (layoutPaints)
            return;
        if (slide.blocks.empty()) {
            const bool frameIsEmpty = !comp.paintBackground
                && (style.backgroundImage.empty() || comp.styleImageSuppressed);
            // A style with no name is the engine's own style-less preview build
            // (no output wears it), so a blank frame there is not a fault.
            if (!frameIsEmpty || style.name.empty())
                return;
            if (!FirstBlankSlideReport(slide.id))
                return;   // already said so for this slide
            Logger::Instance().Info(
                std::format("slide '{}' arrived with no blocks, no title and no body, and "
                            "{} of style '{}' has nothing to fill — the outputs compose a "
                            "blank frame (the content did not reach the engine)",
                            slide.id, what, style.name),
                "SceneBuilder");
            return;
        }
        Logger::Instance().Info(
            std::format("slide '{}': {} of style '{}' paints nothing (no bound text, no "
                        "shapes) — the slide's own blocks own the composition, so the outputs "
                        "and the NDI feed carry the picture instead of a blank frame",
                        slide.id, what, style.name),
            "SceneBuilder");
        comp.source = SlideComposition::Source::SlideBlocks;
        comp.blocks.clear();
        comp.presetKey.clear();
        comp.preset = StyleBuilder::Layout{};
    };

    const std::vector<ContentBlock>& selectedFamilyTemplate = categoryTemplate
        ? categoryTemplate->blocks
        : (comp.familyIdx < 4 ? style.familyTemplateBlocks[comp.familyIdx]
                              : style.familyTemplateBlocks[kFamilySlotShows]);
    if (slideHasContent && comp.familyIdx < 4
        && !selectedFamilyTemplate.empty()) {
        comp.source = SlideComposition::Source::FamilyTemplate;
        if (slideIsShowsContent && !slide.blocks.empty()) {
            std::vector<ContentBlock> categoryBlocks = selectedFamilyTemplate;
            std::vector<bool> used(slide.blocks.size(), false);
            for (ContentBlock& box : categoryBlocks) {
                if (box.kind != "text")
                    continue;
                auto source = slide.blocks.end();
                if (!box.id.empty()) {
                    source = std::find_if(slide.blocks.begin(), slide.blocks.end(),
                        [&](const ContentBlock& candidate) {
                            const size_t index = static_cast<size_t>(&candidate - slide.blocks.data());
                            return !used[index] && candidate.kind == "text"
                                && candidate.id == box.id
                                && !candidate.text.empty();
                        });
                }
                if (source == slide.blocks.end()) {
                    source = std::find_if(slide.blocks.begin(), slide.blocks.end(),
                        [&](const ContentBlock& candidate) {
                            const size_t index = static_cast<size_t>(&candidate - slide.blocks.data());
                            return !used[index] && candidate.kind == "text"
                                && !candidate.text.empty();
                        });
                }
                if (source == slide.blocks.end())
                    continue;
                used[static_cast<size_t>(std::distance(slide.blocks.begin(), source))] = true;
                box.text = source->text;
                box.bind.clear();
            }
            comp.blocks = BindTemplateBlocks(categoryBlocks, slide);
        } else {
            comp.blocks = BindTemplateBlocks(selectedFamilyTemplate, slide);
        }
        fallBackToSlideBlocks(
            CompositionPaints(comp.blocks),
            std::format("family {} template '{}'", comp.familyIdx,
                        categoryTemplate ? categoryTemplate->templateKey
                                         : style.familyTemplateKeys[comp.familyIdx]));
        return comp;
    }
    if (slideHasContent && !style.templateBlocks.empty()
        && (style.contentType.empty() || comp.contentType.empty()
            || comp.contentType == style.contentType)) {
        comp.source = SlideComposition::Source::StyleTemplate;
        comp.blocks = BindTemplateBlocks(style.templateBlocks, slide);
        fallBackToSlideBlocks(CompositionPaints(comp.blocks),
                              std::format("template '{}'", style.templateKey));
        return comp;
    }
    if (comp.familyIdx < 4 && !style.familyTemplateKeys[comp.familyIdx].empty()
        && style.familyTemplateBlocks[comp.familyIdx].empty()) {
        comp.source = SlideComposition::Source::FamilyPreset;
        comp.presetKey = style.familyTemplateKeys[comp.familyIdx];
        comp.preset = StyleBuilder::LayoutFor(comp.presetKey);
        fallBackToSlideBlocks(PresetHasContent(slide),
                              std::format("family {} preset '{}'", comp.familyIdx,
                                          comp.presetKey));
        return comp;
    }
    if (!slide.blocks.empty()) {
        comp.source = SlideComposition::Source::SlideBlocks;
        return comp;   // `blocks` stays empty: the caller draws the slide's own
    }
    comp.source = SlideComposition::Source::StylePreset;
    comp.presetKey = style.templateKey;
    comp.preset = StyleBuilder::LayoutFor(comp.presetKey);
    fallBackToSlideBlocks(PresetHasContent(slide),
                          std::format("preset '{}'", comp.presetKey));
    return comp;
}

// ---------------------------------------------------------------------------
// StyleBuilder — layout presets + colour parsing
// ---------------------------------------------------------------------------
StyleBuilder::Layout StyleBuilder::LayoutFor(std::string_view templateKey) {
    if (templateKey == "lowerThird")
        return Layout{{0.060f, 0.760f, 0.880f, 0.160f},
                      {0.060f, 0.830f, 0.880f, 0.110f},
                      false};
    if (templateKey == "title")
        return Layout{{0.060f, 0.300f, 0.880f, 0.180f},
                      {0.060f, 0.520f, 0.880f, 0.100f},
                      true};
    if (templateKey == "sidebar")
        return Layout{{0.040f, 0.080f, 0.300f, 0.120f},
                      {0.040f, 0.240f, 0.300f, 0.680f},
                      false};
    if (templateKey == "bottomBar")
        return Layout{{0.060f, 0.840f, 0.880f, 0.100f},
                      {0.060f, 0.860f, 0.880f, 0.080f},
                      true};
    // "fullscreen" and everything unknown (engine design ids, ""): the whole
    // stage — the safe default a style with no known preset falls back to.
    return Layout{{0.060f, 0.100f, 0.880f, 0.140f},
                  {0.060f, 0.300f, 0.880f, 0.600f},
                  true};
}
} // namespace bps::presentation