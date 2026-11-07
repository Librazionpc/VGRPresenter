// ScenePaint — the block → draw-command translators. See ScenePaint.hpp.
// Moved out of SceneBuilder.cpp unchanged (same behaviour, one file).
#include "modules/presentation/ScenePaint.hpp"

#include "modules/presentation/BlockMeta.hpp"
#include "modules/presentation/MediaPixels.hpp"
#include "modules/presentation/SlideBinding.hpp"
#include "modules/presentation/SlideRules.hpp"
#include "modules/presentation/TemplateTextMetrics.hpp"
#include "modules/presentation/TextFormat.hpp"

#include "core/logging/Logger.hpp"
#include "modules/library/DesignLibrary.hpp"
#include "modules/presentation/CompositorState.hpp"
#include "modules/rendering/Effects.hpp"
#include "modules/rendering/RenderObject.hpp"
#include "modules/rendering/Scene.hpp"
#include "platform/PlatformAccessor.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <format>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace bps::presentation {
float BisectFittedSize(const std::string& text, const rendering::TextStyle& style,
                       const rendering::Rect& box, const rendering::FontManager& fonts,
                       float minPx, float maxPx) {
    rendering::TextStyle probe = style;
    const float setSpacing = style.lineSpacing;
    const float setSize = std::max(0.001f, style.size);
    const auto fits = [&](float s) {
        probe.size = s;
        probe.lineSpacing = setSpacing * (s / setSize);
        const auto r = rendering::TextLayout::Measure(text, probe, fonts);
        return r.totalHeight <= box.height && r.totalWidth <= box.width;
    };
    if (!fits(minPx)) return minPx;   // even the floor overflows: floor it
    float lo = minPx;
    float hi = std::max(maxPx, minPx);
    for (int i = 0; i < 12; ++i) {
        const float mid = (lo + hi) * 0.5f;
        if (fits(mid)) lo = mid;
        else hi = mid;
    }
    return lo;
}

bool IsBoxKind(const ContentBlock& b) {
    return b.kind == "box" || b.kind == "shape" || b.kind == "rectangle";
}
bool HasOwnBox(const ContentBlock& b) {
    return b.kind == "text"
        && StyleBuilder::ParseColor(b.style.backgroundColor).a > 0.0f;
}

rendering::Rect BlockRect(const ContentBlock& block, const rendering::Size& size) {
    constexpr double kStageW = bps::library::kStageWidth, kStageH = bps::library::kStageHeight;
    return rendering::Rect(
        static_cast<float>(block.x / kStageW * size.width),
        static_cast<float>(block.y / kStageH * size.height),
        static_cast<float>(block.width / kStageW * size.width),
        static_cast<float>(block.height / kStageH * size.height));
}

void ReportLayerState(const std::string& sceneId, const std::string& label, std::string state);

