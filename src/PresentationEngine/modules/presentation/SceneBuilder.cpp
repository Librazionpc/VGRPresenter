#include "modules/presentation/SceneBuilder.hpp"

#include "core/logging/Logger.hpp"
#include "modules/rendering/RenderEngine.hpp"
#include "modules/rendering/RenderObject.hpp"
#include "modules/rendering/Scene.hpp"
#include "modules/rendering/TextEngine.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <fstream>
#include <format>
#include <map>
#include <mutex>

#include "modules/media/FrameSource.hpp"
#include "modules/rendering/PngCodec.hpp"

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

// ---- block metaJson: the design Text() helper's colour/size/align fields ----
// (tiny inline JSON probe — pulling core/config/Json.hpp in here for five
// flat fields is heavier than the hand roll; the writer is std::format with
// fixed key order, quoted string values, bare numbers/bools.)
std::string MetaString(std::string_view json, std::string_view key) {
    const std::string needle = std::format("\"{}\":\"", key);
    const size_t at = json.find(needle);
    if (at == std::string_view::npos) return {};
    const size_t start = at + needle.size();
    const size_t end = json.find('"', start);
    if (end == std::string_view::npos) return {};
    return std::string(json.substr(start, end - start));
}
double MetaNumber(std::string_view json, std::string_view key, double dflt) {
    const std::string needle = std::format("\"{}\":", key);
    const size_t at = json.find(needle);
    if (at == std::string_view::npos) return dflt;
    const size_t start = at + needle.size();
    const size_t end = json.find_first_of(",}", start);
    if (end == std::string_view::npos) return dflt;
    const std::string num(json.substr(start, end - start));
    double out = dflt;
    const char* b = num.c_str();
    char* e = nullptr;
    out = std::strtod(b, &e);
    return e == b ? dflt : out;
}
bool MetaBool(std::string_view json, std::string_view key) {
    return json.find(std::format("\"{}\":true", key)) != std::string_view::npos;
}

rendering::TextAlign ParseAlign(std::string_view a) {
    if (a == "center" || a == "middle") return rendering::TextAlign::Center;
    if (a == "right") return rendering::TextAlign::Right;
    return rendering::TextAlign::Left;
}
rendering::TextVAlign ParseVAlign(std::string_view a) {
    if (a == "center" || a == "middle") return rendering::TextVAlign::Middle;
    if (a == "bottom") return rendering::TextVAlign::Bottom;
    return rendering::TextVAlign::Top;
}

// ---- style background image cache -----------------------------------------
// The image is fingerprinted into the scene id, so every rebuild of a style
// with a different image builds a different scene — but ONE image file would
// still be read+decoded once per slide per rebuild without this cache. Keyed
// by path; a read failure returns an empty image (the caller paints the colour
// underneath instead).
std::mutex g_imageMutex;
std::map<std::string, rendering::RgbaImage> g_imageCache;

rendering::RgbaImage LoadImageCached(const std::string& path) {
    std::lock_guard<std::mutex> lock(g_imageMutex);
    auto it = g_imageCache.find(path);
    if (it != g_imageCache.end())
        return it->second;
    g_imageCache[path] = rendering::RgbaImage{};   // reserve the key
    rendering::RgbaImage& slot = g_imageCache[path];

    std::ifstream file(path, std::ios::binary);
    if (!file) return slot;
    std::vector<unsigned char> bytes((std::istreambuf_iterator<char>(file)),
                                     std::istreambuf_iterator<char>());
    if (bytes.size() >= 8 && bytes[0] == 0x89 && bytes[1] == 'P') {
        auto decoded = rendering::DecodePng(bytes.data(), bytes.size());
        if (decoded.ok()) {
            slot.width = decoded.value().width;
            slot.height = decoded.value().height;
            slot.pixels = std::move(decoded.value().pixels);
        }
        return slot;
    }
    // Non-PNG (jpg/webp/...): the media module's platform frame source decodes
    // them (Media Foundation + shell on Windows).
    auto pic = bps::media::MakePlatformFrameSource()->Grab(
        path, bps::media::MediaKind::Image, 0.0, 1920);
    if (pic.ok() && !pic.value().empty()) {
        slot.width = pic.value().width;
        slot.height = pic.value().height;
        slot.pixels.resize(static_cast<size_t>(pic.value().width) * pic.value().height);
        for (size_t i = 0; i < slot.pixels.size(); ++i) {
            const size_t o = i * 4;
            slot.pixels[i] = rendering::Color(pic.value().rgba[o] / 255.0f,
                                              pic.value().rgba[o + 1] / 255.0f,
                                              pic.value().rgba[o + 2] / 255.0f,
                                              pic.value().rgba[o + 3] / 255.0f).Pack();
        }
    }
    return slot;
}

