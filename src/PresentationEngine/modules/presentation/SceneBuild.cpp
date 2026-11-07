#include "modules/presentation/SceneBuilder.hpp"

#include "modules/presentation/BlockMeta.hpp"
#include "modules/library/DesignLibrary.hpp"
#include "modules/presentation/MediaPixels.hpp"
#include "modules/presentation/ScenePaint.hpp"
#include "modules/presentation/SlideBinding.hpp"
#include "modules/presentation/SlideRules.hpp"

#include "core/logging/Logger.hpp"
#include "modules/presentation/CompositorState.hpp"
#include "modules/presentation/PresentationTemplates.hpp"
#include "modules/presentation/TextFormat.hpp"
#include "modules/rendering/RenderEngine.hpp"
#include "modules/rendering/RenderObject.hpp"
#include "modules/rendering/Scene.hpp"
#include "modules/rendering/TextEngine.hpp"

#include <algorithm>
#include <format>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace bps::presentation {
// This output's line breaks for the slide's passage, if the service cut them (see Slide::flowBreaks).
static const std::vector<int>* ReadingBreaksFor(const Slide& slide, const OutputStyleSpec& style) {
    if (style.readingStyleKey.empty())
        return nullptr;
    const auto it = slide.flowBreaks.find(style.readingStyleKey);
    return it == slide.flowBreaks.end() ? nullptr : &it->second;
}


static std::vector<int> FlowCharStartsFor(const std::string& flowText,
                                          const rendering::TextStyle& style,
                                          const rendering::FontManager& fonts)
{
    return rendering::TextLayout::FlowCharacterStarts(flowText, style, fonts);
}

void ApplyReadingFlow(rendering::RenderEngine& engine, const std::string& sceneId,
                      const Slide& slide) {
    if (slide.flowText.empty())
        return;
    rendering::TextObject* reading = nullptr;
    size_t longest = 0;
    for (auto* obj : engine.CollectObjects(sceneId)) {
        auto* text = dynamic_cast<rendering::TextObject*>(obj);
        if (!text || text->HasReadingFlow() || text->Text().empty())
            continue;
        if (text->Text().size() > longest) {
            longest = text->Text().size();
            reading = text;
        }
    }
    if (!reading)
        return;
    {
        static std::mutex pickMutex;
        static std::map<std::string, std::pair<float, float>> lastPick;
        std::lock_guard<std::mutex> lock(pickMutex);
        const auto box = reading->Bounds();
        auto &prev = lastPick[sceneId];
        if (prev.first != box.width || prev.second != box.height) {
            prev = { box.width, box.height };
            Logger::Instance().Info(std::format(
                "reading block picked: box={:.2f}x{:.2f} at ({:.2f},{:.2f}) textLen={}",
                box.width, box.height, box.x, box.y, reading->Text().size()),
                "SceneBuilder");
        }
    }
    if (slide.flowBreaks.empty() && slide.flowText.rfind(reading->Text(), 0) != 0) {
        static std::mutex gateMutex;
        static std::set<std::string> logged;
        std::lock_guard<std::mutex> lock(gateMutex);
        if (logged.insert(sceneId).second)
            Logger::Instance().Debug(std::format(
                "reading flow skipped: the passage does not begin with this slide's "
                "reading text ({} chars of page, {} of flow) — the outputs render the page",
                reading->Text().size(), slide.flowText.size()), "SceneBuilder");
        return;
    }
    const auto* styleComp = reading->FindComponent<rendering::TextStyleComponent>();
    const rendering::TextStyle style = styleComp ? styleComp->style : rendering::TextStyle{};
    const auto pageLayout = rendering::TextLayout::Measure(reading->Text(), style, engine.Fonts());
    const int pageLines = static_cast<int>(pageLayout.lines.size());
    reading->SetFlowCharTable(FlowCharStartsFor(slide.flowText, style, engine.Fonts()));
    reading->SetReadingFlow(pageLines, slide.flowChar < 0.0f ? 0.0f : slide.flowChar);
    reading->SetText(slide.flowText);
    {
        static std::mutex flowLogMutex;
        static std::set<std::string> flowLogged;
        std::lock_guard<std::mutex> lock(flowLogMutex);
        if (flowLogged.insert(sceneId).second) {
            Logger::Instance().Debug(std::format(
                "reading flow attached: page {} line(s), passage {} char(s), resting at char {:.2f}",
                pageLines, slide.flowText.size(), slide.flowChar), "SceneBuilder");
            const bool shared = style.forcedBreaks && !style.forcedBreaks->starts.empty()
                && style.forcedBreaks->textHash == rendering::TextLayout::TextHash(slide.flowText);
            Logger::Instance().Info(std::format(
                "reading column lines: {} ({} line(s), box {:.0f}px wide at {:.0f}px)",
                shared ? "SHARED stage table" : "own wrap (no table for this passage yet)",
                shared ? style.forcedBreaks->starts.size()
                       : rendering::TextLayout::Measure(slide.flowText, style, engine.Fonts()).lines.size(),
                style.wrapWidth, style.size), "SceneBuilder");
        }
    }
}