int AddStageBlocks(rendering::RenderEngine& engine, const std::string& sceneId,
                   const std::vector<ContentBlock>& blocks,
                   const rendering::Size& size, std::string_view tag,
                   std::string_view layer,
                   const std::string& flowText,
                   float readingFontPx,
                   const std::vector<int>* readingLineStarts,
                   uint64_t readingLineTextHash,
                   bool flowWindow,
                   const std::string& readingStyleKey) {
    constexpr double kBlockStageW = bps::library::kStageWidth, kBlockStageH = bps::library::kStageHeight;
    constexpr double kDefaultTextFontSize = 16.0;
    constexpr double kNaturalLineHeight = template_text_metrics::kNaturalLineHeight;
    const SceneBuilder::BlockLayerPlan plan = SceneBuilder::PlanBlockLayers(blocks);
    const std::vector<bool>& isBackdrop = plan.underVideo;
    const bool stageCovered = plan.stageCovered;
    int n = 0;
    int demoted = 0;
    size_t blockIndex = 0;
    size_t longestTextBlock = blocks.size();   // none
    {
        size_t longest = 0;
        for (size_t i = 0; i < blocks.size(); ++i) {
            if (blocks[i].kind == "text" && blocks[i].text.size() > longest) {
                longest = blocks[i].text.size();
                longestTextBlock = i;
            }
        }
    }
    for (const ContentBlock& block : blocks) {
        ++n;
        const std::string objId = std::format("{}{}", tag, n);
        // The block's rectangle on the frame — see BlockRect.
        const rendering::Rect rect = BlockRect(block, size);
        const float radiusOut = static_cast<float>(
            std::max(0.0, block.style.cornerRadius) / kBlockStageW * size.width);
        const bool boxKind = IsBoxKind(block);
        const bool mediaKind = (block.kind == "image" || block.kind == "video");
        const std::string underVideo = MetaBoolToken(block.metaJson, "underVideo");
        bool backgroundSlab = isBackdrop[blockIndex];
        if (!underVideo.empty())   // the design's own statement on this block
            backgroundSlab = (underVideo == "true");
        ++blockIndex;
        const std::string_view blockLayer =
            (backgroundSlab && layer == "text") ? std::string_view("bg") : layer;
        if (blockLayer == "bg")
            ++demoted;
        if (block.kind == "text") {
            const rendering::Color boxFill =
                StyleBuilder::ParseColor(block.style.backgroundColor);
            if (boxFill.a > 0.0f) {
                const std::string boxId = objId + "box";
                auto box = std::make_shared<rendering::ShapeObject>(boxId, boxId);
                box->SetRectangle(boxFill, radiusOut);
                box->SetBounds(rect);
                box->SetLayer(std::string(blockLayer));
                (void)engine.AddObject(sceneId, box, std::string(blockLayer));
            }
            const bool oneLine = MetaBool(block.metaJson, "oneLine");
            std::string textBody = block.text;
            if (oneLine) {
                for (char& ch : textBody)
                    if (ch == '\n' || ch == '\r') ch = ' ';
            }
            // The reading block: the one whose text the passage continues. In window mode the passage
            // is the exact word window, which starts at the current word and so does not begin with
            // the slide's own text — there the reading block is the longest text block, the same
            // rule the service steps the walk by.
            const size_t thisBlock = static_cast<size_t>(&block - blocks.data());
            const bool isFlowBlock = !flowText.empty() && !textBody.empty()
                && (flowWindow ? thisBlock == longestTextBlock : flowText.rfind(textBody, 0) == 0);
            if (!isFlowBlock) {
                textBody = ApplyTextCase(
                    bps::presentation::ApplyList(textBody, MetaString(block.metaJson, "list")),
                    MetaString(block.metaJson, "textCase"));
            }
            const float horizontalPaddingPx = static_cast<float>(
                std::max(0.0, block.style.padding) * size.width / kBlockStageW);
            const float verticalPaddingPx = static_cast<float>(
                std::max(0.0, block.style.padding) * size.height / kBlockStageH);
            const float horizontalPadding = std::min(horizontalPaddingPx, rect.width * 0.5f);
            const float verticalPadding = isFlowBlock
                ? std::min(verticalPaddingPx, rect.height * 0.5f) : 0.0f;
            const rendering::Rect textBounds(
                rect.x + horizontalPadding, rect.y + verticalPadding,
                rect.width - horizontalPadding * 2.0f,
                rect.height - verticalPadding * 2.0f);
            auto text = std::make_shared<rendering::TextObject>(objId, objId, textBody);
            text->SetBounds(textBounds);
            // Scroll's max-words column fills the text area with no vertical alignment.
            text->SetFlowFillsBox(isFlowBlock && readingFontPx > 0.0f);
            // Whose line breaks this block draws: a later refresh of the same scene hands it a new table.
            if (isFlowBlock)
                text->SetReadingStyleKey(readingStyleKey);
            auto styleComp = std::make_shared<rendering::TextStyleComponent>();
            styleComp->style.size = static_cast<float>(
                ((isFlowBlock && readingFontPx > 0.0f)
                     ? static_cast<double>(readingFontPx)
                     : MetaNumber(block.metaJson, "fontSize", kDefaultTextFontSize))
                * size.height / kBlockStageH);
            // THE READING COLUMN KEEPS THE MONITOR'S PROPORTIONS. The monitors wrap a column of
            // `width` stage px with a font of `readingFontPx` stage px, so words-per-line is that
            // ratio. Scaling the font by the HEIGHT ratio (the stage is not exactly 16:9) and the
            // wrap width by the WIDTH ratio, then flooring the font to a whole pixel, made the
            // engine's column ~1-2% wider than the monitor's: one more word on a line. Here the
            // font follows the width ratio, and the wrap is trimmed by what the whole-pixel font
            // lost, so the box-to-font ratio is the monitor's own.
            float flowWrapScale = 1.0f;
            if (isFlowBlock && readingFontPx > 0.0f) {
                const float exact = static_cast<float>(
                    static_cast<double>(readingFontPx) * size.width / kBlockStageW);
                const float whole = static_cast<float>(rendering::FontManager::QtPixelSize(exact));
                styleComp->style.size = whole;
                flowWrapScale = whole / exact;
                // THE PERMANENT PART: when the service handed this output's style the line
                // breaks it cut for THIS passage, draw those lines — no wrapping here at all.
                // The wrap width is then only the justify target, so it is the box itself (a
                // line cut at the stage's size is never wider than the box once scaled).
                if (readingLineStarts && !readingLineStarts->empty()
                        && rendering::TextLayout::TextHash(flowText) == readingLineTextHash) {
                    auto forced = std::make_shared<rendering::ForcedLineBreaks>();
                    forced->starts = *readingLineStarts;
                    forced->textHash = readingLineTextHash;
                    styleComp->style.forcedBreaks = std::move(forced);
                    flowWrapScale = 1.0f;
                }
            }
            styleComp->style.color = StyleBuilder::ParseColor(
                MetaString(block.metaJson, "color").empty()
                    ? "#ffffff" : MetaString(block.metaJson, "color"));
            styleComp->style.align = ParseAlign(MetaString(block.metaJson, "align"));
            // SCROLL READS LIKE A BOOK PAGE: the max-words column is fully justified on every screen,
            // whatever the template says (Main's justify, the Lower Third's centre), so only the size
            // differs.
            if (isFlowBlock && readingFontPx > 0.0f)
                styleComp->style.align = rendering::TextAlign::Justify;
            styleComp->style.valign = ParseVAlign(MetaString(block.metaJson, "verticalAlign"));
            styleComp->style.bold = MetaBool(block.metaJson, "bold");
            {
                const std::string weight = MetaString(block.metaJson, "fontWeight");
                styleComp->style.fontWeight = weight == "Regular" ? QFont::Normal
                    : weight == "SemiBold" ? QFont::DemiBold
                    : weight == "Bold" ? QFont::Bold
                    : QFont::Medium;
            }
            styleComp->style.underline = MetaBool(block.metaJson, "underline");
            styleComp->style.strikeout = MetaBool(block.metaJson, "strikethrough");
            {
                const std::string family = MetaString(block.metaJson, "fontFamily");
                styleComp->style.fontId = family.empty() ? "Cambria" : family;
            }
            styleComp->style.italic = MetaBool(block.metaJson, "italic");
            styleComp->style.wrap = !oneLine;
            styleComp->style.wrapWidth = oneLine ? 0.0f : textBounds.width * flowWrapScale;
            {
                const double lh = template_text_metrics::lineHeightMultiplier(
                    MetaNumber(block.metaJson, "lineHeight", kNaturalLineHeight));
                const float sizePx = styleComp->style.size;   // the output-scaled font size
                const auto& fonts = rendering::RenderEngine::Instance().Fonts();
                const std::string family = fonts.SubstituteFamily(styleComp->style.fontId);
                rendering::FontManager::LineMetrics lm{};
                if (!family.empty() && fonts.SystemLineMetrics(
                        family, sizePx, styleComp->style.bold,
                        styleComp->style.italic, lm, styleComp->style.fontWeight)) {
                    styleComp->style.lineSpacing = static_cast<float>(
                        template_text_metrics::engineLineSpacing(sizePx, lm.inkHeight, lh));
                } else {
                    const double naturalHeight = MetaNumber(
                        block.metaJson, "fontSize", kDefaultTextFontSize)
                        * size.height / kBlockStageH;
                    styleComp->style.lineSpacing = static_cast<float>(
                        template_text_metrics::engineLineSpacing(sizePx, naturalHeight, lh));
                }
            }
            {
                const double ls = MetaNumber(block.metaJson, "letterSpacing", 0.0);
                styleComp->style.letterSpacing = static_cast<float>(ls * size.width / kBlockStageW);
            }
            {
                const double ws = MetaNumber(block.metaJson, "wordSpacing", 0.0);
                styleComp->style.wordSpacing = static_cast<float>(ws * size.width / kBlockStageW);
            }
            {
                const double gap = MetaNumber(block.metaJson, "lineSpacing", 0.0);
                styleComp->style.lineGap = static_cast<float>(gap * size.height / kBlockStageH);
            }
            styleComp->style.lineBg =
                StyleBuilder::ParseColor(MetaString(block.metaJson, "lineBg"));
            {
                const double lr = MetaNumber(block.metaJson, "lineRadius", 0.0);
                styleComp->style.lineRadius =
                    static_cast<float>(std::max(0.0, lr) * size.width / kBlockStageW);
            }
            const std::string autoSize = MetaString(block.metaJson, "autoSize");
            const bool growToFit = autoSize == "grow" || autoSize == "growToFit";
            const float minPx =
                static_cast<float>(3.0 * size.height / kBlockStageH);   // the canvas solver's own 3px floor, output-scaled
            // (A solved scroll size means NO auto-fit at all, grow-to-fit included: Scroll takes
            // the budget's size as given, as the monitor surfaces do.)
            if (isFlowBlock && (!growToFit || readingFontPx > 0.0f)) {
            } else if (growToFit) {
                styleComp->style.size = BisectFittedSize(
                    block.text, styleComp->style, text->Bounds(),
                    rendering::RenderEngine::Instance().Fonts(), minPx,
                    static_cast<float>(400.0 * size.height / kBlockStageH));
            } else {
                styleComp->style.size = BisectFittedSize(
                    block.text, styleComp->style, text->Bounds(),
                    rendering::RenderEngine::Instance().Fonts(), minPx,
                    styleComp->style.size);
            }
            text->AddComponent(styleComp);
            text->SetLayer(std::string(layer));
            (void)engine.AddObject(sceneId, text, std::string(layer));
        } else if (mediaKind) {
            const std::string src = MediaSrcOf(block);
            rendering::RgbaImage picture;
            if (!src.empty())
                picture = (block.kind == "video") ? LoadVideoPosterCached(src)
                                                  : LoadImageCached(src);
            if (picture.width > 0 && picture.height > 0) {
                const MediaItemValues media = ParseMediaItemValues(block.metaJson);
                const bool identity = !media.HasCrop() && !media.flipH && !media.flipV
                                   && !media.HasFilter();
                rendering::RgbaImage sharp = identity ? std::move(picture)
                                                     : ApplyMediaPixelWork(std::move(picture), media);
                const std::string& fit = media.fit;
                rendering::Rect outBox = ContainRectIn(sharp.width, sharp.height, rect);
                if (fit == "cover") {
                    // Cover inside a box the engine cannot clip: crop the SOURCE to
                    // the box's own aspect and let the contain fit fill it exactly.
                    sharp = CropToAspect(sharp, rect.width, rect.height);
                    outBox = ContainRectIn(sharp.width, sharp.height, rect);
                } else if (fit == "fill") {
                    // Stretched to the box, aspect ignored — the canvas draws the
                    // same file with Image.Stretch.
                    outBox = rect;
                } else if (fit == "blur") {
                    rendering::RgbaImage backdrop = CropToAspect(sharp, rect.width, rect.height);
                    rendering::SlideFilterValues soft;
                    soft.blurPx = 24.0;
                    soft.brightness = 0.85;
                    rendering::ApplyFilters(backdrop, soft);
                    auto bg = std::make_shared<rendering::ImageObject>(objId + "-blur",
                                                                      block.kind + "-blur");
                    bg->SetBounds(rect);
                    bg->SetImage(std::move(backdrop), src);
                    bg->SetLayer(std::string(blockLayer));
                    (void)engine.AddObject(sceneId, bg, std::string(blockLayer));
                }
                auto image = std::make_shared<rendering::ImageObject>(objId, block.kind);
                image->SetBounds(outBox);
                image->SetImage(std::move(sharp), src);
                image->SetLayer(std::string(blockLayer));
                image->SetBlendMode(rendering::ParseBlendMode(media.blend));
                (void)engine.AddObject(sceneId, image, std::string(blockLayer));
            }
        } else if (boxKind) {
            auto shape = std::make_shared<rendering::ShapeObject>(objId, objId);
            shape->SetRectangle(StyleBuilder::ParseColor(
                block.style.backgroundColor.empty() ? "#66000000"
                                                    : block.style.backgroundColor),
                                radiusOut);
            shape->SetBounds(rect);
            shape->SetLayer(std::string(blockLayer));
            (void)engine.AddObject(sceneId, shape, std::string(blockLayer));
        }
    }
    if (layer == "text")
        ReportLayerState(sceneId, "backdrop",
                         std::format("{} of {} blocks under the video layer (stage covered: {})",
                                     demoted, n, stageCovered ? "yes" : "no"));
    return n;
}

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

