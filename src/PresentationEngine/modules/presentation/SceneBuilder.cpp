#include "modules/presentation/SceneBuilder.hpp"

#include "core/logging/Logger.hpp"
#include "core/config/Json.hpp"
#include "modules/presentation/CompositorState.hpp"
#include "modules/presentation/PresentationTemplates.hpp"
#include "modules/rendering/RenderEngine.hpp"
#include "modules/rendering/RenderObject.hpp"
#include "modules/rendering/Scene.hpp"
#include "modules/rendering/TextEngine.hpp"
#include "platform/IVideo.hpp"
#include "platform/PlatformAccessor.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstdint>
#include <cstring>
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

// ---- template-blocks fingerprint -----------------------------------------
// A stable 64-bit FNV-1a over the spec's templateBlocks — geometry, kind,
// bind, text, style and meta all feed it. Scene ids are strings, not numbers,
// so this rides the id as hex: the ONLY requirement is that two different
// block sets (an edited template landing mid-show) hash differently, which a
// position-sensitive mix gives far more reliably than concatenating raw
// geometry floats into the id.
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
        // Geometry at millimetre resolution. Through int64_t — a signed cast
        // is defined for the negative coordinates some shipped templates use
        // (off-screen panels); straight to uint64_t would be UB.
        const auto mixCoord = [&mix](double d) {
            mix(static_cast<uint64_t>(static_cast<int64_t>(d * 1000.0)));
        };
        mixCoord(b.x);
        mixCoord(b.y);
        mixCoord(b.width);
        mixCoord(b.height);
        mixStr(b.style.backgroundColor);
        mix(b.style.borderEnabled ? 1ull : 0ull);
    }
    return h;
}

// The slide's CONTENT text — what a bound "text" block shows. slide.text
// first; when empty (content tabs go live with the verse riding in their
// OWN template's blocks and only the reference in `title`), the first
// text-bearing slide block's text; else the title.
std::string SlideContentText(const Slide& slide) {
    if (!slide.text.empty())
        return slide.text;
    for (const ContentBlock& b : slide.blocks)
        if (b.kind == "text" && !b.text.empty() && b.text.find('{') != 0)
            return b.text;
    return slide.title;
}

// The content family the slide belongs to ("scripture", "table", …) — the
// frontend tags it in the slide's meta (ShowConverter). Empty = untagged =
// SHOWS content (imported shows, Quick Lyrics): it follows the shows family
// slot via FamilyTemplateIndexFor.
std::string slideContentType(const Slide& slide) {
    if (auto meta = json::Parse(slide.metaJson); meta.ok())
        if (const json::Value* v = meta.value().Find("contentType"); v && v->type() == json::Value::Type::String)
            return std::string(v->asString());
    return {};
}

// The OutputStyleSpec::familyTemplateKeys/blocks slot a content family maps
// to (shows | media | scripture | table, in the show* gates' order). Slides
// without a family tag are SHOWS content (imported and Quick-Lyrics slides
// carry no tag) — they follow the shows slot, so the Styles screen's shows
// pick (or its Bible fallback) styles them. Only an unknown tag returns 4,
// where a slide keeps its own layout.
size_t FamilyTemplateIndexFor(const std::string& contentType) {
    if (contentType == "media")     return 1;
    if (contentType == "scripture") return 2;
    if (contentType == "table")     return 3;
    return 0;   // "shows" — and untagged slides, which are shows content
}

// Fills every bound text block of a baked template from `slide` — the same
// contract SlideResolver uses for show templates, narrowed to the plain
// slides this path serves: "text" takes the slide's CONTENT (see
// SlideContentText — the verse, not the reference), "ref" takes the slide's
// title (its reference), and anything else goes through
// SlideResolver::BoundValue (meta field, "title", "notes"...). Unbound
// blocks keep their literal text — a template's decorative labels render as
// designed — UNLESS the literal is a leftover SCRIPTURE PLACEHOLDER
// ({scripture_name}, {scripture_reference}, …): plain slides carry no such
// values, so the raw placeholder would render as-is instead of an empty
// line.
std::vector<ContentBlock> BindTemplateBlocks(const std::vector<ContentBlock>& tmpl,
                                             const Slide& slide) {
    std::vector<ContentBlock> out = tmpl;
    for (ContentBlock& b : out) {
        if (b.kind != "text")
            continue;
        if (!b.bind.empty()) {
            std::string value = SlideResolver::BoundValue(slide, b.bind);
            if (value.empty()) {
                if (b.bind == "text")
                    value = SlideContentText(slide);
                else if (b.bind == "ref")
                    value = slide.title;
            }
            b.text = std::move(value);
        } else if (b.text.find('{') != std::string::npos) {
            b.text.clear();   // unfilled {scripture_*} on a plain slide → blank line
        }
    }
    return out;
}