bool SceneBuilder::RefreshReadingFlow(rendering::RenderEngine& engine,
                                      const std::string& sceneId, const Slide& slide) {
    rendering::TextObject* reading = nullptr;
    for (auto* obj : engine.CollectObjects(sceneId)) {
        auto* text = dynamic_cast<rendering::TextObject*>(obj);
        if (text && text->HasReadingFlow()) {
            reading = text;
            break;
        }
    }
    if (slide.flowText.empty()) {
        if (!reading)
            return false;
        reading->ClearReadingFlow();
        return true;
    }
    if (!reading) {
        ApplyReadingFlow(engine, sceneId, slide);
        for (auto* obj : engine.CollectObjects(sceneId)) {
            auto* text = dynamic_cast<rendering::TextObject*>(obj);
            if (text && text->HasReadingFlow()) {
                reading = text;
                break;
            }
        }
        if (!reading)
            return false;   // this scene draws no flow (a refused family, a bus feed)
    }
    auto* styleComp = reading->FindComponent<rendering::TextStyleComponent>();
    // THE LINE BREAKS FOLLOW THE SERVICE'S TABLE, NOT THE ONE THE SCENE WAS BUILT WITH. The service anchors every
    // output's table at the word the walk has reached (so each screen's top line begins on that word) and hands
    // the new starts with the position; the scene keeps its text, so without this it would go on drawing
    // the table it was built with and the top words would differ from screen to screen again.
    if (styleComp && !reading->ReadingStyleKey().empty()) {
        const auto breaks = slide.flowBreaks.find(reading->ReadingStyleKey());
        if (breaks != slide.flowBreaks.end() && !breaks->second.empty()) {
            auto forced = std::make_shared<rendering::ForcedLineBreaks>();
            forced->starts = breaks->second;
            forced->textHash = rendering::TextLayout::TextHash(slide.flowText);
            styleComp->style.forcedBreaks = std::move(forced);
        }
    }
    const rendering::TextStyle style = styleComp ? styleComp->style : rendering::TextStyle{};
    reading->SetFlowCharTable(FlowCharStartsFor(slide.flowText, style, engine.Fonts()));
    reading->SetReadingFlow(reading->FlowPageLines(), slide.flowChar < 0.0f ? 0.0f : slide.flowChar);
    reading->SetText(slide.flowText);
    return true;
}

void AddPresetTextObjects(rendering::RenderEngine& engine, const std::string& sceneId,
                          const rendering::Size& size, const StyleBuilder::Layout& layout,
                          const Slide& slide) {
    const rendering::Rect titleRect = Scaled(layout.title, size);
    const rendering::Rect bodyRect = Scaled(layout.body, size);
    if (!slide.title.empty() && layout.showTitle) {
        auto title = std::make_shared<rendering::TextObject>("title", "Title", slide.title);
        title->SetBounds(titleRect);
        title->SetLayer("text");
        (void)engine.AddObject(sceneId, title, "text");
    }
    if (!slide.text.empty()) {
        if (SlideRules::ContentType(slide) == "table") {
            constexpr double stageWidth = bps::library::kStageWidth;
            constexpr double stageHeight = bps::library::kStageHeight;
            ContentBlock body;
            body.kind = "text";
            body.x = layout.body.x * stageWidth;
            body.y = layout.body.y * stageHeight;
            body.width = layout.body.w * stageWidth;
            body.height = layout.body.h * stageHeight;
            body.text = slide.text;
            body.metaJson = R"({"fontSize":16,"autoSize":"shrink"})";
            AddStageBlocks(engine, sceneId, {body}, size, "preset-body");
            return;
        }
        auto body = std::make_shared<rendering::TextObject>("body", "Body", slide.text);
        body->SetBounds(bodyRect);
        body->SetLayer("text");
        (void)engine.AddObject(sceneId, body, "text");
    }
}