void UpsertVideoLayer(rendering::RenderEngine& engine, const std::string& sceneId,
                      const std::string& objId, const rendering::RgbaImage& img,
                      const rendering::Rect& bounds, uint64_t sourceSeq,
                      rendering::BlendMode blend) {
    auto existing = engine.GetObject(sceneId, objId);
    if (existing.ok()) {
        if (auto* v = dynamic_cast<rendering::VideoObject*>(existing.value())) {
            v->SetFrame(img);
            v->SetBounds(bounds);
            v->SetSourceSeq(sourceSeq);
            v->SetBlendMode(blend);
        }
        return;
    }
    if (img.empty())
        return;   // nothing to show and nothing existed — stay absent
    auto videoObj = std::make_shared<rendering::VideoObject>(objId, objId);
    videoObj->SetFrame(img);
    videoObj->SetBounds(bounds);
    videoObj->SetSourceSeq(sourceSeq);
    videoObj->SetBlendMode(blend);
    videoObj->SetLayer("media");
    (void)engine.AddObject(sceneId, videoObj, "media");
}

// The same, for a caller that is DONE with the frame: it is moved into the scene object instead of
// copied (8 MB at 1080p, once per scene per video frame — the taken input lands in every scene).
void UpsertVideoLayerOwned(rendering::RenderEngine& engine, const std::string& sceneId,
                           const std::string& objId, rendering::RgbaImage&& img,
                           const rendering::Rect& bounds, uint64_t sourceSeq,
                           rendering::BlendMode blend) {
    auto existing = engine.GetObject(sceneId, objId);
    if (existing.ok()) {
        if (auto* v = dynamic_cast<rendering::VideoObject*>(existing.value())) {
            v->SetFrame(std::move(img));
            v->SetBounds(bounds);
            v->SetSourceSeq(sourceSeq);
            v->SetBlendMode(blend);
        }
        return;
    }
    if (img.empty())
        return;
    auto videoObj = std::make_shared<rendering::VideoObject>(objId, objId);
    videoObj->SetFrame(std::move(img));
    videoObj->SetBounds(bounds);
    videoObj->SetSourceSeq(sourceSeq);
    videoObj->SetBlendMode(blend);
    videoObj->SetLayer("media");
    (void)engine.AddObject(sceneId, videoObj, "media");
}

