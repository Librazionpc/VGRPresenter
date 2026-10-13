#include "modules/presentation/SceneBuilder.hpp"

#include "core/logging/Logger.hpp"
#include "core/config/Json.hpp"
#include "modules/library/DesignLibrary.hpp"
#include "modules/presentation/CompositorState.hpp"
#include "modules/presentation/BookFill.hpp"
#include "modules/presentation/PresentationTemplates.hpp"
#include "modules/presentation/SlideRules.hpp"
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
#include <cstdlib>
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
// Does the meta NAME this key at all? MetaString cannot tell an empty value from
// a missing key, and one caller must: the fill writes `refLast` as "" on every
// slide that is not the last, which means "the whole selection is not shown
// here" — falling back to the slide's own reference would put it on every slide
// instead (see SlideValues).
bool MetaHas(std::string_view json, std::string_view key) {
    return json.find(std::format("\"{}\":\"", key)) != std::string_view::npos;
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
// TRI-STATE boolean: "true" / "false" when the design states the field, "" when
// it does not. MetaBool cannot express the third case (absent reads exactly like
// false), and a per-block override like "underVideo" has to tell "the author
// chose over-the-feed" from "the author said nothing". Tolerates a pretty-printed
// value (a space after the colon) so a hand-written or re-serialised metaJson
// reads the same as this codebase's own compact writer.
std::string MetaBoolToken(std::string_view json, std::string_view key) {
    const std::string needle = std::format("\"{}\"", key);
    for (size_t at = json.find(needle); at != std::string_view::npos;
         at = json.find(needle, at + 1)) {
        size_t i = at + needle.size();
        while (i < json.size() && (json[i] == ' ' || json[i] == '\t')) ++i;
        if (i >= json.size() || json[i] != ':') continue;   // a value, not this key
        ++i;
        while (i < json.size() && (json[i] == ' ' || json[i] == '\t')) ++i;
        if (json.compare(i, 4, "true") == 0) return "true";
        if (json.compare(i, 5, "false") == 0) return "false";
    }
    return {};
}

rendering::TextAlign ParseAlign(std::string_view a) {
    if (a == "left") return rendering::TextAlign::Left;
    if (a == "right") return rendering::TextAlign::Right;
    return rendering::TextAlign::Center;   // canvas default (see the call site)
}
rendering::TextVAlign ParseVAlign(std::string_view a) {
    if (a == "top") return rendering::TextVAlign::Top;
    if (a == "bottom") return rendering::TextVAlign::Bottom;
    return rendering::TextVAlign::Middle;   // canvas default (see the call site)
}

// ---- style background image cache -----------------------------------------
// The image is fingerprinted into the scene id, so every rebuild of a style
// with a different image builds a different scene — but ONE image file would
// still be read+decoded once per slide per rebuild without this cache. Keyed
// by path; a read failure returns an empty image (the caller paints the colour
// underneath instead).
std::mutex g_imageMutex;
std::map<std::string, rendering::RgbaImage> g_imageCache;

namespace {
// The uncached load: PAL path resolution, the file read and the decode, all
// with NO cache lock held (the caller stores the result). An empty result is
// cached too — the old negative-caching behavior, so a missing file is not
// re-read on every rebuild.
rendering::RgbaImage LoadImageUncached(const std::string& path) {
    rendering::RgbaImage slot;
    std::string imagePath = path;
    auto& platform = bps::platform::PlatformAccessor::Get();
    if (!platform.Filesystem().Exists(imagePath)) {
        // A stored style can carry the authoring machine's absolute path. The
        // installer seeds the matching image by filename into this user's
        // data directory, so attach that local copy before trying to decode.
        const size_t slash = imagePath.find_last_of("/\\");
        const std::string name = slash == std::string::npos
            ? imagePath : imagePath.substr(slash + 1);
        if (!name.empty()) {
            const std::string roots[] = {
                platform.Paths().UserDataDir(),
                platform.Paths().AssetDir(),
                platform.Paths().ExecutableDir() + "/assets",
            };
            for (const std::string& root : roots) {
                if (root.empty()) continue;
                const std::string candidate = platform.Filesystem().Join(root, name);
                if (platform.Filesystem().Exists(candidate)) {
                    imagePath = candidate;
                    break;
                }
            }
        }
    }

    std::ifstream file(imagePath, std::ios::binary);
    if (!file) return slot;
    std::vector<unsigned char> bytes((std::istreambuf_iterator<char>(file)),
                                     std::istreambuf_iterator<char>());
    if (bytes.size() >= 8 && bytes[0] == 0x89 && bytes[1] == 'P') {
        auto decoded = rendering::DecodePng(bytes.data(), bytes.size());
        if (decoded.ok()) {
            slot.width = decoded.value().width;
            slot.height = decoded.value().height;
            slot.pixels = std::move(decoded.value().pixels);
            return slot;
        }
    }
    // PNGs unsupported by the lightweight engine codec and non-PNG formats
    // (jpg/webp/...) fall back to the platform image decoder (WIC on Windows).
    auto pic = bps::media::MakePlatformFrameSource()->Grab(
        imagePath, bps::media::MediaKind::Image, 0.0, 1920);
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
} // namespace

rendering::RgbaImage LoadImageCached(const std::string& path) {
    {
        std::lock_guard<std::mutex> lock(g_imageMutex);
        auto it = g_imageCache.find(path);
        if (it != g_imageCache.end())
            return it->second;
    }

    // MISS: read + decode with the cache lock RELEASED. The PAL existence
    // check, the seeded-root search, the file read and the WIC fallback decode
    // are all I/O, and a large image takes tens of ms — holding the global
    // image-cache mutex across them stalled every other scene build (and the
    // lock-scope guard flags the PAL call). Two builders racing the same path
    // now decode it twice; the later store wins, which is harmless because a
    // cache value is a pure function of the file.
    rendering::RgbaImage image = LoadImageUncached(path);
    std::lock_guard<std::mutex> lock(g_imageMutex);
    g_imageCache[path] = image;
    return image;
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
        // padding/cornerRadius/borderWidth/borderStyle/borderColor were
        // missing from this fingerprint — a template edit touching only one
        // of these (a corner-radius drag, a border colour swap) landed as a
        // SAME hash, so the styled-scene cache served the pre-edit look
        // until something else in the block happened to change too.
        mixCoord(b.style.padding);
        mixCoord(b.style.cornerRadius);
        mixCoord(b.style.borderWidth);
        mixStr(b.style.borderStyle);
        mixStr(b.style.borderColor);
    }
    return h;
}

// A slide's content family, the style slot that family is styled by, the text
// a bound block shows, and whether a style's gates admit the slide: the SHARED
// rules, defined once in SlideRules.cpp and read by BOTH this builder (what the
// outputs are composed of) and the QML service layer's live preview (what the
// monitor's tiles show). They used to exist here AND there — two copies of a
// rule that decides whether a slide renders at all is precisely how a tile and
// an output come to disagree. SlideRules.hpp carries each rule's own contract.

// Fills every bound text block of a baked template from `slide` — the same
// contract SlideResolver uses for show templates, narrowed to the plain
// slides this path serves: "text" takes the slide's CONTENT (see
// SlideContentText — the verse, not the reference), "ref" takes the slide's
// title (its reference), and anything else goes through
// SlideResolver::BoundValue (meta field, "title", "notes"...). Unbound
// blocks keep their literal text — a template's decorative labels render as
// designed. An unbound block that NAMES PLACEHOLDERS is a slot as well, and is
// resolved through BookFill::FillTokens with the values the slide carries (see
// SlideValues below): {scripture_text}/{table_reference}/… must read the same on
// the outputs as they do in the preview pane, which fills through that same
// vocabulary. A placeholder whose value the engine was never given still comes
// out empty — a raw `{token}` must never reach the air — and a block that names
// no token of the vocabulary at all keeps its old blank-line rule.
// The placeholder values a SLIDE carries, in the one vocabulary BookFill owns.
//
// Where they come from: the body is the slide's own content text, the reference
// is the `ref` the services put in the slide's meta (ShowConverter writes it
// there — the model has no dedicated field), and the table_ family's code /
// year / title / numbers are read from the same meta when a caller supplies
// them. EVERYTHING ELSE STAYS EMPTY ON PURPOSE: the engine does not guess a
// value it was not given, so a token whose value never reaches it renders as an
// empty line rather than as a raw `{table_code}` on air. This is the same
// contract the preview fill keeps (BookValues in BookFill.hpp) — one vocabulary,
// two callers, so a token cannot mean one thing in the preview and another on
// the outputs.
BookValues SlideValues(const Slide& slide) {
    BookValues v;
    // WHICH FAMILY the slide is, read from the same key the fill wrote
    // (SlideBuilder::valuesToMeta). It decides what a family-bound token may
    // answer: {scripture_name}/{scripture_book_abbr}/{scripture_chapter} are
    // facts only a Bible has, so on a sermon they stay empty however the values
    // got here (see BookValues::Answers). A slide with no tag at all is
    // scripture, the default the enumerator carries — the fills that predate
    // this key therefore behave exactly as they did.
    v.family = FromTag(MetaString(slide.metaJson, "family"));
    v.text = SlideRules::ContentText(slide);
    v.reference = MetaString(slide.metaJson, "ref");
    if (v.reference.empty())
        v.reference = slide.title;
    // The fill's whole-selection variants, read with PRESENCE (see MetaHas): a
    // fill-built slide always carries both keys, and an empty `refLast` is the
    // fill saying "not the last slide", not a missing answer.
    v.referenceFull = MetaHas(slide.metaJson, "refFull") ? MetaString(slide.metaJson, "refFull") : v.reference;
    v.referenceLast = MetaHas(slide.metaJson, "refLast") ? MetaString(slide.metaJson, "refLast") : v.reference;
    v.title = MetaString(slide.metaJson, "tableTitle");
    if (v.title.empty())
        v.title = slide.title;
    v.code = MetaString(slide.metaJson, "tableCode");
    v.year = MetaString(slide.metaJson, "tableYear");
    v.numbers = MetaString(slide.metaJson, "tableNumbers");
    v.book = MetaString(slide.metaJson, "book");
    v.bookAbbr = MetaString(slide.metaJson, "bookAbbr");
    v.name = MetaString(slide.metaJson, "bible");
    v.chapter = MetaString(slide.metaJson, "chapter");
    v.copyright = MetaString(slide.metaJson, "copyright");
    // THE PARALLEL COLUMNS (see BookValues::bibles). Column 1 IS this slide's Bible —
    // its body and the `bible`/`bookAbbr` names the meta already carries — and the
    // extra versions arrive as bible2/3/4{Name,Abbr,Text}, put there by the fill
    // (SlideBuilder::valuesToMeta). A slide with none has a single column, which is
    // what every layout that is not a parallel one asks for.
    v.bibles.clear();
    v.bibles.push_back({ v.name, v.bookAbbr, v.text });
    for (int n = 2; n <= 4; ++n) {
        const std::string prefix = "bible" + std::to_string(n);
        const std::string name = MetaString(slide.metaJson, prefix + "Name");
        const std::string abbr = MetaString(slide.metaJson, prefix + "Abbr");
        const std::string text = MetaString(slide.metaJson, prefix + "Text");
        if (name.empty() && abbr.empty() && text.empty())
            break;   // no further version on this slide: the vocabulary's slots stay empty
        v.bibles.push_back({ name, abbr, text });
    }
    return v;
}

std::vector<ContentBlock> BindTemplateBlocks(const std::vector<ContentBlock>& tmpl,
                                             const Slide& slide) {
    std::vector<ContentBlock> out = tmpl;
    const BookValues values = SlideValues(slide);
    for (ContentBlock& b : out) {
        if (b.kind != "text")
            continue;
        if (!b.bind.empty()) {
            std::string value = SlideResolver::BoundValue(slide, b.bind);
            if (value.empty()) {
                if (b.bind == "text")
                    value = SlideRules::ContentText(slide);
                else if (b.bind == "ref")
                    value = slide.title;
            }
            b.text = std::move(value);
        } else if (b.text.find('{') != std::string::npos) {
            // An UNBOUND block that names placeholders is a template slot too:
            // the author wrote {scripture_text}/{table_reference}/… in it rather
            // than naming a bind. Resolve it through the SAME fill the preview
            // uses (BookFill::FillTokens) instead of blanking it, which is what
            // made a table design's reference block draw as an empty box on the
            // outputs while the preview pane showed it filled. A block naming
            // NO token of the vocabulary keeps the old rule (blank — an
            // unrecognised leftover must not reach the air), and so does a block
            // whose every token came out empty: nothing to draw.
            b.text = HasTokens(b.text) ? FillTokens(b.text, values) : std::string();
            if (b.text.find_first_not_of(" \t\r\n") == std::string::npos)
                b.text.clear();
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
// Largest font size ≤ `style.size` whose wrapped layout still fits `box` —
// The ONE fit solver both auto-size modes ride — the canvas's own shape
// (EditScreen.recomputeFit / DesignPreview.solveFit): bisect candidate sizes
// between `minPx` and `maxPx` (OUTPUT px), re-measuring the full layout
// through TextLayout::Measure at each probe — wrapping, real-font advances
// and ink height included — and keep the largest size that fits the box on
// BOTH axes. 12 rounds converge sub-pixel; containment is PROVEN by the
// last measurement, never assumed by a formula. The two heuristic walks
// this replaces were the "auto size/grow misbehaves when I change the
// font" report: the grow walk ascended in ONE proportional step and gave
// up the moment that step overshot (a short one-line verse's first step
// blows past the width bound, so grow silently returned the SET size —
// the exact test failure that proved it), and the shrink walk undershot
// by a whole step. lineSpacing scales proportionally with each probe: it
// is derived from the font's natural line height (SceneBuilder above),
// exactly how Qt's ProportionalHeight lineHeight tracks the pixel size —
// measuring a probe with the set size's spacing would lie about the
// wrapped line count. letterSpacing stays FIXED across probes (a meta px
// value, like the canvas's font.letterSpacing).
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

// The two block shapes that can BE a backdrop: a box/shape block (its own paint
// IS its content) and a text block carrying a box of its own (a content page's
// full-stage fill). Shared by the rule and by the builder below — one reading of
// "what kind of block is this", not one per call site.
bool IsBoxKind(const ContentBlock& b) {
    return b.kind == "box" || b.kind == "shape" || b.kind == "rectangle";
}
bool HasOwnBox(const ContentBlock& b) {
    return b.kind == "text"
        && StyleBuilder::ParseColor(b.style.backgroundColor).a > 0.0f;
}

// Defined below (one line per scene per STATE CHANGE). Declared here because
// AddStageBlocks is where a build reports the split it got from
// SceneBuilder::PlanBlockLayers.
void ReportLayerState(const std::string& sceneId, const std::string& label, std::string state);

int AddStageBlocks(rendering::RenderEngine& engine, const std::string& sceneId,
                   const std::vector<ContentBlock>& blocks,
                   const rendering::Size& size, std::string_view tag,
                   std::string_view layer = "text") {
    // The stage grid has ONE definition on this side too (bps::library's
    // kStageWidth/kStageHeight, beside the ContentBlock they describe; the QML
    // counterpart is qml/settings/StageGrid.qml) — this used to be a second copy
    // of the same two literals, which is how a surface could agree with the frame
    // while another derived its own idea of the grid.
    constexpr double kBlockStageW = bps::library::kStageWidth, kBlockStageH = bps::library::kStageHeight;
    // The default text size — in STAGE units, like every meta `fontSize` — for a
    // block whose meta names none. ONE value, because three surfaces render
    // every block and they have to agree: the Edit canvas
    // (EditableCanvasLabel: `tmeta.fontSize ?? 16`), the preview pane
    // (DesignPreview: `el.meta.fontSize ?? 16`) and this engine's outputs.
    // The engine fell back to 24 here, so a block with no meta — which is
    // exactly what a newly created text item is, the Metadata palette's insert
    // included — drew at 16 on both QML surfaces and at 24 on the outputs: one
    // item at two sizes, and the size the operator could NOT see (the output)
    // was the larger one. This is the same class of fix as the alignment
    // mismatch below: the engine agrees with what the canvas shows. A registered
    // template block never reaches this fallback (DesignCatalogs' Text() always
    // writes a fontSize); it is for the hand-made items.
    constexpr double kDefaultTextFontSize = 16.0;
    // THE SPLIT COMES FROM ITS ONE HOME — the same call the UI's live preview
    // makes, so the tile and every real output read one design the same way.
    // `demoted`/`stageCovered` below are this build's report of it.
    const SceneBuilder::BlockLayerPlan plan = SceneBuilder::PlanBlockLayers(blocks);
    const std::vector<bool>& isBackdrop = plan.underVideo;
    const bool stageCovered = plan.stageCovered;
    int n = 0;
    // How many of this scene's blocks ended up UNDER the compositor's video
    // layer (PlanBlockLayers' split — plus any block whose own "underVideo"
    // put it there). Reported once per scene below: it is the only thing that
    // says, end to end, that the design's backdrop is behind the live feed
    // rather than over it.
    int demoted = 0;
    size_t blockIndex = 0;
    for (const ContentBlock& block : blocks) {
        ++n;
        const std::string objId = std::format("{}{}", tag, n);
        const rendering::Rect rect(
            static_cast<float>(block.x / kBlockStageW * size.width),
            static_cast<float>(block.y / kBlockStageH * size.height),
            static_cast<float>(block.width / kBlockStageW * size.width),
            static_cast<float>(block.height / kBlockStageH * size.height));
        // The block's corner radius is authored on the SAME 754x428 stage grid
        // as its geometry (DesignCatalogs scales a catalog radius by the x
        // factor too), so it scales like the WIDTH does. Without this the
        // radius never reached the shape at all: a template's rounded band or
        // panel ("Box"'s radius 8, "Trendy Curved"'s 30, the Table designs' 20)
        // drew with SQUARE corners on every engine output — NDI, the distributed
        // frames and recordings — while the canvas, the library cards and the
        // in-app previews rounded it through Qt's Rectangle { radius }.
        const float radiusOut = static_cast<float>(
            std::max(0.0, block.style.cornerRadius) / kBlockStageW * size.width);
        const bool boxKind = IsBoxKind(block);
        // AN EXPLICIT CHOICE BEATS THE INFERENCE. The geometry above is right for
        // the designs the app ships, but a design author who means something else
        // says so in the block's own meta and is obeyed: "underVideo": true puts
        // the block's own paint under the compositor's video layer (a live feed
        // paints over it), "underVideo": false keeps it over the feed whatever
        // its size. Absent = the leading-run rule.
        const std::string underVideo = MetaBoolToken(block.metaJson, "underVideo");
        bool backgroundSlab = isBackdrop[blockIndex];
        if (!underVideo.empty())   // the design's own statement on this block
            backgroundSlab = (underVideo == "true");
        ++blockIndex;
        // An OVERLAY's own blocks (placeUnderSlide decides their group) keep the
        // layer they were given: an overlay is a self-contained picture, and
        // demoting its slab into the scene's background would put it under the
        // style's own background image.
        const std::string_view blockLayer =
            (backgroundSlab && layer == "text") ? std::string_view("bg") : layer;
        if (blockLayer == "bg")
            ++demoted;
        if (block.kind == "text") {
            // THE TEXT'S OWN BOX, painted first (same layer → under its text):
            // a text item carries its own Background on the canvas (EditScreen's
            // itemBackground target → style.backgroundColor, the #aarrggbb tint
            // palette "transparent black" is one of), and DesignPreview draws
            // exactly that fill behind every text block it renders — the canvas,
            // the library cards, and the in-app tile/output-window previews.
            // The engine rendered the TextObject and NOTHING else, so a template
            // whose text sits on a translucent band (the shipped scripture
            // recipe's #66000000 box) lost that band on every ENGINE output —
            // the NDI feed, the distributed frames and recordings — while the
            // preview showed it: the reported "the template defined a transparent
            // black bg behind the text but it doesn't show". A box/shape BLOCK
            // was already honored below; a text block's own box was not.
            //
            // Its cornerRadius rounds it like the canvas does; a border stays
            // unsupported (ShapeComponent has always modeled a fill only — see
            // the shape branch below).
            const rendering::Color boxFill =
                StyleBuilder::ParseColor(block.style.backgroundColor);
            if (boxFill.a > 0.0f) {
                // THE ITEM'S OWN BOX rides the same decision as a box block: a
                // text item that fills the stage with a fill IS the design's
                // backdrop (its TEXT still renders over the live feed — only the
                // paint moves below the video layer).
                const std::string boxId = objId + "box";
                auto box = std::make_shared<rendering::ShapeObject>(boxId, boxId);
                box->SetRectangle(boxFill, radiusOut);
                box->SetBounds(rect);
                box->SetLayer(std::string(blockLayer));
                (void)engine.AddObject(sceneId, box, std::string(blockLayer));
            }
            auto text = std::make_shared<rendering::TextObject>(objId, objId, block.text);
            text->SetBounds(rect);
            auto styleComp = std::make_shared<rendering::TextStyleComponent>();
            styleComp->style.size = static_cast<float>(
                MetaNumber(block.metaJson, "fontSize", kDefaultTextFontSize) * size.height / kBlockStageH);
            styleComp->style.color = StyleBuilder::ParseColor(
                MetaString(block.metaJson, "color").empty()
                    ? "#ffffff" : MetaString(block.metaJson, "color"));
            styleComp->style.align = ParseAlign(MetaString(block.metaJson, "align"));
            styleComp->style.valign = ParseVAlign(MetaString(block.metaJson, "verticalAlign"));
            styleComp->style.bold = MetaBool(block.metaJson, "bold");
            // CANVAS-DEFAULT ALIGNMENT MISMATCH: EditableCanvasLabel centers
            // BOTH axes when a meta field is absent (TextItemPanel:
            // align ?? "center", verticalAlign ?? "center"), but ParseAlign
            // /ParseVAlign fall back to Left/Top — so a plain block rendered
            // top-left in the engine while every QML surface showed it
            // centered. On a multi-line overflow the top-left anchoring is
            // what pushed the block's tail BELOW the frame while the canvas
            // showed the same box centered (part of the NDI "text not fit"
            // report). Missing values now read as Center/Middle, matching
            // the canvas; an explicit "left"/"top" still wins.
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
            // The typography knobs the canvas/DesignPreview honor and the
            // engine silently dropped — lineHeight (QML's Text default 1.2,
            // an empty line of black serif overflows a verse box at output
            // scale) and letterSpacing. letter-spacing is PER GAP (n-1 gaps
            // for n glyphs), the same convention TextLayout's width math uses.
            //
            // lineHeight → lineSpacing: Qt renders Text.lineHeight as a
            // PROPORTIONAL multiplier of the font's OWN natural line height
            // (a per-font number — its ascent/descent/lineGap), so the
            // engine's additive lineSpacing must come from the font's real
            // metrics too: lineStep = lineHeight · naturalStep, lineSpacing
            // = lineStep − fontSize. The old (lineHeight−1)·fontSize formula
            // forced every font onto the SAME line grid; a font with taller
            // natural metrics (most display serifs vs Segoe UI) drew lines
            // FARTHER apart than the fit solver measured — block taller than
            // the box, last verse line clipped at the frame bottom with one
            // font and fine with another (the "auto size/grow breaks when I
            // change the font" report; the screenshots pair: same verse,
            // same box, one font fits, one clips). Unresolvable metrics
            // (builtin font, off-Windows) keep the legacy formula.
            {
                const double lh = MetaNumber(block.metaJson, "lineHeight", 1.2);
                const float sizePx = styleComp->style.size;   // the output-scaled font size
                rendering::FontManager::LineMetrics lm{};
                if (rendering::RenderEngine::Instance().Fonts().SystemLineMetrics(
                        styleComp->style.fontId, sizePx, styleComp->style.bold,
                        styleComp->style.italic, lm)) {
                    styleComp->style.lineSpacing = lm.baselineStep * static_cast<float>(lh) - sizePx;
                } else {
                    styleComp->style.lineSpacing = static_cast<float>(
                        (lh - 1.0) * MetaNumber(block.metaJson, "fontSize", kDefaultTextFontSize)
                        * size.height / kBlockStageH);
                }
            }
            {
                const double ls = MetaNumber(block.metaJson, "letterSpacing", 0.0);
                styleComp->style.letterSpacing = static_cast<float>(ls * size.width / kBlockStageW);
            }
            // autoSize — the FULL vocabulary the template editor writes
            // (TextItemPanel: "none" | "shrinkToFit" | "growToFit"; legacy
            // "shrink"/"grow" from DesignCatalogs arrive too, and
            // DesignPreview has always accepted both spellings).
            //   grow/growToFit — BisectFittedSize bracketed to the full
            //     [3px, 400 stage px] range: the LARGEST size that fits
            //     (the canvas solver's `lo`).
            //   shrink/shrinkToFit/none — BisectFittedSize bracketed to
            //     [3px, set size]: the set size is the CEILING, squeeze
            //     until the layout fits (the canvas rule for every non-grow
            //     mode, "none" included).
            // THE CANVAS RULE FOR EVERYTHING ELSE ("none" included): the box
            // is the master and the text must always sit inside it — render
            // at the set size while it fits, SQUEEZE the moment it doesn't.
            // EditScreen.recomputeFit and DesignPreview.solveFit both solve
            // for EVERY mode with exactly this sentence; the engine stopped
            // at the set size instead, so a none-mode block that fit on the
            // canvas overflowed on the outputs — the reported "NDI output
            // renders the verse bigger than the canvas and clips the last
            // line" (GDI+ serif runs wider than the same string in the QML
            // renderer, so a size that fit 754×428 left the 1920×1080
            // re-layout wrapping more lines and pushing the tail off the
            // frame). ALWAYS solving (with the set size as the ceiling) makes
            // the engine agree with both QML surfaces by construction.
            const std::string autoSize = MetaString(block.metaJson, "autoSize");
            const float minPx =
                static_cast<float>(3.0 * size.height / kBlockStageH);   // the canvas solver's own 3px floor, output-scaled
            if (autoSize == "grow" || autoSize == "growToFit") {
                // GROW: the LARGEST size that fits — full stop (the canvas
                // solver's `lo`, bracketed up to 400 stage px like its [3, 400]).
                // The set size is neither floor nor ceiling: a short verse
                // grows past it, an overflowing one descends below it — the
                // box is the master even for grow. The floor (3px, output-
                // scaled) protects only the degenerate 1×1-box case.
                styleComp->style.size = BisectFittedSize(
                    block.text, styleComp->style, text->Bounds(),
                    rendering::RenderEngine::Instance().Fonts(), minPx,
                    static_cast<float>(400.0 * size.height / kBlockStageH));
            } else {
                // SHRINK/NONE: the set size is the CEILING — render at the
                // set size while it fits, squeeze the moment it doesn't
                // ("none" included; the box is the master on every surface).
                styleComp->style.size = BisectFittedSize(
                    block.text, styleComp->style, text->Bounds(),
                    rendering::RenderEngine::Instance().Fonts(), minPx,
                    styleComp->style.size);
            }
            text->AddComponent(styleComp);
            text->SetLayer(std::string(layer));
            (void)engine.AddObject(sceneId, text, std::string(layer));
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
    // The slide's own content pass (never an overlay's — an overlay keeps the
    // layer its caller named): state the split this build produced. A scene
    // whose designs never cover the stage honestly reports 0, which is the
    // "these blocks are decoration OVER the feed, as designed" case.
    if (layer == "text")
        ReportLayerState(sceneId, "backdrop",
                         std::format("{} of {} blocks under the video layer (stage covered: {})",
                                     demoted, n, stageCovered ? "yes" : "no"));
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

// One-shot-per-scene memory of the last state REPORTED per compositor layer
// (media, taken input), so the diagnostics below write a line when a layer
// changes state (frame arrives, frame clears, tap taken/cleared, object
// appears) instead of 60 times a second.
std::mutex g_mediaLogMutex;
std::map<std::string, std::string> g_mediaStateLog;

// Writes `label layer <state> [sceneId]` once per scene per STATE CHANGED.
// Both compositor layers report through here so their lines have one shape and
// one keying rule; the map key is sceneId + label so the two layers of a scene
// never overwrite each other's memory.
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
                         const rendering::Size& size, bool showOverlay = true) {
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

    // THE ENGINE-SIDE HALF OF THE TAKEN-INPUT CHAIN (the QML half is
    // EngineBridge's own preview taps; the state half is
    // CompositorState::SetTakenInput). The in-app monitor preview draws the
    // tap from the QML provider, so a monitor preview that shows the camera
    // while NDI/HDMI stay black means either the device id never reached the
    // compositor, or the PAL tap had no pixels for it, or the object was
    // refused — and nothing said which. One line per scene per state:
    //   * "no input taken" here + a picture in the app ⇒ the take never
    //     mirrored into CompositorState (the UI's tap is not the engine's);
    //   * "no pixels" here with a device named ⇒ the PAL tap the renderer
    //     reads is not the one the UI started (wrong id, or a refused tap);
    //   * pixels and MISSING ⇒ AddObject refused (bounds/layer), not the tap.
    {
        const bool present = engine.GetObject(sceneId, "takeninput").ok();
        const std::string state = taken.deviceId.empty()
            ? std::string("absent — no input taken")
            : (takenImg.empty()
               ? std::format("absent — PAL tap gave no pixels for device '{}' (kind '{}')",
                             taken.deviceId, taken.kind)
               : std::format("{}{} — device '{}' (kind '{}') {}x{} px -> {}x{} px",
                             present ? "present" : "MISSING",
                             present ? "" : " (AddObject refused)",
                             taken.deviceId, taken.kind, takenImg.width, takenImg.height,
                             size.width, size.height));
        ReportLayerState(sceneId, "taken input", state);
    }

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

    // THE ENGINE-SIDE HALF OF THE "media hand-off" DIAGNOSTIC (the UI half is
    // LiveOutputService::pushMediaFrameToCompositor, the state side is
    // CompositorState::SetMediaFrame): this line says what the media layer
    // looked like the moment THIS scene was composed — whether the compositor
    // had pixels to give it at all, and whether the object ended up in the
    // scene. "The tile has the picture but the output is black" is then one
    // grep away from an answer instead of a guess:
    //   * "no frame" here + a UI hand-off line with dimensions ⇒ the engine
    //     copy was clear by the time this scene built (the frame is being
    //     cleared or never set for this take);
    //   * dimensions here but the object ABSENT ⇒ AddObject refused (bad size
    //     or missing layer);
    //   * dimensions AND present ⇒ the layer is in the scene, and the fault is
    //     downstream (texture upload / draw order), not in the hand-off.
    // Written once per scene per STATE, not per render tick.
    {
        const bool present = engine.GetObject(sceneId, "mediaonair").ok();
        const std::string state = mediaImg.empty()
            ? std::format("absent — compositor frame empty (media {}x{}, {} bytes)",
                          media.width, media.height, media.rgba.size())
            : std::format("{}{} — compositor frame {}x{} -> {}x{} px{}",
                          present ? "present" : "MISSING",
                          present ? "" : " (AddObject refused)",
                          media.width, media.height,
                          mediaImg.width, mediaImg.height, mediaImg.pixels.size(),
                          sceneId.find("@gated") != std::string::npos ? " [gated scene]" : "");
        ReportLayerState(sceneId, "media", state);
    }

    // Overlays: the SAME block-rendering pass the slide's own content uses
    // (AddStageBlocks), once per active overlay, in take order — id-tagged
    // so overlays can never collide with each other or the slide's own
    // objects. placeUnderSlide routes into "media" (below the slide's
    // text — a lower-third-BEHIND-text kind of layer) or "overlay" (above
    // everything, the more common placement) — the exact two groups
    // OutputMonitorTile.qml's own two Repeaters already split on.
    //
    // UNLIKE the taken-input/media layers above, overlays DO respect a
    // style gate (OutputStyleSpec::showOverlay, the Styles screen's own
    // pill): an output wearing a style with Overlay switched off shows
    // none, even while other outputs have the same overlay live — an
    // explicit, separate control from the Overlays pane's own take/clear,
    // same shape as the existing Shows/Media/Scripture/Table pills but with
    // no per-family template (overlays render from the design's own
    // blocks, not a style template). Folded into effectiveOverlays rather
    // than short-circuiting the whole function: a gate flip must still
    // clear whatever overlay objects a PRIOR (gate-on) call already baked
    // into this scene, which only the signature-diff cache-eviction below
    // does correctly.
    const std::vector<presentation::CompositorState::ActiveOverlay> overlays = showOverlay
        ? presentation::CompositorState::Instance().GetActiveOverlays()
        : std::vector<presentation::CompositorState::ActiveOverlay>{};
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
        for (size_t i = 0; i < ov.blocks.size(); ++i) {
            const std::string id = std::format("{}{}", tag, i + 1);
            cache.objectIds.push_back(id);
            // A text block whose own style carries a box fill is TWO objects
            // (AddStageBlocks adds `<id>box` under the text) — registering only
            // the text would leave every previous overlay's box in the scene
            // when the overlay set changes (AddObject has no upsert; the diff
            // loop above is the ONLY thing that removes them). A missing id is
            // a harmless no-op for the removals.
            cache.objectIds.push_back(id + "box");
        }
    }
}

} // namespace

// ---------------------------------------------------------------------------
// The backdrop rule (SceneBuilder::PlanBlockLayers) — a member of the class, so
// it is defined out here rather than in the anonymous namespace above, which is
// where a member definition may not live.
//
// WHICH OF A DESIGN'S BLOCKS PAINT UNDER THE COMPOSITOR'S VIDEO LAYER — the
// rule's ONE home. The scene builder below paints scenes with it, and the UI's
// live preview asks for it (EngineBridge::backdropBlockIndexes) instead of
// re-deriving it: a tile that decides for itself what covers the feed is how
// "it shows on the monitor but the output goes black" happens, and two copies
// of a rule this geometric drift apart the moment either one changes.
//
// THE COMPOSITOR'S VIDEO LAYER HAS A DEFINED Z-POSITION: a design's BACKGROUND
// goes under it, its content over it. The layers a scene is built from are,
// bottom to top — "bg" (the style's colour and background image, then the
// design's own background), "media" (the compositor: a taken camera/screen
// input, media on air, then the under-slide overlay group), "text" (the
// slide's/template's content) and "overlay" (the over-slide group). A design's
// background is expressed as a box block, and every box block used to land on
// the CONTENT layer — i.e. above the video layer — so a full-stage slab, or a
// set of leading panels that together cover the stage, painted over a live
// feed: the reported "only media doesn't go" (opaque black output while the
// monitor tile, which draws the tap itself, looked right). A live feed is the
// output's picture; a design may decorate OVER it (bands, panels, a text item's
// own box) but never BURY it.
//
// So: a design's BACKDROPS — the paint a block puts behind the slide's content,
// i.e. a box/shape block, or a text block's OWN background box (content pages
// express their backdrop that way: one text item filling the stage with an
// opaque fill, which is how this was reported) — are treated as background once
// the LEADING ones together reach the stage. That catches one full-stage slab
// and a "panel + bottom bar" pair that only cover the stage together, as the ONE
// backdrop they are: demoting just the block that completed the cover left the
// earlier panels painting across the feed. A band, a sidebar or a chip whose
// leading run never reaches the stage keeps its content position and stays above
// the video, exactly as designed, and everything after a completed backdrop is
// content (decoration on top of it). A text block with no box of its own
// contributes nothing to the run and does not end it; any other block kind does
// end it.
//
// A block that states its own "underVideo" is left out of the run's bounds (its
// placement is not the geometry's business) but does not break it: the author
// decided that one.

SceneBuilder::BlockLayerPlan SceneBuilder::PlanBlockLayers(const std::vector<ContentBlock>& blocks) {
    // The stage grid has ONE definition on this side too (bps::library's
    // kStageWidth/kStageHeight, beside the ContentBlock they describe; the QML
    // counterpart is qml/settings/StageGrid.qml) — this used to be a second copy
    // of the same two literals, which is how a surface could agree with the frame
    // while another derived its own idea of the grid.
    constexpr double kBlockStageW = bps::library::kStageWidth, kBlockStageH = bps::library::kStageHeight;
    BlockLayerPlan plan;
    plan.underVideo.assign(blocks.size(), false);
    double accX0 = 0.0, accY0 = 0.0, accX1 = 0.0, accY1 = 0.0;
    bool any = false, covered = false;
    for (size_t i = 0; i < blocks.size() && !covered; ++i) {
        const ContentBlock& b = blocks[i];
        if (b.kind == "text" && !HasOwnBox(b))
            continue;                 // the design's text: no backdrop, run goes on
        if (!IsBoxKind(b) && !HasOwnBox(b))
            break;                    // clock/timer/image/media: the run ends here
        if (!MetaBoolToken(b.metaJson, "underVideo").empty())
            continue;                 // the author decided this one — not the rule
        const double bx0 = b.x, by0 = b.y;
        const double bx1 = b.x + b.width, by1 = b.y + b.height;
        if (!any) {
            accX0 = bx0; accY0 = by0; accX1 = bx1; accY1 = by1;
            any = true;
        } else {
            accX0 = std::min(accX0, bx0);
            accY0 = std::min(accY0, by0);
            accX1 = std::max(accX1, bx1);
            accY1 = std::max(accY1, by1);
        }
        constexpr double kStageTolX = 0.02 * kBlockStageW;
        constexpr double kStageTolY = 0.02 * kBlockStageH;
        covered = accX0 <= kStageTolX && accY0 <= kStageTolY
            && accX1 >= kBlockStageW - kStageTolX
            && accY1 >= kBlockStageH - kStageTolY;
        if (covered) {
            // The whole leading run IS the backdrop — every block that got it
            // there, not just the last one.
            for (size_t j = 0; j <= i; ++j)
                if (IsBoxKind(blocks[j]) || HasOwnBox(blocks[j]))
                    plan.underVideo[j] = true;
        }
    }
    plan.stageCovered = covered;
    return plan;
}

// The two text regions of a built-in layout preset, as the engine renders them
// (Title, Body). Both preset paths — a family riding a preset key, and the
// style's own key with no baked blocks — draw exactly these, so they draw them
// through one function rather than two copies of the same eight lines.
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
        auto body = std::make_shared<rendering::TextObject>("body", "Body", slide.text);
        body->SetBounds(bodyRect);
        body->SetLayer("text");
        (void)engine.AddObject(sceneId, body, "text");
    }
}

// WHAT A SLIDE LOOKS LIKE ON AN OUTPUT WEARING THIS STYLE — see the header for
// the contract. `applyGates` is the caller's answer to "is this output's style
// gating content?": the gated build (and the live preview, which shows what the
// gates leave on air) says yes; the plain styled build renders the slide whether
// or not the gates would admit it, exactly as it always has.
SceneBuilder::SlideComposition SceneBuilder::ComposeSlide(const Slide& slide,
                                                         const OutputStyleSpec& style,
                                                         bool applyGates) {
    SlideComposition comp;
    comp.contentType = SlideRules::ContentType(slide);
    comp.familyIdx = SlideRules::FamilyTemplateSlot(comp.contentType);

    // ---- The background the output paints ---------------------------------
    // THE PRECEDENCE, in one place, because this is the decision the monitor
    // tile and every real output have to agree on:
    //   * a style colour paints, unless clearBackgroundOnText hands the frame
    //     to a slide that carries a real colour of its own;
    //   * failing that, the slide's own colour;
    //   * failing all of those, NOTHING NAMED A COLOUR — and nothing named
    //     means NOTHING IS PAINTED: `paintBackground == false`, a KEYABLE
    //     frame, not an opaque black fill of the engine's own. An empty frame
    //     is "not painting yet": whatever is keyed, recorded or composited
    //     behind it shows through, and the first thing that really paints (a
    //     style colour, a design's own backdrop block, a background image,
    //     pushed content) fills it from there. This used to fall through to
    //     opaque black — read as "a projector's no-signal state" — which laid a
    //     black slab over the feed behind a live output before any content
    //     existed, instead of leaving the frame empty as it asks.
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
        // NOTHING NAMED A COLOUR: the frame paints NOTHING and stays keyable,
        // rather than filling itself with an opaque black of its own. An empty
        // frame is "not painting yet" — what is keyed, recorded or composited
        // behind it shows through — and the first thing that DOES paint (a
        // style colour, a design's own backdrop block, a background image, any
        // pushed content) fills it from there. `background` stays empty, which
        // is what tells this apart from a style that CHOSE transparency: the
        // two are the same pixels, and only the composition knows the
        // difference.
        comp.paintBackground = false;
    }

    // The style's background IMAGE rule (see styleImageSuppressed): independent
    // of which layout ends up owning the slide, so it is settled before the
    // gates can return early.
    comp.styleImageSuppressed = style.clearBackgroundOnText && !slide.blocks.empty();

    // ---- Which layout owns the slide -------------------------------------
    // The style's gates first (when the caller is applying them): a refused
    // family is background only. Then the style's layout, most specific first —
    // the family's own engine template, the whole-style one (gated by its
    // contentType, so a scripture-keyed style does not steamroll The Table's
    // layout), a family preset key, the slide's own blocks, and last the style's
    // own preset key. A slide with NO content at all never wears a template: its
    // decorative blocks have nothing to sit behind and would cover the
    // compositor's video layer outright (the blank GO LIVE carrier).
    if (applyGates && !SlideRules::StyleAdmits(style, slide)) {
        comp.source = SlideComposition::Source::Refused;
        return comp;
    }
    const bool slideHasContent = !slide.blocks.empty() || !slide.title.empty()
        || !slide.text.empty();
    if (slideHasContent && comp.familyIdx < 4
        && !style.familyTemplateBlocks[comp.familyIdx].empty()) {
        comp.source = SlideComposition::Source::FamilyTemplate;
        comp.blocks = BindTemplateBlocks(style.familyTemplateBlocks[comp.familyIdx], slide);
        return comp;
    }
    if (slideHasContent && !style.templateBlocks.empty()
        && (style.contentType.empty() || comp.contentType.empty()
            || comp.contentType == style.contentType)) {
        comp.source = SlideComposition::Source::StyleTemplate;
        comp.blocks = BindTemplateBlocks(style.templateBlocks, slide);
        return comp;
    }
    if (comp.familyIdx < 4 && !style.familyTemplateKeys[comp.familyIdx].empty()
        && style.familyTemplateBlocks[comp.familyIdx].empty()) {
        comp.source = SlideComposition::Source::FamilyPreset;
        comp.presetKey = style.familyTemplateKeys[comp.familyIdx];
        comp.preset = StyleBuilder::LayoutFor(comp.presetKey);
        return comp;
    }
    if (!slide.blocks.empty()) {
        comp.source = SlideComposition::Source::SlideBlocks;
        return comp;   // `blocks` stays empty: the caller draws the slide's own
    }
    comp.source = SlideComposition::Source::StylePreset;
    comp.presetKey = style.templateKey;
    comp.preset = StyleBuilder::LayoutFor(comp.presetKey);
    return comp;
}


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
                                           const OutputStyleSpec& style,
                                           rendering::Size sceneSize) {
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
    size_t familyIdx = SlideRules::FamilyTemplateSlot(SlideRules::ContentType(s));
    const std::string &famKey = familyIdx < 4 ? style.familyTemplateKeys[familyIdx]
                                              : std::string();
    const uint64_t famHash = familyIdx < 4 && !style.familyTemplateBlocks[familyIdx].empty()
                                 ? HashBlocks(style.familyTemplateBlocks[familyIdx])
                                 : 0;
    std::string id = SceneIdFor(p, s) + std::format("@s:{}_{}_{}_{}_{:016x}_{}_{:016x}",
                                                   style.backgroundColor,
                                                   style.templateKey,
                                                   style.clearBackgroundOnText ? 1 : 0,
                                                   style.backgroundImage,
                                                   HashBlocks(style.templateBlocks),
                                                   famKey,
                                                   famHash);
    // SIZE TAG (see the header): appended at the END so the gated variant's
    // id derivation ("@gated" + the styled suffix) keeps working. Absent for
    // the default 1920x1080, so every id — and therefore every cache key —
    // built before this parameter existed is byte-identical.
    const int sw = static_cast<int>(sceneSize.width);
    const int sh = static_cast<int>(sceneSize.height);
    if (sw > 0 && sh > 0 && (sw != 1920 || sh != 1080))
        id += std::format("@size:{}x{}", sw, sh);
    return id;
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

    // REFUSED: a background-only variant of the styled scene. Id carries a
    // "gated" tag + the same style fingerprint, so (a) it never collides
    // with the ungated scene of the same slide (another output may show it),
    // and (b) a gate flip or style edit re-keys it like any styled scene.
    const std::string sceneId =
        SceneIdFor(presentation, slide)
        + "@gated" + StyledSceneIdFor(presentation, slide, style, size).substr(SceneIdFor(presentation, slide).size());

    if (engine.GetScene(sceneId).ok()) {
        AddCompositorLayers(engine, sceneId, size, style.showOverlay);   // same cache-hit refresh as BuildSlideScene
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
    // The REFUSED (background-only) family composes its background through the
    // same single rule as every other build — ComposeSlide spells out the
    // precedence, including what an explicitly transparent style and an unnamed
    // one both mean (nothing painted, a keyable frame) — so a keyed output
    // stays keyed here instead of going opaque black over whatever it is keyed
    // onto.
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
    AddCompositorLayers(engine, sceneId, size, style.showOverlay);
    Logger::Instance().Debug(std::format("Gated scene built: {} (family refused by style '{}')",
                                        sceneId, style.name), "SceneBuilder");
    return sceneId;
}

Result<std::string> SceneBuilder::BuildSlideScene(const Presentation& presentation,
                                                  const Slide& slide,
                                                  const OutputStyleSpec& style,
                                                  rendering::RenderEngine& engine,
                                                  rendering::Size size) {
    const std::string sceneId = StyledSceneIdFor(presentation, slide, style, size);

    // Already built (cache hit) — the STRUCTURE (which text/shape objects
    // exist for this slide+style) is reused as-is, but the compositor
    // layers (a taken camera/screen input, a playing media file, active
    // overlays) are dynamic, continuously-updating state that lives OUTSIDE
    // this fingerprint — they must still refresh on every call, cache hit
    // or not, or a taken camera/media freezes on whatever frame existed the
    // moment this scene was first built (see AddCompositorLayers/
    // UpsertVideoLayer's own comments).
    if (engine.GetScene(sceneId).ok()) {
        AddCompositorLayers(engine, sceneId, size, style.showOverlay);
        return sceneId;
    }

    auto scene = engine.CreateScene(sceneId, slide.title, size);
    if (!scene.ok()) return scene.error();

    // Layer order: background -> video -> image -> text -> overlay.
    // The compositor's video layer ("media" — the taken camera/screen input and
    // media on air) sits between the scene's BACKGROUND and its CONTENT, and
    // AddStageBlocks keeps a design's own background slabs under it: a live feed
    // is the output's picture, and only decoration (bands, panels, text) may be
    // drawn over it. See AddStageBlocks' leading-run note.
    (void)engine.AddLayer(sceneId, rendering::Layer("bg", "Background",
                                                    rendering::LayerKind::Background, 0));
    (void)engine.AddLayer(sceneId, rendering::Layer("media", "Media",
                                                    rendering::LayerKind::Video, 1));
    (void)engine.AddLayer(sceneId, rendering::Layer("text", "Text",
                                                    rendering::LayerKind::Text, 2));
    (void)engine.AddLayer(sceneId, rendering::Layer("overlay", "Overlay",
                                                    rendering::LayerKind::Overlay, 3));

    // ---- Background ------------------------------------------------------
    // THE COMPOSITION DECIDES IT (ComposeSlide — the same call the live preview
    // makes), this renders it. FreeShow's Output.svelte composition: the style's
    // background (IMAGE first, colour beneath it) paints behind everything;
    // clearStyleBackgroundOnText lets a slide that carries its own colour keep
    // it. A background the spec does not name (ParseColor("")->a == 0) paints
    // NOTHING: the scene is flagged keyable, so a live output never lays an
    // opaque slab of the engine's own over whatever is keyed behind it.
    const SlideComposition comp = SceneBuilder::ComposeSlide(slide, style);
    const bool slideOwnsBackground = comp.slideOwnsBackground;
    // paintBackground implies a NAMED colour (see ComposeSlide), so the default
    // below is unreachable belt-and-braces rather than a paint of its own.
    const rendering::Color bg = comp.background.empty()
        ? rendering::Color(0.0f, 0.0f, 0.0f, 1.0f)
        : StyleBuilder::ParseColor(comp.background);
    // EXPLICIT "transparent" vs "NOTHING NAMED": the SAME pixels (ParseColor
    // alpha 0), kept distinct because the composition records which it was.
    //   * "" (no colour chosen) paints nothing, exactly like the explicit case —
    //     see ComposeSlide; an empty frame is "not painting yet".
    //   * "transparent" (chosen on purpose) marks the CHOICE in
    //     SlideComposition::explicitTransparent, which is what the monitor tile
    //     reads to draw its checkerboard. The engine's frame is keyable either
    //     way — the reported "a transparent template does not show as
    //     transparent" bug was this branch falling through to black.
    const bool paintBackground = comp.paintBackground;

    // DIAGNOSTIC (VGR_SCENEDIAG=1), one line per scene build: the background
    // decision the REAL output renders with. The reported "the output preview
    // shows the template but the real output has no opaque black" is precisely a
    // difference between this decision and what the in-app preview resolves for
    // itself, and the two inputs that can cause it (a style colour of
    // "transparent", and clearBackgroundOnText dropping the style colour when
    // the slide carries its own) are both invisible from the outside. Printed
    // before the branch so a keyed frame states WHY it is keyed.
    // The style background IMAGE gets the same treatment on the same line: a
    // branded backdrop that vanishes the moment content goes on air is the
    // clearBackgroundOnText gate (see the image branch below), and a style whose
    // image is set but whose FILE will not load is a different bug entirely —
    // "imgSet=1 imgBlocked=0 img=0x0" names the second, "imgBlocked=1" the
    // first, with no test-render needed.
    // std::getenv, not qEnvironmentVariableIsSet: the engine module is Qt-free
    // (it has no QtCore include path) and this file already talks to the
    // environment through the standard library — see BroadcastProviders.cpp.
    if (const char* diag = std::getenv("VGR_SCENEDIAG"); diag && *diag) {
        // INFO, not WARNING: a warning is what the app's own message policy
        // forwards to the UI as a notification card, so a diagnostic line on
        // every scene build popped a toast over the operator's live screen
        // whenever VGR_SCENEDIAG was set. A diagnostic belongs in engine.log.
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

    // Style background IMAGE — painted over the colour, under everything else
    // (FreeShow's style backgroundImage, cover-fit). Fingerprinted into the
    // scene id, so a new image always lands as a rebuild. clearBackgroundOnText
    // hides it outright while a slide is on air (FreeShow's Output.svelte:
    // styleBackground is "" whenever clearStyleBackgroundOnText && (slide ||
    // background) — unconditional on the slide's OWN background, unlike the
    // colour precedence a few lines up). Mirrors OutputMonitorTile.qml's own
    // suppressStyleBgImage gate, which already got this right on the preview
    // side — a busy branded background must never fight with live text.
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

    // Taken camera/screen input, media file on air, active overlays — see
    // AddCompositorLayers' own comment. Runs ONCE here, before every content
    // branch below (each returns early after its own content), so every
    // path gets these layers regardless of which one executes.
    AddCompositorLayers(engine, sceneId, size, style.showOverlay);

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
    // ---- Content: WHICH LAYOUT OWNS THIS SLIDE ---------------------------
    // The decision is ComposeSlide's — the same call the live preview makes, so
    // the two cannot read one slide differently. Its cases, in order: the
    // style's per-family engine template (slide content bound in), the
    // whole-style template (gated by its contentType), a family preset key, the
    // slide's own blocks, and last the style's own preset key. A slide with NO
    // content at all never wears a template: the blank GO LIVE carrier's history
    // is on ComposeSlide, and the rule that keeps it that way is one line there.
    switch (comp.source) {
    case SlideComposition::Source::FamilyTemplate: {
        const int n = AddStageBlocks(engine, sceneId, comp.blocks, size, "tplf");
        Logger::Instance().Debug(std::format("Scene built: {} (style '{}', family {} template {} blocks)",
                                            sceneId, style.name, comp.familyIdx, n), "SceneBuilder");
        return sceneId;
    }
    case SlideComposition::Source::StyleTemplate: {
        const int n = AddStageBlocks(engine, sceneId, comp.blocks, size, "tpl");
        Logger::Instance().Debug(std::format("Scene built: {} (style '{}', template {} blocks)",
                                            sceneId, style.name, n), "SceneBuilder");
        return sceneId;
    }
    case SlideComposition::Source::FamilyPreset: {
        AddPresetTextObjects(engine, sceneId, size, comp.preset, slide);
        Logger::Instance().Debug(std::format("Scene built: {} (style '{}', family {} preset '{}')",
                                            sceneId, style.name, comp.familyIdx, comp.presetKey),
                                 "SceneBuilder");
        return sceneId;
    }
    case SlideComposition::Source::SlideBlocks: {
        const int n = AddStageBlocks(engine, sceneId, slide.blocks, size, "blk");
        Logger::Instance().Debug(std::format("Scene built: {} (style '{}', {} blocks)",
                                            sceneId, style.name, n), "SceneBuilder");
        return sceneId;
    }
    case SlideComposition::Source::StylePreset:
        break;   // the style's own built-in layout: the two text regions below
    case SlideComposition::Source::Refused:
        // Only reachable through the gated build, which returns before this
        // point (see ComposeSlide's `applyGates`); the plain styled build
        // renders the slide's content as it always has.
        break;
    }

    // ---- The style's own layout preset ------------------------------------
    // Two text regions on the preset's own rectangles (LayoutFor). This is also
    // where a style whose templateKey names a design that no longer exists
    // lands: LayoutFor falls back to the safe default full layout for an
    // unknown key.
    AddPresetTextObjects(engine, sceneId, size, comp.preset, slide);
    Logger::Instance().Debug(std::format("Scene built: {} (style '{}')", sceneId, style.name),
                             "SceneBuilder");
    return sceneId;
}

} // namespace bps::presentation