Result<std::string> SceneBuilder::BuildSlideScene(const Presentation& presentation,
                                                  const Slide& slide,
                                                  rendering::RenderEngine& engine,
                                                  rendering::Size size) {
    return BuildSlideScene(presentation, slide, OutputStyleSpec{}, engine, size);
}

Result<std::string> SceneBuilder::BuildGatedSlideScene(const Presentation& presentation,
                                                       const Slide& slide,
                                                       const OutputStyleSpec& style,
                                                       rendering::RenderEngine& engine,
                                                       rendering::Size size) {
    if (SlideRules::StyleAdmits(style, slide))
        return BuildSlideScene(presentation, slide, style, engine, size);

    const std::string sceneId =
        SceneIdFor(presentation, slide)
        + "@gated" + StyledSceneIdFor(presentation, slide, style, size).substr(SceneIdFor(presentation, slide).size());

    if (engine.GetScene(sceneId).ok()) {
        // A CLEAR CARRIER OWNS NO COMPOSITOR LAYERS (see ComposeSlide): the
        // blackout is "nothing on screen", the feed included.
        if (!slide.clearStage) {
            AddCompositorLayers(engine, sceneId, size, style.showOverlay);   // same cache-hit refresh as BuildSlideScene
            RefreshBusPlanes(engine, sceneId);   // ...and the bus windows' frames (none recorded for a refused scene)
        }
        return sceneId;
    }
    auto scene = engine.CreateScene(sceneId, slide.title, size);
    if (!scene.ok()) return scene.error();
    (void)engine.AddLayer(sceneId, rendering::Layer("bg", "Background",
                                                    rendering::LayerKind::Background, 0));
    (void)engine.AddLayer(sceneId, rendering::Layer("media", "Media",
                                                    rendering::LayerKind::Video, 1));
    (void)engine.AddLayer(sceneId, rendering::Layer("overlay", "Overlay",
                                                    rendering::LayerKind::Overlay, 3));

    const SlideComposition comp = SceneBuilder::ComposeSlide(slide, style, /*applyGates=*/true);
    const bool paintBackground = comp.paintBackground;
    // paintBackground implies a NAMED colour (see ComposeSlide), so the default
    // below is unreachable belt-and-braces rather than a paint of its own.
    const rendering::Color bg = comp.background.empty()
        ? rendering::Color(0.0f, 0.0f, 0.0f, 1.0f)
        : StyleBuilder::ParseColor(comp.background);
    if (paintBackground) {
        auto bgObj = std::make_shared<rendering::BackgroundObject>("bg", "Background", bg);
        bgObj->SetBounds(rendering::Rect(0, 0, size.width, size.height));
        bgObj->SetLayer("bg");
        (void)engine.AddObject(sceneId, bgObj, "bg");
    } else {
        scene.value()->SetTransparentBackground(true);
    }
    // Same clearBackgroundOnText gate as BuildSlideScene — see its comment.
    if (!style.backgroundImage.empty() && !comp.styleImageSuppressed) {
        const rendering::RgbaImage img = LoadImageCached(style.backgroundImage);
        if (!img.empty()) {
            auto imageObj = std::make_shared<rendering::ImageObject>("stylebg", "StyleBackground");
            imageObj->SetImage(img, "stylebg:" + style.backgroundImage);
            imageObj->SetBounds(CoverRect(img.width, img.height, size));
            imageObj->SetLayer("bg");
            (void)engine.AddObject(sceneId, imageObj, "bg");
        }
    }
    if (!slide.clearStage)
        AddCompositorLayers(engine, sceneId, size, style.showOverlay);
    Logger::Instance().Debug(std::format("Gated scene built: {} (family refused by style '{}')",
                                        sceneId, style.name), "SceneBuilder");
    return sceneId;
}