// Renders positioned stage blocks (the 754×428 Edit-stage grid template
// designs and slides share) into a scene layer: text blocks become styled
// TextObjects, box/shape blocks ShapeObjects; other kinds (clock, timer,
// media...) are not part of this pass. `tag` prefixes object ids so a
// slide-block render and a template-block render can never collide within a
// scene. `layer` defaults to "text" (every pre-existing caller here targets
// the slide's own text layer); AddCompositorLayers below is the one caller
// that passes "media"/"overlay" instead, for an active overlay's own blocks
// (placeUnderSlide decides which). Returns the number of blocks seen (for
// the scene log line).
int AddStageBlocks(rendering::RenderEngine& engine, const std::string& sceneId,
                   const std::vector<ContentBlock>& blocks,
                   const rendering::Size& size, std::string_view tag,
                   std::string_view layer = "text") {
    constexpr double kBlockStageW = 754.0, kBlockStageH = 428.0;
    int n = 0;
    for (const ContentBlock& block : blocks) {
        ++n;
        const std::string objId = std::format("{}{}", tag, n);
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
            // fontId: the real system font TextEngine's GDI+ backend tries
            // first (RenderEngine::DrawTextObject / GlyphAtlas), degrading
            // to the builtin bitmap font only when it can't resolve — the
            // block's own fontFamily when the canvas set one, else the
            // app's own UI default so plain/legacy blocks with no family
            // still get real typography instead of reading as "no font
            // chosen, so no real font" and falling all the way back.
            {
                const std::string family = MetaString(block.metaJson, "fontFamily");
                styleComp->style.fontId = family.empty() ? "Segoe UI" : family;
            }
            styleComp->style.italic = MetaBool(block.metaJson, "italic");
            styleComp->style.wrap = true;
            styleComp->style.wrapWidth = text->Bounds().width;
            text->AddComponent(styleComp);
            text->SetLayer(std::string(layer));
            (void)engine.AddObject(sceneId, text, std::string(layer));
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
            shape->SetLayer(std::string(layer));
            (void)engine.AddObject(sceneId, shape, std::string(layer));
        }
    }
    return n;
}

// Raw RGBA8 bytes (row-major, top-down — CompositorState's and
// IVideo::DecodedFrame's shared convention) -> RgbaImage. A straight memcpy
// is correct here: RgbaImage::pixels is Color::Pack() format (0xAABBGGRR on
// a little-endian machine, i.e. bytes R,G,B,A in memory) — bit-for-bit the
// SAME layout as the RGBA8 bytes already are, the identical trick
// LivePreviewProvider::requestImage relies on in the other direction.
rendering::RgbaImage ToRgbaImage(uint32_t w, uint32_t h, const std::vector<uint8_t>& rgba) {
    rendering::RgbaImage img;
    if (w == 0 || h == 0 || rgba.size() < static_cast<size_t>(w) * h * 4)
        return img;
    img.width = static_cast<int>(w);
    img.height = static_cast<int>(h);
    img.pixels.resize(static_cast<size_t>(w) * h);
    std::memcpy(img.pixels.data(), rgba.data(), img.pixels.size() * sizeof(uint32_t));
    return img;
}

// Upserts a single VideoObject by a WELL-KNOWN fixed id ("takeninput" /
// "mediaonair" — exactly 0 or 1 instance of each ever exists): mutates the
// EXISTING object's frame in place when the scene is a cache hit (scenes
// are fingerprinted/cached by SLIDE+STYLE content — SceneBuilder's own
// cache, see BuildSlideScene's "already built" early return — so a
// continuously-updating video feed must refresh THROUGH that cache, not
// rebuild around it), otherwise creates it fresh. An empty image clears the
// existing object's frame (VideoObject::HasFrame() false → the rasterizer's
// `if (!v->HasFrame()) break;` already makes it invisible) rather than
// removing the node — cheaper, and this id is reused every time this layer
// has content again anyway.
void UpsertVideoLayer(rendering::RenderEngine& engine, const std::string& sceneId,
                      const std::string& objId, const rendering::RgbaImage& img,
                      const rendering::Rect& bounds) {
    auto existing = engine.GetObject(sceneId, objId);
    if (existing.ok()) {
        if (auto* v = dynamic_cast<rendering::VideoObject*>(existing.value())) {
            v->SetFrame(img);
            v->SetBounds(bounds);
        }
        return;
    }
    if (img.empty())
        return;   // nothing to show and nothing existed — stay absent
    auto videoObj = std::make_shared<rendering::VideoObject>(objId, objId);
    videoObj->SetFrame(img);
    videoObj->SetBounds(bounds);
    videoObj->SetLayer("media");
    (void)engine.AddObject(sceneId, videoObj, "media");
}