// The same for a picture held by pointer: every scene's object keeps the ONE shared allocation (no
// pixel copy at all). A null image clears an existing layer, like an empty frame does.
void UpsertVideoLayerShared(rendering::RenderEngine& engine, const std::string& sceneId,
                            const std::string& objId, const rendering::SharedRgbaImage& img,
                            const rendering::Rect& bounds, uint64_t sourceSeq,
                            rendering::BlendMode blend) {
    auto existing = engine.GetObject(sceneId, objId);
    if (existing.ok()) {
        if (auto* v = dynamic_cast<rendering::VideoObject*>(existing.value())) {
            v->SetFrameShared(img);
            v->SetBounds(bounds);
            v->SetSourceSeq(sourceSeq);
            v->SetBlendMode(blend);
        }
        return;
    }
    if (!img || img->empty())
        return;
    auto videoObj = std::make_shared<rendering::VideoObject>(objId, objId);
    videoObj->SetFrameShared(img);
    videoObj->SetBounds(bounds);
    videoObj->SetSourceSeq(sourceSeq);
    videoObj->SetBlendMode(blend);
    videoObj->SetLayer("media");
    (void)engine.AddObject(sceneId, videoObj, "media");
}

// One camera frame per sequence number, built once and shared by every scene that asks for it (the
// program scene and each output's scene all want the SAME picture): the first asker pays the PAL copy
// and the conversion, the rest get the pointer. An uncounted source (seq 0) is never cached.
rendering::SharedRgbaImage SharedTakenFrame(const std::string& deviceId, uint64_t seq) {
    static std::mutex cacheMutex;
    static std::string cachedDevice;
    static uint64_t cachedSeq = 0;
    static rendering::SharedRgbaImage cachedImage;
    if (seq != 0) {
        std::lock_guard<std::mutex> lock(cacheMutex);
        if (cachedImage && cachedSeq == seq && cachedDevice == deviceId)
            return cachedImage;
    }
    const auto frame = bps::platform::PlatformAccessor::Get().Video().PreviewFramePixels(deviceId);
    rendering::RgbaImage img = ToRgbaImage(frame.width, frame.height, frame.rgba);
    if (img.empty())
        return nullptr;
    rendering::SharedRgbaImage shared = std::make_shared<const rendering::RgbaImage>(std::move(img));
    if (seq != 0) {
        std::lock_guard<std::mutex> lock(cacheMutex);
        cachedDevice = deviceId;
        cachedSeq = seq;
        cachedImage = shared;
    }
    return shared;
}

