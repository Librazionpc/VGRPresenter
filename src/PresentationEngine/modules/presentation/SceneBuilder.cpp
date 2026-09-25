#include "modules/presentation/SceneBuilder.hpp"

#include "core/logging/Logger.hpp"
#include "modules/rendering/RenderEngine.hpp"
#include "modules/rendering/RenderObject.hpp"
#include "modules/rendering/Scene.hpp"
#include "modules/rendering/TextEngine.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <format>

namespace bps::presentation {

namespace {

// The 1920×1080 design grid LayoutFor() returns fractions OF. LayoutFor
// multiplies back onto the actual stage, so any output resolution renders
// the same composition.
constexpr float kDesignW = 1920.0f;
constexpr float kDesignH = 1080.0f;

rendering::Rect Scaled(const StyleBuilder::LayoutRect& r, const rendering::Size& stage) {
    return rendering::Rect(
        static_cast<int>(r.x * stage.width),
        static_cast<int>(r.y * stage.height),
        static_cast<int>(r.w * stage.width),
        static_cast<int>(r.h * stage.height));
}

bool IsHexColor(std::string_view s) {
    if (s.size() != 7 && s.size() != 9)
        return false;
    if (s[0] != '#')
        return false;
    return std::all_of(s.begin() + 1, s.end(), [](unsigned char c) {
        return std::isdigit(c) != 0 || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
    });
}

} // namespace

// ---------------------------------------------------------------------------
// StyleBuilder — layout presets + colour parsing
// ---------------------------------------------------------------------------
StyleBuilder::Layout StyleBuilder::LayoutFor(std::string_view templateKey) {
    // Fractions of the design grid (see kDesignW/kDesignH). The shapes follow
    // FreeShow's style compositions: a lower third hugs the bottom edge, a
    // sidebar owns the left column, fullscreen hands the stage to the text.
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

rendering::Color StyleBuilder::ParseColor(std::string_view css) {
    // "transparent" or anything unparseable → fully transparent; an EMPTY
    // input also lands here with a+0, which callers use to distinguish
    // "no colour given" from an explicit colour.
    if (!IsHexColor(css))
        return rendering::Color(0, 0, 0, 0);

    const auto channel = [&css](size_t offset) {
        uint8_t v = 255;
        // Two hex digits starting at `offset` (after '#').
        std::from_chars(css.data() + offset, css.data() + offset + 2, v, 16);
        return static_cast<float>(v) / 255.0f;
    };
    if (css.size() == 7)
        return rendering::Color(channel(1), channel(3), channel(5), 1.0f);
    return rendering::Color(channel(3), channel(5), channel(7), channel(1));
}

// ---------------------------------------------------------------------------
// SceneBuilder
// ---------------------------------------------------------------------------
std::string SceneBuilder::SceneIdFor(const Presentation& p, const Slide& s) {
    return "pres:" + p.id + ":" + s.id;
}

std::string SceneBuilder::StyledSceneIdFor(const Presentation& p, const Slide& s,
                                           const OutputStyleSpec& style) {
    // The fingerprint changes whenever a styled field does, so the render
    // cache can't serve a scene built under the old style. Fields beyond the
    // layout/background pair (name, contentType) are diagnostics only — not
    // fingerprinted.
    return SceneIdFor(p, s) + std::format("@s:{}_{}_{}",
                                          style.backgroundColor,
                                          style.templateKey,
                                          style.clearBackgroundOnText ? 1 : 0);
}

Result<std::string> SceneBuilder::BuildSlideScene(const Presentation& presentation,
                                                  const Slide& slide,
                                                  rendering::RenderEngine& engine,
                                                  rendering::Size size) {
    return BuildSlideScene(presentation, slide, OutputStyleSpec{}, engine, size);
}

Result<std::string> SceneBuilder::BuildSlideScene(const Presentation& presentation,
                                                  const Slide& slide,
                                                  const OutputStyleSpec& style,
                                                  rendering::RenderEngine& engine,
                                                  rendering::Size size) {
    const std::string sceneId = StyledSceneIdFor(presentation, slide, style);

    // Already built (cache hit) — return it.
    if (engine.GetScene(sceneId).ok()) return sceneId;

    auto scene = engine.CreateScene(sceneId, slide.title, size);
    if (!scene.ok()) return scene.error();

    // Layer order: background -> video -> image -> text -> overlay.
    (void)engine.AddLayer(sceneId, rendering::Layer("bg", "Background",
                                                    rendering::LayerKind::Background, 0));
    (void)engine.AddLayer(sceneId, rendering::Layer("media", "Media",
                                                    rendering::LayerKind::Video, 1));
    (void)engine.AddLayer(sceneId, rendering::Layer("text", "Text",
                                                    rendering::LayerKind::Text, 2));
    (void)engine.AddLayer(sceneId, rendering::Layer("overlay", "Overlay",
                                                    rendering::LayerKind::Overlay, 3));

    // ---- Background ------------------------------------------------------
    // FreeShow's Output.svelte composition: the style's background paints
    // behind everything; clearStyleBackgroundOnText lets a slide that carries
    // its own colour keep it. An EMPTY style background (spec carries none —
    // ParseColor("")->a == 0) leaves the slide's own (or the default) in place.
    rendering::Color bg = rendering::Color(0.06f, 0.07f, 0.09f, 1.0f);   // engine default
    bool slideOwnsBackground = !slide.background.empty() && slide.background != "transparent";
    rendering::Color styleBg = StyleBuilder::ParseColor(style.backgroundColor);
    if (styleBg.a > 0.0f && !(style.clearBackgroundOnText && slideOwnsBackground)) {
        bg = styleBg;
    } else if (slideOwnsBackground) {
        if (rendering::Color parsed = StyleBuilder::ParseColor(slide.background); parsed.a > 0.0f)
            bg = parsed;
    }

    auto bgObj = std::make_shared<rendering::BackgroundObject>(
        "bg", "Background", bg);
    bgObj->SetBounds(rendering::Rect(0, 0, size.width, size.height));
    bgObj->SetLayer("bg");
    (void)engine.AddObject(sceneId, bgObj, "bg");

    // ---- Text layout -------------------------------------------------------
    const StyleBuilder::Layout layout = StyleBuilder::LayoutFor(style.templateKey);
    const rendering::Rect titleRect = Scaled(layout.title, size);
    const rendering::Rect bodyRect = Scaled(layout.body, size);

    if (!slide.title.empty() && layout.showTitle) {
        auto title = std::make_shared<rendering::TextObject>("title", "Title", slide.title);
        title->SetBounds(titleRect);
        title->SetLayer("text");
        (void)engine.AddObject(sceneId, title, "text");
    }
    if (!slide.text.empty()) {
        auto body = std::make_shared<rendering::TextObject>("body", "Body", slide.text);
        body->SetBounds(bodyRect);
        body->SetLayer("text");
        (void)engine.AddObject(sceneId, body, "text");
    }

    Logger::Instance().Debug(std::format("Scene built: {} (style '{}')", sceneId, style.name),
                             "SceneBuilder");
    return sceneId;
}

} // namespace bps::presentation