// Per-sceneId memory of which object ids AddCompositorLayers added for
// overlays last call, and the take-order signature (comma-joined ids) that
// produced them — so a call that finds the SAME signature again (the
// overwhelmingly common case: a slide re-rendering at 60Hz while nothing
// about its overlays changed) skips straight past the expensive part
// (re-running AddStageBlocks' text-layout work for every block of every
// overlay) instead of redoing it every single frame. A changed signature
// removes the previous call's objects first — AddObject has no upsert
// semantics (see its own definition), so leaving them would just pile up
// duplicates each time an overlay is taken/cleared mid-slide.
struct OverlaySceneCache {
    std::string signature;
    std::vector<std::string> objectIds;
};
std::mutex g_overlayCacheMutex;
std::map<std::string, OverlaySceneCache> g_overlayCache;

// The three compositor layers OWNED BY THE UI (LiveOutputService — the real
// PAL video taps, the real QMediaPlayer decoder, the overlay pane's take/
// clear gestures) rather than by any slide's own content: a taken camera/
// screen input, a playing media file, and any active overlays. Independent
// of the slide's own content family — present even in a gated/"refused"
// scene (BuildGatedSlideScene), same rule the QML preview tile already
// follows (OutputMonitorTile.qml's inputTaken/mediaOnAir never depend on
// whether the on-air TEXT content passed an output style's gates). Reads
// CompositorState (populated by LiveOutputService whenever this state
// changes) — this function, and everything it calls, never reaches back
// into a QML service itself. MUST be called on every BuildSlideScene/
// BuildGatedSlideScene invocation, INCLUDING cache hits (see UpsertVideoLayer's
// comment) — never gated behind the "already built" early return.
void AddCompositorLayers(rendering::RenderEngine& engine, const std::string& sceneId,
                         const rendering::Size& size) {
    // Taken camera/screen input: pulled straight from the PAL's own
    // thread-safe tap (this runs on SceneBuilder's own worker thread) —
    // CompositorState only mirrors WHICH device, not its pixels, so a live
    // feed never goes stale waiting on a push from the UI thread.
    const auto taken = presentation::CompositorState::Instance().GetTakenInput();
    rendering::RgbaImage takenImg;
    if (!taken.deviceId.empty() && bps::platform::PlatformAccessor::Installed()) {
        const auto frame = bps::platform::PlatformAccessor::Get().Video()
                               .PreviewFramePixels(taken.deviceId);
        takenImg = ToRgbaImage(frame.width, frame.height, frame.rgba);
    }
    UpsertVideoLayer(engine, sceneId, "takeninput", takenImg,
                     rendering::Rect(0, 0, size.width, size.height));

    // Media file on air: already-decoded pixels LiveOutputService pushed
    // (it owns the real QMediaPlayer/QVideoSink decoder — no second decode
    // here). CoverRect (this file's own style-background-image fit) rather
    // than a letterboxed fit — a reasonable first cut, not a pixel-match of
    // the QML tile's PreserveAspectFit.
    const auto media = presentation::CompositorState::Instance().GetMediaFrame();
    const rendering::RgbaImage mediaImg = ToRgbaImage(media.width, media.height, media.rgba);
    UpsertVideoLayer(engine, sceneId, "mediaonair", mediaImg,
                     mediaImg.empty() ? rendering::Rect(0, 0, size.width, size.height)
                                      : CoverRect(mediaImg.width, mediaImg.height, size));

    // Overlays: the SAME block-rendering pass the slide's own content uses
    // (AddStageBlocks), once per active overlay, in take order — id-tagged
    // so overlays can never collide with each other or the slide's own
    // objects. placeUnderSlide routes into "media" (below the slide's
    // text — a lower-third-BEHIND-text kind of layer) or "overlay" (above
    // everything, the more common placement) — the exact two groups
    // OutputMonitorTile.qml's own two Repeaters already split on.
    const std::vector<presentation::CompositorState::ActiveOverlay> overlays =
        presentation::CompositorState::Instance().GetActiveOverlays();
    std::string signature;
    for (const auto& ov : overlays)
        signature += ov.id + (ov.placeUnderSlide ? "u," : "o,");

    std::lock_guard<std::mutex> lock(g_overlayCacheMutex);
    OverlaySceneCache& cache = g_overlayCache[sceneId];
    if (cache.signature == signature)
        return;   // unchanged since last call — the objects already in the scene are still current

    for (const std::string& objId : cache.objectIds)
        (void)engine.RemoveObject(sceneId, objId);
    cache.objectIds.clear();
    cache.signature = signature;

    int overlayIdx = 0;
    for (const auto& ov : overlays) {
        ++overlayIdx;
        const std::string layer = ov.placeUnderSlide ? "media" : "overlay";
        const std::string tag = std::format("ov{}_", overlayIdx);
        AddStageBlocks(engine, sceneId, ov.blocks, size, tag, layer);
        for (size_t i = 0; i < ov.blocks.size(); ++i)
            cache.objectIds.push_back(std::format("{}{}", tag, i + 1));
    }
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
    // only — not fingerprinted. The BAKED template blocks are: editing the
    // template design re-pushes a spec whose blocks differ, and the hash is
    // what turns that into a rebuild instead of a stale cache hit.
    //
    // PER-FAMILY TEMPLATES join the fingerprint: the family this slide
    // belongs to picks familyTemplateKeys/familyTemplateBlocks, so a family
    // re-pick (or a block edit under it) must re-key THIS slide's scenes —
    // but only the relevant family's slot (an unrelated family's edit must
    // not churn every cached scene). Each family hashes only its own pair;
    // a family with no custom template contributes just its 0 marker.
    size_t familyIdx = FamilyTemplateIndexFor(slideContentType(s));
    const std::string &famKey = familyIdx < 4 ? style.familyTemplateKeys[familyIdx]
                                              : std::string();
    const uint64_t famHash = familyIdx < 4 && !style.familyTemplateBlocks[familyIdx].empty()
                                 ? HashBlocks(style.familyTemplateBlocks[familyIdx])
                                 : 0;
    return SceneIdFor(p, s) + std::format("@s:{}_{}_{}_{}_{:016x}_{}_{:016x}",
                                          style.backgroundColor,
                                          style.templateKey,
                                          style.clearBackgroundOnText ? 1 : 0,
                                          style.backgroundImage,
                                          HashBlocks(style.templateBlocks),
                                          famKey,
                                          famHash);
}