// ---------------------------------------------------------------------------
// A WIRED VIDEO BUS AS AN OUTPUT'S OWN PICTURE (Settings · Routing)
// ---------------------------------------------------------------------------
Result<std::string> SceneBuilder::BuildBusFeedScene(int busId, rendering::RenderEngine& engine,
                                                   rendering::Size size) {
    if (busId <= 0)
        return Error::Make(Err::InvalidArgument, "SceneBuilder",
                           "a bus feed needs the bus's own number");

    const int sw = static_cast<int>(size.width);
    const int sh = static_cast<int>(size.height);
    // The id carries BOTH the bus and the size (see the header): an output at
    // 720p and one at 4K never share a scene, and a resolution edit re-keys.
    const std::string sceneId = std::format("busfeed:{}@{}x{}", busId, sw, sh);
    if (!engine.GetScene(sceneId).ok()) {
        auto scene = engine.CreateScene(sceneId, std::format("Bus {}", busId), size);
        if (!scene.ok())
            return scene.error();
        // The video layer the picture is painted on ("media" — the layer the
        // compositor's own live layers use), declared once with the scene.
        (void)engine.AddLayer(sceneId, rendering::Layer("media", "Bus feed",
                                                        rendering::LayerKind::Video, 1));
    }

    const auto& compositor = presentation::CompositorState::Instance();
    const uint64_t seq = compositor.GetBusFrameSeq(busId);
    const std::string objId = std::format("busfeed:{}", busId);
    if (!LayerFrameCurrent(engine, sceneId, objId, seq)) {
        const auto frame = compositor.GetBusFrame(busId);
        const rendering::RgbaImage img = ToRgbaImage(frame.width, frame.height, frame.rgba);
        // Full-frame at THIS scene's size: the bus's own aspect is scaled by the
        // rasterizer the same way every other video layer is.
        UpsertVideoLayer(engine, sceneId, objId, img,
                         rendering::Rect(0, 0, size.width, size.height), seq);
    }
    ReportLayerState(sceneId, "bus feed",
                     std::format("bus {} {}", busId, seq == 0 ? "has no frame" : "live"));
    return sceneId;
}

// ---------------------------------------------------------------------------
// A TEMPLATE BUS AS AN OUTPUT'S OWN PICTURE
// ---------------------------------------------------------------------------
Result<std::string> SceneBuilder::BuildTemplateBusScene(int busNumber,
                                                        const std::vector<ContentBlock>& blocks,
                                                        const std::string& background,
                                                        rendering::RenderEngine& engine,
                                                        rendering::Size size) {
    if (busNumber <= 0)
        return Error::Make(Err::InvalidArgument, "SceneBuilder",
                           "a template bus scene needs the bus's own number");

    const int sw = static_cast<int>(size.width);
    const int sh = static_cast<int>(size.height);
    uint64_t layout = 1469598103934665603ull;   // FNV offset basis
    const auto mix = [&layout](std::string_view sv) {
        for (const char c : sv) {
            layout ^= static_cast<unsigned char>(c);
            layout *= 1099511628211ull;   // FNV prime
        }
        layout ^= 0x1f;   // separator, so "ab"+"c" never equals "a"+"bc"
        layout *= 1099511628211ull;
    };
    const auto mixCoord = [&layout](double v) {
        layout ^= static_cast<uint64_t>(static_cast<int64_t>(v * 1000.0));
        layout *= 1099511628211ull;
    };
    for (const ContentBlock& b : blocks) {
        mix(b.id);
        mix(b.kind);
        mix(b.metaJson);
        mixCoord(b.x);
        mixCoord(b.y);
        mixCoord(b.width);
        mixCoord(b.height);
    }
    mix(background);
    const std::string sceneId =
        std::format("tplbus:{}@{}x{}#{:x}", busNumber, sw, sh, layout);
    if (!engine.GetScene(sceneId).ok()) {
        auto scene = engine.CreateScene(sceneId, std::format("Template bus {}", busNumber), size);
        if (!scene.ok())
            return scene.error();
        (void)engine.AddLayer(sceneId, rendering::Layer("bg", "Background",
                                                        rendering::LayerKind::Background, 0));
        (void)engine.AddLayer(sceneId, rendering::Layer("media", "Media",
                                                        rendering::LayerKind::Video, 1));
        (void)engine.AddLayer(sceneId, rendering::Layer("text", "Text",
                                                        rendering::LayerKind::Text, 2));
        // The template's own background: named = painted, ""/transparent = a
        // keyable frame (the same reading ComposeSlide gives a style's colour).
        if (!background.empty() && background != "transparent") {
            auto bgObj = std::make_shared<rendering::BackgroundObject>(
                "bg", "Background", StyleBuilder::ParseColor(background));
            bgObj->SetBounds(rendering::Rect(0, 0, size.width, size.height));
            bgObj->SetLayer("bg");
            (void)engine.AddObject(sceneId, bgObj, "bg");
        } else {
            scene.value()->SetTransparentBackground(true);
        }
        (void)AddStageBlocks(engine, sceneId, blocks, size, "tpw", "text");
        RecordTemplateWindows(sceneId, blocks, size, busNumber);
        Logger::Instance().Debug(
            std::format("Template bus scene built: {} ({} block(s))", sceneId, blocks.size()),
            "SceneBuilder");
    }
    RefreshBusPlanes(engine, sceneId);
    return sceneId;
}