// ---- DRAW A LIVE LAYER ONLY WHEN ITS PICTURE MOVED -------------------------
bool LayerRefreshAlways() {
    static const bool always = std::getenv("BPS_LAYER_REFRESH_ALWAYS") != nullptr;
    return always;
}

bool LayerFrameCurrent(rendering::RenderEngine& engine, const std::string& sceneId,
                       const std::string& objId, uint64_t seq) {
    if (seq == 0 || LayerRefreshAlways())
        return false;   // uncounted source (or the measurement hook): refresh, never guess
    auto obj = engine.GetObject(sceneId, objId);
    if (!obj.ok())
        return false;
    const auto* video = dynamic_cast<rendering::VideoObject*>(obj.value());
    return video && video->HasFrame() && video->SourceSeq() == seq;
}

// ---- Bus video planes (a "bus" block on the on-air slide) -----------------
struct BusPlane {
    int busId = 0;
    std::string objId;          // stable per block, so the frame upserts in place
    rendering::Rect rect;
    MediaItemValues media;
};
std::mutex g_busPlaneMutex;
std::map<std::string, std::vector<BusPlane>> g_busPlanes;

int BusIdOf(const ContentBlock& block) {
    if (block.kind != "bus")
        return 0;
    return static_cast<int>(MetaNumber(block.metaJson, "busId", 0.0));
}