Result<std::string> SceneBuilder::BuildSlideScene(const Presentation& presentation,
                                                  const Slide& slide,
                                                  rendering::RenderEngine& engine,
                                                  rendering::Size size) {
    return BuildSlideScene(presentation, slide, OutputStyleSpec{}, engine, size);
}

// The gate check behind BuildGatedSlideScene: does this spec's show* gates
// admit the slide's content family? Untagged/unknown families are always
// admitted (they belong to no gated tab).
bool StyleAdmitsSlide(const OutputStyleSpec& style, const Slide& slide) {
    const std::string fam = slideContentType(slide);
    if (fam.empty())    return true;
    if (fam == "shows")    return style.showShows;
    if (fam == "media")    return style.showMedia;
    if (fam == "scripture") return style.showScripture;
    if (fam == "table")    return style.showTable;
    return true;
}

Result<std::string> SceneBuilder::BuildGatedSlideScene(const Presentation& presentation,
                                                       const Slide& slide,
                                                       const OutputStyleSpec& style,
                                                       rendering::RenderEngine& engine,
                                                       rendering::Size size) {
    if (StyleAdmitsSlide(style, slide))
        return BuildSlideScene(presentation, slide, style, engine, size);

    // REFUSED: a background-only variant of the styled scene. Id carries a
    // "gated" tag + the same style fingerprint, so (a) it never collides
    // with the ungated scene of the same slide (another output may show it),
    // and (b) a gate flip or style edit re-keys it like any styled scene.
    const std::string sceneId =
        SceneIdFor(presentation, slide)
        + "@gated" + StyledSceneIdFor(presentation, slide, style).substr(SceneIdFor(presentation, slide).size());

    if (engine.GetScene(sceneId).ok()) {
        AddCompositorLayers(engine, sceneId, size);   // same cache-hit refresh as BuildSlideScene
        return sceneId;
    }
    auto scene = engine.CreateScene(sceneId, slide.title, size);
    if (!scene.ok()) return scene.error();
    (void)engine.AddLayer(sceneId, rendering::Layer("bg", "Background",
                                                    rendering::LayerKind::Background, 0));
    // "media"/"overlay" (no "text" — that's the whole point of "refused"):
    // a taken camera/screen input, a playing media file, and any active
    // overlays are their OWN compositor layers, independent of whether this
    // output's style gates the slide's TEXT content family — see
    // AddCompositorLayers' own comment for why.
    (void)engine.AddLayer(sceneId, rendering::Layer("media", "Media",
                                                    rendering::LayerKind::Video, 1));
    (void)engine.AddLayer(sceneId, rendering::Layer("overlay", "Overlay",
                                                    rendering::LayerKind::Overlay, 3));

    // The SAME background composition BuildSlideScene runs (colour, then the
    // style image, the slide's own colour still winning under the same
    // clear-on-text rule) — the refused output keeps its look, minus content.
    rendering::Color bg = rendering::Color(0.06f, 0.07f, 0.09f, 1.0f);
    const bool slideOwnsBackground = !slide.background.empty() && slide.background != "transparent";
    const rendering::Color styleBg = StyleBuilder::ParseColor(style.backgroundColor);
    if (styleBg.a > 0.0f && !(style.clearBackgroundOnText && slideOwnsBackground))
        bg = styleBg;
    else if (slideOwnsBackground) {
        if (rendering::Color parsed = StyleBuilder::ParseColor(slide.background); parsed.a > 0.0f)
            bg = parsed;
    }
    auto bgObj = std::make_shared<rendering::BackgroundObject>("bg", "Background", bg);
    bgObj->SetBounds(rendering::Rect(0, 0, size.width, size.height));
    bgObj->SetLayer("bg");
    (void)engine.AddObject(sceneId, bgObj, "bg");
    // Same clearBackgroundOnText gate as BuildSlideScene — see its comment.
    if (!style.backgroundImage.empty() && !style.clearBackgroundOnText) {
        const rendering::RgbaImage img = LoadImageCached(style.backgroundImage);
        if (!img.empty()) {
            auto imageObj = std::make_shared<rendering::ImageObject>("stylebg", "StyleBackground");
            imageObj->SetImage(img, "stylebg:" + style.backgroundImage);
            imageObj->SetBounds(CoverRect(img.width, img.height, size));
            imageObj->SetLayer("bg");
            (void)engine.AddObject(sceneId, imageObj, "bg");
        }
    }
    AddCompositorLayers(engine, sceneId, size);
    Logger::Instance().Debug(std::format("Gated scene built: {} (family refused by style '{}')",
                                        sceneId, style.name), "SceneBuilder");
    return sceneId;
}