// ---------------------------------------------------------------------------
Result<std::string> SceneBuilder::BuildSlideScene(const Presentation& presentation,
                                                  const Slide& slide,
                                                  const OutputStyleSpec& style,
                                                  rendering::RenderEngine& engine,
                                                  rendering::Size size) {
    const std::string sceneId = StyledSceneIdFor(presentation, slide, style, size);

    if (engine.GetScene(sceneId).ok()) {
        if (!slide.clearStage) {
            AddCompositorLayers(engine, sceneId, size, style.showOverlay);
            RefreshBusPlanes(engine, sceneId);   // bus windows keep flowing on a cache hit (see BusPlane)
        }
        return sceneId;
    }

    auto scene = engine.CreateScene(sceneId, slide.title, size);
    if (!scene.ok()) return scene.error();

    (void)engine.AddLayer(sceneId, rendering::Layer("bg", "Background",
                                                    rendering::LayerKind::Background, 0));
    (void)engine.AddLayer(sceneId, rendering::Layer("media", "Media",
                                                    rendering::LayerKind::Video, 1));
    (void)engine.AddLayer(sceneId, rendering::Layer("text", "Text",
                                                    rendering::LayerKind::Text, 2));
    (void)engine.AddLayer(sceneId, rendering::Layer("overlay", "Overlay",
                                                    rendering::LayerKind::Overlay, 3));

    // ---- Background ------------------------------------------------------
    const SlideComposition comp = SceneBuilder::ComposeSlide(slide, style);
    const bool slideOwnsBackground = comp.slideOwnsBackground;
    // paintBackground implies a NAMED colour (see ComposeSlide), so the default
    // below is unreachable belt-and-braces rather than a paint of its own.
    const rendering::Color bg = comp.background.empty()
        ? rendering::Color(0.0f, 0.0f, 0.0f, 1.0f)
        : StyleBuilder::ParseColor(comp.background);
    const bool paintBackground = comp.paintBackground;

    if (const char* diag = std::getenv("VGR_SCENEDIAG"); diag && *diag) {
        const bool imgBlocked = comp.styleImageSuppressed;
        const rendering::RgbaImage diagImg = (style.backgroundImage.empty() || imgBlocked)
            ? rendering::RgbaImage{} : LoadImageCached(style.backgroundImage);
        Logger::Instance().Info(
            std::format("SCENEDIAG style='{}' styleBg='{}' clearOnText={} slideOwnsBg={} "
                        "slideBg='{}' paint={} effective=({:.3f},{:.3f},{:.3f},{:.3f}) "
                        "imgSet={} imgBlocked={} img={}x{}",
                        style.name, style.backgroundColor, style.clearBackgroundOnText,
                        slideOwnsBackground, slide.background, paintBackground,
                        bg.r, bg.g, bg.b, bg.a,
                        !style.backgroundImage.empty(), imgBlocked,
                        diagImg.width, diagImg.height),
            "SceneBuilder");
    }
    if (paintBackground) {
        auto bgObj = std::make_shared<rendering::BackgroundObject>(
            "bg", "Background", bg);
        bgObj->SetBounds(rendering::Rect(0, 0, size.width, size.height));
        bgObj->SetLayer("bg");
        (void)engine.AddObject(sceneId, bgObj, "bg");
    } else {
        scene.value()->SetTransparentBackground(true);
    }

    if (!style.backgroundImage.empty() && !comp.styleImageSuppressed) {
        const rendering::RgbaImage img = LoadImageCached(style.backgroundImage);
        if (!img.empty()) {
            auto imageObj = std::make_shared<rendering::ImageObject>("stylebg", "StyleBackground");
            imageObj->SetImage(img, "stylebg:" + style.backgroundImage);
            imageObj->SetBounds(CoverRect(img.width, img.height, size));
            imageObj->SetLayer("bg");
            (void)engine.AddObject(sceneId, imageObj, "bg");
        } else {
            Logger::Instance().Warning(
                std::format("style background image unreadable: {}", style.backgroundImage),
                "SceneBuilder");
        }
    }

    if (comp.source != SlideComposition::Source::Cleared) {
        AddCompositorLayers(engine, sceneId, size, style.showOverlay);

        RecordBusPlanes(sceneId,
                        comp.source == SlideComposition::Source::SlideBlocks ? slide.blocks
                                                                            : comp.blocks,
                        size, &style);
        RefreshBusPlanes(engine, sceneId);
    }

    // ---- Content blocks ---------------------------------------------------
    // ---- Content blocks ---------------------------------------------------
    // ---- Content: WHICH LAYOUT OWNS THIS SLIDE ---------------------------
    auto addContent = [&]() {
        switch (comp.source) {
        case SlideComposition::Source::FamilyTemplate: {
            const int n = AddStageBlocks(engine, sceneId, comp.blocks, size, "tplf", "text", slide.flowText,
                                         style.readingFontPx, ReadingBreaksFor(slide, style),
                                         rendering::TextLayout::TextHash(slide.flowText), !slide.flowBreaks.empty(),
                                         style.readingStyleKey);
            Logger::Instance().Debug(std::format("Scene built: {} (style '{}', family {} template {} blocks)",
                                                sceneId, style.name, comp.familyIdx, n), "SceneBuilder");
            return;
        }
        case SlideComposition::Source::StyleTemplate: {
            const int n = AddStageBlocks(engine, sceneId, comp.blocks, size, "tpl", "text", slide.flowText,
                                         style.readingFontPx, ReadingBreaksFor(slide, style),
                                         rendering::TextLayout::TextHash(slide.flowText), !slide.flowBreaks.empty(),
                                         style.readingStyleKey);
            Logger::Instance().Debug(std::format("Scene built: {} (style '{}', template {} blocks)",
                                                sceneId, style.name, n), "SceneBuilder");
            return;
        }
        case SlideComposition::Source::FamilyPreset: {
            AddPresetTextObjects(engine, sceneId, size, comp.preset, slide);
            Logger::Instance().Debug(std::format("Scene built: {} (style '{}', family {} preset '{}')",
                                                sceneId, style.name, comp.familyIdx, comp.presetKey),
                                     "SceneBuilder");
            return;
        }
        case SlideComposition::Source::SlideBlocks: {
            const int n = AddStageBlocks(engine, sceneId, slide.blocks, size, "blk", "text", slide.flowText,
                                         style.readingFontPx, ReadingBreaksFor(slide, style),
                                         rendering::TextLayout::TextHash(slide.flowText), !slide.flowBreaks.empty(),
                                         style.readingStyleKey);
            Logger::Instance().Debug(std::format("Scene built: {} (style '{}', {} blocks)",
                                                sceneId, style.name, n), "SceneBuilder");
            return;
        }
        case SlideComposition::Source::Cleared:
            Logger::Instance().Debug(std::format("Scene built: {} (clear — the frame paints nothing)", sceneId),
                                     "SceneBuilder");
            return;
        case SlideComposition::Source::StylePreset:
            break;   // the style's own built-in layout: the two text regions below
        case SlideComposition::Source::Refused:
            break;
        }

        // ---- The style's own layout preset ------------------------------------
        AddPresetTextObjects(engine, sceneId, size, comp.preset, slide);
        Logger::Instance().Debug(std::format("Scene built: {} (style '{}')", sceneId, style.name),
                                 "SceneBuilder");
    };
    addContent();
    ApplyReadingFlow(engine, sceneId, slide);
    return sceneId;
}
} // namespace bps::presentation