// Cover-fit math: the image fills `stage` entirely, cropped centered (FreeShow
// draws style backgrounds cover by default).
rendering::Rect CoverRect(int imgW, int imgH, const rendering::Size& stage) {
    if (imgW <= 0 || imgH <= 0)
        return rendering::Rect(0, 0, static_cast<float>(stage.width), static_cast<float>(stage.height));
    const float scale = std::max(static_cast<float>(stage.width) / imgW,
                                 static_cast<float>(stage.height) / imgH);
    const float w = imgW * scale, h = imgH * scale;
    return rendering::Rect((stage.width - w) / 2.0f, (stage.height - h) / 2.0f, w, h);
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
    // layout/background/image/clear pair (name, contentType) are diagnostics
    // only — not fingerprinted.
    return SceneIdFor(p, s) + std::format("@s:{}_{}_{}_{}",
                                          style.backgroundColor,
                                          style.templateKey,
                                          style.clearBackgroundOnText ? 1 : 0,
                                          style.backgroundImage);
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
    // FreeShow's Output.svelte composition: the style's background (IMAGE
    // first, colour beneath it) paints behind everything;
    // clearStyleBackgroundOnText lets a slide that carries its own colour keep
    // it. An EMPTY style background (spec carries none — ParseColor("")->a ==
    // 0) leaves the slide's own (or the default) in place.
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

    // Style background IMAGE — painted over the colour, under everything else
    // (FreeShow's style backgroundImage, cover-fit). Fingerprinted into the
    // scene id, so a new image always lands as a rebuild.
    if (!style.backgroundImage.empty()) {
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

    // ---- Content blocks ---------------------------------------------------
    // A slide with positioned blocks (the scripture/sermon/lyrics templates:
    // verse box, reference chip, name line...) renders THE BLOCKS — that is
    // the composition the preview shows and the only faithful on-air render.
    // Block geometry is stage-fraction-like: it lives on the 754×428 Edit
    // stage (DesignLibrary's kStageWidth/Height), scaled to the real stage.
    // A slide without blocks falls back to the style preset's title/body
    // layout (the plain text slides SceneBuilder has always rendered).
    if (!slide.blocks.empty()) {
        constexpr double kBlockStageW = 754.0, kBlockStageH = 428.0;
        int n = 0;
        for (const ContentBlock& block : slide.blocks) {
            ++n;
            const std::string objId = std::format("blk{}", n);
            if (block.kind == "text") {
                auto text = std::make_shared<rendering::TextObject>(objId, objId, block.text);
                text->SetBounds(rendering::Rect(
                    static_cast<float>(block.x / kBlockStageW * size.width),
                    static_cast<float>(block.y / kBlockStageH * size.height),
                    static_cast<float>(block.width / kBlockStageW * size.width),
                    static_cast<float>(block.height / kBlockStageH * size.height)));
                auto styleComp = std::make_shared<rendering::TextStyleComponent>();
                styleComp->style.size = static_cast<float>(
                    MetaNumber(block.metaJson, "fontSize", 24.0) * size.height / kBlockStageH);
                styleComp->style.color = StyleBuilder::ParseColor(
                    MetaString(block.metaJson, "color").empty()
                        ? "#ffffff" : MetaString(block.metaJson, "color"));
                styleComp->style.align = ParseAlign(MetaString(block.metaJson, "align"));
                styleComp->style.valign = ParseVAlign(MetaString(block.metaJson, "verticalAlign"));
                styleComp->style.bold = MetaBool(block.metaJson, "bold");
                styleComp->style.wrap = true;
                styleComp->style.wrapWidth = text->Bounds().width;
                text->AddComponent(styleComp);
                text->SetLayer("text");
                (void)engine.AddObject(sceneId, text, "text");
            } else if (block.kind == "box" || block.kind == "shape" || block.kind == "rectangle") {
                auto shape = std::make_shared<rendering::ShapeObject>(objId, objId);
                shape->SetRectangle(StyleBuilder::ParseColor(
                    block.style.backgroundColor.empty() ? "#66000000"
                                                        : block.style.backgroundColor));
                shape->SetBounds(rendering::Rect(
                    static_cast<float>(block.x / kBlockStageW * size.width),
                    static_cast<float>(block.y / kBlockStageH * size.height),
                    static_cast<float>(block.width / kBlockStageW * size.width),
                    static_cast<float>(block.height / kBlockStageH * size.height)));
                shape->SetLayer("text");
                (void)engine.AddObject(sceneId, shape, "text");
            }
            // Other block kinds (clock/timer/media...) render as the plain
            // engine defaults for now — not part of this pass.
        }
        Logger::Instance().Debug(std::format("Scene built: {} (style '{}', {} blocks)", sceneId, style.name, n),
                                 "SceneBuilder");
        return sceneId;
    }

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