Result<std::string> SceneBuilder::BuildSlideScene(const Presentation& presentation,
                                                  const Slide& slide,
                                                  const OutputStyleSpec& style,
                                                  rendering::RenderEngine& engine,
                                                  rendering::Size size) {
    const std::string sceneId = StyledSceneIdFor(presentation, slide, style);

    // Already built (cache hit) — the STRUCTURE (which text/shape objects
    // exist for this slide+style) is reused as-is, but the compositor
    // layers (a taken camera/screen input, a playing media file, active
    // overlays) are dynamic, continuously-updating state that lives OUTSIDE
    // this fingerprint — they must still refresh on every call, cache hit
    // or not, or a taken camera/media freezes on whatever frame existed the
    // moment this scene was first built (see AddCompositorLayers/
    // UpsertVideoLayer's own comments).
    if (engine.GetScene(sceneId).ok()) {
        AddCompositorLayers(engine, sceneId, size);
        return sceneId;
    }

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
    // scene id, so a new image always lands as a rebuild. clearBackgroundOnText
    // hides it outright while a slide is on air (FreeShow's Output.svelte:
    // styleBackground is "" whenever clearStyleBackgroundOnText && (slide ||
    // background) — unconditional on the slide's OWN background, unlike the
    // colour precedence a few lines up). Mirrors OutputMonitorTile.qml's own
    // suppressStyleBgImage gate, which already got this right on the preview
    // side — a busy branded background must never fight with live text.
    if (!style.backgroundImage.empty() && !style.clearBackgroundOnText) {
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

    // Taken camera/screen input, media file on air, active overlays — see
    // AddCompositorLayers' own comment. Runs ONCE here, before every content
    // branch below (each returns early after its own content), so every
    // path gets these layers regardless of which one executes.
    AddCompositorLayers(engine, sceneId, size);

    // ---- Content blocks ---------------------------------------------------
    // The on-air layout when the style wears an ENGINE TEMPLATE: the baked
    // templateBlocks render (with the slide's content bound in), UNDER the
    // slide's own blocks so slide-specific extras still land on top. A style
    // on the legacy presets keeps the old plain text layout below.
    // ---- Content blocks ---------------------------------------------------
    // The on-air layout when the style wears an ENGINE TEMPLATE: the baked
    // templateBlocks render (with the slide's content bound in) and they OWN
    // the composition — the slide's own blocks (carried in from the content
    // tab's own template: verse boxes already laid out for a DIFFERENT
    // design) would paint over the style's template. FreeShow's rule:
    // output style > slide — but PER CONTENT FAMILY: a style's template is
    // keyed for one contentType ("scripture", "table"…), so it only
    // restyles ITS family. The Table going live with a scripture-keyed
    // style keeps its own tab template's layout (the slide's own blocks
    // carry it) — that was the "Table inherits the scripture template" bug.
    // A template-less style keeps the old behaviour (slide blocks are then
    // the layout). EITHER a rendered template layout or slide blocks
    // short-circuits the legacy text layout below.
    const bool styleTemplateApplies = !style.templateBlocks.empty()
        && (style.contentType.empty() || slideContentType(slide).empty()
            || slideContentType(slide) == style.contentType);
    // PER-FAMILY TEMPLATE: a slide renders through ITS family's baked blocks
    // when the slot carries any — the Styles screen's explicit per-type pick
    // (no inherit; the UI bakes the Bible template into the shows slot as
    // the fallback when nothing is picked). Untagged slides are SHOWS
    // content: they follow the shows slot. A family key naming a legacy
    // preset (no bake) degrades to LayoutFor below with that key, exactly
    // like the whole-style pick does.
    const size_t familyIdx = FamilyTemplateIndexFor(slideContentType(slide));
    const bool familyTemplateApplies = familyIdx < 4
        && !style.familyTemplateBlocks[familyIdx].empty();
    if (familyTemplateApplies) {
        const int n = AddStageBlocks(engine, sceneId,
                                     BindTemplateBlocks(style.familyTemplateBlocks[familyIdx], slide),
                                     size, "tplf");
        Logger::Instance().Debug(std::format("Scene built: {} (style '{}', family {} template {} blocks)", sceneId, style.name, familyIdx, n),
                                 "SceneBuilder");
        return sceneId;
    }
    if (styleTemplateApplies) {
        const int n = AddStageBlocks(engine, sceneId,
                                     BindTemplateBlocks(style.templateBlocks, slide),
                                     size, "tpl");
        Logger::Instance().Debug(std::format("Scene built: {} (style '{}', template {} blocks)", sceneId, style.name, n),
                                 "SceneBuilder");
        return sceneId;
    }
    // Family key without a bake (legacy preset per family): LayoutFor with
    // THAT key — a family can ride a built-in preset while other families
    // wear engine designs. The preset owns the composition (style > slide,
    // same rule as the baked templates above), so slide blocks step aside.
    if (familyIdx < 4 && !style.familyTemplateKeys[familyIdx].empty()
        && style.familyTemplateBlocks[familyIdx].empty()) {
        const StyleBuilder::Layout famLayout = StyleBuilder::LayoutFor(style.familyTemplateKeys[familyIdx]);
        const rendering::Rect titleRect = Scaled(famLayout.title, size);
        const rendering::Rect bodyRect = Scaled(famLayout.body, size);
        if (!slide.title.empty() && famLayout.showTitle) {
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
        Logger::Instance().Debug(std::format("Scene built: {} (style '{}', family {} preset '{}')", sceneId, style.name, familyIdx,
                                            style.familyTemplateKeys[familyIdx]),
                                 "SceneBuilder");
        return sceneId;
    }
    if (!slide.blocks.empty()) {
        const int n = AddStageBlocks(engine, sceneId, slide.blocks, size, "blk");
        Logger::Instance().Debug(std::format("Scene built: {} (style '{}', {} blocks)", sceneId, style.name, n),
                                 "SceneBuilder");
        return sceneId;
    }

    // ---- Text layout -------------------------------------------------------
    // The legacy path: plain text slides under the built-in layout presets
    // (the five keys LayoutFor knows). A template-keyed style never reaches
    // this with empty baked blocks — but a style whose templateKey names a
    // design that no longer exists (deleted template) does; LayoutFor falls
    // back to the safe default full layout for the unknown key.
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