void RecordBusPlanes(const std::string& sceneId, const std::vector<ContentBlock>& blocks,
                     const rendering::Size& size, const OutputStyleSpec* gate) {
    std::vector<BusPlane> planes;
    for (size_t i = 0; i < blocks.size(); ++i) {
        const ContentBlock& block = blocks[i];
        const int busId = BusIdOf(block);
        if (busId <= 0)
            continue;
        if (gate && gate->gateVideoBuses &&
            std::find(gate->allowedVideoBuses.begin(), gate->allowedVideoBuses.end(), busId) ==
                gate->allowedVideoBuses.end())
            continue;
        // The block's own id keeps the object stable across refreshes; a block
        // that somehow has none falls back to its index (still unique here).
        const std::string key = block.id.empty() ? std::to_string(i) : block.id;
        planes.push_back(BusPlane{busId, "bus:" + key, BlockRect(block, size),
                                  ParseMediaItemValues(block.metaJson)});
    }
    std::lock_guard<std::mutex> lock(g_busPlaneMutex);
    if (planes.empty())
        g_busPlanes.erase(sceneId);
    else
        g_busPlanes[sceneId] = std::move(planes);
}

void RecordTemplateWindows(const std::string& sceneId, const std::vector<ContentBlock>& blocks,
                           const rendering::Size& size, int busNumber)
{
    std::vector<BusPlane> planes;
    int route = 0;
    for (size_t i = 0; i < blocks.size(); ++i) {
        const ContentBlock& block = blocks[i];
        if (block.kind != "bus")
            continue;
        const int named = BusIdOf(block);
        const int planeKey = named > 0
            ? named
            : CompositorState::TemplateWindowPlaneKey(busNumber, route++);
        if (planeKey <= 0)
            continue;
        const std::string key = block.id.empty() ? std::to_string(i) : block.id;
        planes.push_back(BusPlane{planeKey, "w:" + key, BlockRect(block, size),
                                  ParseMediaItemValues(block.metaJson)});
    }
    std::lock_guard<std::mutex> lock(g_busPlaneMutex);
    if (planes.empty())
        g_busPlanes.erase(sceneId);
    else
        g_busPlanes[sceneId] = std::move(planes);
}

rendering::RgbaImage BusPlaceholder(const rendering::Rect& rect) {
    rendering::RgbaImage img;
    img.width = std::max(1, static_cast<int>(rect.width));
    img.height = std::max(1, static_cast<int>(rect.height));
    const uint8_t plate[4] = {0x24, 0x26, 0x30, 0xff};   // the app's dark card ground
    uint32_t pixel = 0;
    std::memcpy(&pixel, plate, sizeof(pixel));
    img.pixels.assign(static_cast<size_t>(img.width) * img.height, pixel);
    return img;
}

rendering::Rect BusFitRect(const rendering::RgbaImage& img, const rendering::Rect& box,
                           const std::string& fit) {
    // A BUS WINDOW IS NOT A PICTURE ON THE SLIDE. The designed block rectangle IS the
    // plane — that is the whole point of a window block — so the feed is scaled into it
    // instead of being letterboxed inside it (bars the design never asked for, and a plane
    // that no longer sits where the block says it does). "cover" has already cropped the
    // source to the window's aspect, so it lands on the box either way; only an explicit
    // "contain" still letterboxes.
    if (fit == "contain")
        return ContainRectIn(img.width, img.height, box);
    return box;
}

void RefreshBusPlanes(rendering::RenderEngine& engine, const std::string& sceneId) {
    std::vector<BusPlane> planes;
    {
        std::lock_guard<std::mutex> lock(g_busPlaneMutex);
        const auto it = g_busPlanes.find(sceneId);
        if (it == g_busPlanes.end())
            return;
        planes = it->second;
    }
    const auto& compositor = presentation::CompositorState::Instance();
    int withFrame = 0;
    for (const BusPlane& plane : planes) {
        const uint64_t seq = compositor.GetBusFrameSeq(plane.busId);
        if (LayerFrameCurrent(engine, sceneId, plane.objId, seq)) {
            ++withFrame;   // it has a picture, and it is the current one
            continue;
        }
        const auto frame = compositor.GetBusFrame(plane.busId);
        rendering::RgbaImage img = ToRgbaImage(frame.width, frame.height, frame.rgba);
        rendering::Rect outBox = plane.rect;
        if (!img.empty()) {
            ++withFrame;
            const MediaItemValues& media = plane.media;
            const bool identity = !media.HasCrop() && !media.flipH && !media.flipV
                               && !media.HasFilter();
            if (!identity)
                img = ApplyMediaPixelWork(std::move(img), media);
            // ...and FIT decides the box it lands in (cover crops the source to
            // the window's aspect first, because the engine cannot clip).
            if (media.fit == "cover")
                img = CropToAspect(img, plane.rect.width, plane.rect.height);
            outBox = BusFitRect(img, plane.rect, media.fit);
        } else {
            img = BusPlaceholder(plane.rect);   // no feed yet: a plate, not a hole
        }
        UpsertVideoLayer(engine, sceneId, plane.objId, img, outBox, seq,
                         rendering::ParseBlendMode(plane.media.blend));
    }
    ReportLayerState(sceneId, "bus video",
                     std::format("{} of {} bus plane(s) have a frame", withFrame, planes.size()));
}

struct OverlaySceneCache {
    std::string signature;
    std::vector<std::string> objectIds;
    bool building = false;
};
std::mutex g_overlayCacheMutex;
std::map<std::string, OverlaySceneCache> g_overlayCache;

std::mutex g_mediaLogMutex;
std::map<std::string, std::string> g_mediaStateLog;

void ReportLayerState(const std::string& sceneId, const std::string& label, std::string state) {
    bool changed = false;
    {
        std::lock_guard<std::mutex> lock(g_mediaLogMutex);
        std::string& last = g_mediaStateLog[sceneId + "/" + label];
        if (last != state) {
            last = state;
            changed = true;
        }
    }
    if (changed)
        Logger::Instance().Info(std::format("{} layer {} [{}]", label, state, sceneId),
                                "SceneBuilder");
}

void AddCompositorLayers(rendering::RenderEngine& engine, const std::string& sceneId,
                         const rendering::Size& size, bool showOverlay) {
    const auto taken = presentation::CompositorState::Instance().GetTakenInput();
    rendering::SharedRgbaImage takenImg;
    int takenW = 0, takenH = 0;
    uint64_t takenSeq = 0;
    bool takenRefresh = true;
    if (!taken.deviceId.empty() && bps::platform::PlatformAccessor::Installed()) {
        takenSeq = bps::platform::PlatformAccessor::Get().Video().PreviewFrameSeq(taken.deviceId);
        takenRefresh = !LayerFrameCurrent(engine, sceneId, "takeninput", takenSeq);
    }
    if (takenRefresh) {
        if (!taken.deviceId.empty() && bps::platform::PlatformAccessor::Installed())
            takenImg = SharedTakenFrame(taken.deviceId, takenSeq);
        takenW = takenImg ? takenImg->width : 0;
        takenH = takenImg ? takenImg->height : 0;
        UpsertVideoLayerShared(engine, sceneId, "takeninput", takenImg,
                               rendering::Rect(0, 0, size.width, size.height), takenSeq,
                               rendering::BlendMode::Alpha);
    }

    if (takenRefresh) {
        const bool present = engine.GetObject(sceneId, "takeninput").ok();
        const std::string state = taken.deviceId.empty()
            ? std::string("absent — no input taken")
            : (takenW == 0 || takenH == 0
               ? std::format("absent — PAL tap gave no pixels for device '{}' (kind '{}')",
                             taken.deviceId, taken.kind)
               : std::format("{}{} — device '{}' (kind '{}') {}x{} px -> {}x{} px",
                             present ? "present" : "MISSING",
                             present ? "" : " (AddObject refused)",
                             taken.deviceId, taken.kind, takenW, takenH,
                             size.width, size.height));
        ReportLayerState(sceneId, "taken input", state);
    }

    const auto& compositorState = presentation::CompositorState::Instance();
    const uint64_t mediaSeq = compositorState.GetMediaSeq();
    const bool mediaRefresh = !LayerFrameCurrent(engine, sceneId, "mediaonair", mediaSeq);
    // The frame arrives as one shared picture (converted once per frame, however many scenes ask).
    const auto media = mediaRefresh ? compositorState.GetMediaShared()
                                    : presentation::CompositorState::SharedMedia{};
    const rendering::SharedRgbaImage& mediaImg = media.image;
    const bool haveMedia = mediaImg && !mediaImg->empty();
    const std::string_view effectiveFit = media.fit.empty() ? std::string_view("contain")
                                                            : std::string_view(media.fit);
    rendering::Rect mediaRect(0, 0, size.width, size.height);
    if (haveMedia) {
        if (effectiveFit == "cover") {
            mediaRect = CoverRect(mediaImg->width, mediaImg->height, size);
        } else if (effectiveFit == "fill") {
            mediaRect = rendering::Rect(0, 0, size.width, size.height);
        } else {
            mediaRect = ContainRect(mediaImg->width, mediaImg->height, size);
        }
    }
    if (mediaRefresh)
        UpsertVideoLayerShared(engine, sceneId, "mediaonair", mediaImg, mediaRect, mediaSeq,
                               rendering::BlendMode::Alpha);

    if (mediaRefresh) {
        const bool present = engine.GetObject(sceneId, "mediaonair").ok();
        const std::string state = !haveMedia
            ? std::string("absent — compositor frame empty")
            : std::format("{}{} — compositor frame {}x{} -> {}x{} px{}",
                          present ? "present" : "MISSING",
                          present ? "" : " (AddObject refused)",
                          mediaImg->width, mediaImg->height,
                          mediaImg->width, mediaImg->height, mediaImg->pixels.size(),
                          sceneId.find("@gated") != std::string::npos ? " [gated scene]" : "");
        ReportLayerState(sceneId, "media", state);
    }

    const std::vector<presentation::CompositorState::ActiveOverlay> overlays = showOverlay
        ? presentation::CompositorState::Instance().GetActiveOverlays()
        : std::vector<presentation::CompositorState::ActiveOverlay>{};
    std::string signature;
    for (const auto& ov : overlays)
        signature += ov.id + (ov.placeUnderSlide ? "u," : "o,");

    std::vector<std::string> staleIds;
    {
        std::lock_guard<std::mutex> lock(g_overlayCacheMutex);
        OverlaySceneCache& cache = g_overlayCache[sceneId];
        if (cache.signature == signature)
            return;   // unchanged since last call — the objects already in the scene are still current
        if (cache.building)
            return;   // another thread is composing this scene's overlays; they land on its next call
        cache.building = true;
        cache.signature.clear();
        staleIds.swap(cache.objectIds);
    }

    // Removals and the build itself, OUTSIDE the lock (see the note above).
    for (const std::string& objId : staleIds)
        (void)engine.RemoveObject(sceneId, objId);

    std::vector<std::string> builtIds;
    int overlayIdx = 0;
    for (const auto& ov : overlays) {
        ++overlayIdx;
        const std::string layer = ov.placeUnderSlide ? "media" : "overlay";
        const std::string tag = std::format("ov{}_", overlayIdx);
        AddStageBlocks(engine, sceneId, ov.blocks, size, tag, layer);
        for (size_t i = 0; i < ov.blocks.size(); ++i) {
            const std::string id = std::format("{}{}", tag, i + 1);
            builtIds.push_back(id);
            builtIds.push_back(id + "box");
        }
    }

    {
        std::lock_guard<std::mutex> lock(g_overlayCacheMutex);
        OverlaySceneCache& cache = g_overlayCache[sceneId];
        cache.objectIds = std::move(builtIds);
        cache.signature = std::move(signature);
        cache.building = false;
    }
}
} // namespace bps::presentation