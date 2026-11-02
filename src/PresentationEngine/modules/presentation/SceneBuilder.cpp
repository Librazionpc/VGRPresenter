#include "modules/presentation/SceneBuilder.hpp"
#include "modules/presentation/TextFormat.hpp"
#include "modules/presentation/TemplateTextMetrics.hpp"

#include "core/logging/Logger.hpp"
#include "core/config/Json.hpp"
#include "modules/library/DesignLibrary.hpp"
#include "modules/presentation/CompositorState.hpp"
#include "modules/presentation/BookFill.hpp"
#include "modules/presentation/PresentationTemplates.hpp"
#include "modules/presentation/SlideRules.hpp"
#include "modules/rendering/Effects.hpp"
#include "modules/rendering/RenderEngine.hpp"
#include "modules/rendering/RenderObject.hpp"
#include "modules/rendering/Scene.hpp"
#include "modules/rendering/TextEngine.hpp"
#include "platform/IVideo.hpp"
#include "platform/PlatformAccessor.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <charconv>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <fstream>
#include <format>
#include <map>
#include <mutex>
#include <QFont>
#include <QTextBoundaryFinder>
#include <QString>
#include <set>
#include <string_view>

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

std::string ApplyTextCase(std::string_view text, std::string_view textCase) {
    const QString input = QString::fromUtf8(text.data(), static_cast<qsizetype>(text.size()));
    QString output = input;
    if (textCase == "upper") {
        output = input.toUpper();
    } else if (textCase == "lower") {
        output = input.toLower();
    } else if (textCase == "capitalize") {
        output = input.toLower();
        QTextBoundaryFinder finder(QTextBoundaryFinder::Word, output);
        qsizetype start = 0;
        while (true) {
            const qsizetype end = finder.toNextBoundary();
            if (end < 0)
                break;
            for (qsizetype i = start; i < end; ++i) {
                if (output.at(i).isLetterOrNumber()) {
                    output[i] = output.at(i).toTitleCase();
                    break;
                }
            }
            start = end;
        }
    } else {
        return std::string(text);
    }
    const QByteArray utf8 = output.toUtf8();
    return std::string(utf8.constData(), static_cast<size_t>(utf8.size()));
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
    if (a == "justify") return rendering::TextAlign::Justify;
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

// ---- a placed video item's poster frame -----------------------------------
// A "video" block on a slide is the FILE the operator placed. The engine has no
// file-video transport of its own (a playing media file is decoded by the UI and
// handed over as the compositor's frame — see CompositorState), so what a slide
// build can honestly paint is the video's OWN still: the same poster frame the
// canvas shows, made by the same platform frame source (Media Foundation), which
// is what makes the canvas and the outputs one picture instead of two opinions.
//
// Cached like the style background images: a scene is rebuilt per slide, and
// pulling a frame out of a 4K file is not something to repeat on every build.
std::map<std::string, rendering::RgbaImage> g_videoPosterCache;

rendering::RgbaImage LoadVideoPosterUncached(const std::string& path) {
    rendering::RgbaImage slot;
    auto pic = bps::media::MakePlatformFrameSource()->Grab(
        path, bps::media::MediaKind::Video, 0.0, 1920);
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
        return slot;
    }
    // A file Media Foundation will not open as video still has a shell picture
    // (the WIC/shell path LoadImageUncached already owns) — the same fallback
    // the media library's own tiles use for such files.
    return LoadImageCached(path);
}

rendering::RgbaImage LoadVideoPosterCached(const std::string& path) {
    {
        std::lock_guard<std::mutex> lock(g_imageMutex);
        auto it = g_videoPosterCache.find(path);
        if (it != g_videoPosterCache.end())
            return it->second;
    }
    rendering::RgbaImage poster = LoadVideoPosterUncached(path);
    std::lock_guard<std::mutex> lock(g_imageMutex);
    g_videoPosterCache[path] = poster;
    return poster;
}

// Contain-fit INSIDE a rect: the whole picture inside the item's own box, centred,
// never cropped. A placed media item is a window the operator sized — a portrait
// photo in a landscape box keeps its whole frame and letterboxes, which is the
// one rule the canvas applies to the same item (Image.PreserveAspectFit) and the
// one the engine already applies to a taken media file. Nothing here may exceed
// `box`: the engine has no clipping, so a cover fit would paint outside it.
rendering::Rect ContainRectIn(int imgW, int imgH, const rendering::Rect& box) {
    if (imgW <= 0 || imgH <= 0 || box.width <= 0 || box.height <= 0)
        return box;
    const float scale = std::min(box.width / imgW, box.height / imgH);
    const float w = imgW * scale, h = imgH * scale;
    return rendering::Rect(box.x + (box.width - w) / 2.0f,
                           box.y + (box.height - h) / 2.0f, w, h);
}

// The file a placed media item carries (its meta `src`). Empty = nothing to
// paint (an item whose picker never completed), which draws nothing at all
// rather than a guessed box.
std::string MediaSrcOf(const ContentBlock& block) {
    return MetaString(block.metaJson, "src");
}

// ---------------------------------------------------------------------------
// A placed media file's OWN values (the Edit screen's MEDIA tab)
// ---------------------------------------------------------------------------
// The tab writes them into the block's meta JSON under "media" — the one place
// the engine persists verbatim, since ContentStyle has no room for a flip, a
// crop or a filter set:
//
//   "media": { "fit": "cover", "flipH": true, "flipV": false,
//              "crop":   { "top": 0, "right": 0, "bottom": 0, "left": 0 },
//              "filter": { "hueRotate": 0, "invert": 0, "blur": 0,
//                          "grayscale": 0, "sepia": 0, "brightness": 1,
//                          "contrast": 1, "saturate": 1 } }
//
// The QML canvas draws from the same numbers (EditScreen's media picture), so a
// flip on screen is a flip on air — the point of doing the pixel work HERE, on
// the decoded file, rather than teaching every backend a new transform.
struct MediaItemValues {
    std::string fit;   // "" (contain) | contain | cover | fill | blur
    bool flipH = false;
    bool flipV = false;
    // Percentages of the source picture, 0–90.
    double cropTop = 0, cropRight = 0, cropBottom = 0, cropLeft = 0;
    rendering::SlideFilterValues filter;
    double sepia = 0.0;    // 0–1 (not part of the slide filter set)
    double opacity = 1.0;  // 0–1, the file's own transparency (a filter row)
    // FreeShow's own blend name ("normal", "screen", "soft-light" …) — how the
    // picture composites onto what is already under it. Empty = normal.
    std::string blend;

    bool HasCrop() const {
        return cropTop > 0 || cropRight > 0 || cropBottom > 0 || cropLeft > 0;
    }
    bool HasFilter() const { return !filter.IsIdentity() || sepia > 0.0 || opacity < 1.0; }
};

// The "media" object's own text, so a key lookup below can never wander into a
// neighbouring field (`top`/`left` are ordinary enough words to appear in other
// block metas). Brace-counted from the object's opening brace.
std::string_view MediaObjectOf(std::string_view metaJson) {
    const std::string needle = "\"media\"";
    const size_t at = metaJson.find(needle);
    if (at == std::string_view::npos)
        return {};
    size_t i = metaJson.find(':', at + needle.size());
    if (i == std::string_view::npos)
        return {};
    ++i;
    while (i < metaJson.size() && (metaJson[i] == ' ' || metaJson[i] == '\t'))
        ++i;
    if (i >= metaJson.size() || metaJson[i] != '{')
        return {};
    const size_t start = i;
    int depth = 0;
    for (; i < metaJson.size(); ++i) {
        if (metaJson[i] == '{')
            ++depth;
        else if (metaJson[i] == '}' && --depth == 0)
            return metaJson.substr(start, i - start + 1);
    }
    return {};
}

// The value after "key":, whitespace tolerated (a re-serialised meta may carry
// a space the compact writer does not).
std::string_view MediaRawValue(std::string_view obj, std::string_view key) {
    const std::string needle = "\"" + std::string(key) + "\"";
    const size_t at = obj.find(needle);
    if (at == std::string_view::npos)
        return {};
    size_t i = obj.find(':', at + needle.size());
    if (i == std::string_view::npos)
        return {};
    ++i;
    while (i < obj.size() && (obj[i] == ' ' || obj[i] == '\t'))
        ++i;
    const size_t end = obj.find_first_of(",}", i);
    if (end == std::string_view::npos)
        return {};
    return obj.substr(i, end - i);
}

double MediaNumber(std::string_view obj, std::string_view key, double dflt) {
    const std::string_view raw = MediaRawValue(obj, key);
    if (raw.empty())
        return dflt;
    const std::string num(raw);
    char* end = nullptr;
    const double out = std::strtod(num.c_str(), &end);
    return end == num.c_str() ? dflt : out;
}

bool MediaBool(std::string_view obj, std::string_view key, bool dflt) {
    const std::string_view raw = MediaRawValue(obj, key);
    if (raw == "true")
        return true;
    if (raw == "false")
        return false;
    return dflt;
}

std::string MediaText(std::string_view obj, std::string_view key) {
    const std::string_view raw = MediaRawValue(obj, key);
    if (raw.size() < 2 || raw.front() != '"' || raw.back() != '"')
        return {};
    return std::string(raw.substr(1, raw.size() - 2));
}

MediaItemValues ParseMediaItemValues(std::string_view metaJson) {
    MediaItemValues v;
    const std::string_view obj = MediaObjectOf(metaJson);
    if (obj.empty())
        return v;
    v.fit = MediaText(obj, "fit");
    v.flipH = MediaBool(obj, "flipH", false);
    v.flipV = MediaBool(obj, "flipV", false);
    v.cropTop = std::clamp(MediaNumber(obj, "top", 0.0), 0.0, 90.0);
    v.cropRight = std::clamp(MediaNumber(obj, "right", 0.0), 0.0, 90.0);
    v.cropBottom = std::clamp(MediaNumber(obj, "bottom", 0.0), 0.0, 90.0);
    v.cropLeft = std::clamp(MediaNumber(obj, "left", 0.0), 0.0, 90.0);
    v.filter.hueRotateDeg = std::clamp(MediaNumber(obj, "hueRotate", 0.0), 0.0, 360.0);
    v.filter.invert = std::clamp(MediaNumber(obj, "invert", 0.0), 0.0, 1.0);
    v.filter.blurPx = std::clamp(MediaNumber(obj, "blur", 0.0), 0.0, 100.0);
    v.filter.grayscale = std::clamp(MediaNumber(obj, "grayscale", 0.0), 0.0, 1.0);
    v.sepia = std::clamp(MediaNumber(obj, "sepia", 0.0), 0.0, 1.0);
    v.filter.brightness = std::clamp(MediaNumber(obj, "brightness", 1.0), 0.0, 2.0);
    v.filter.contrast = std::clamp(MediaNumber(obj, "contrast", 1.0), 0.0, 2.0);
    v.filter.saturate = std::clamp(MediaNumber(obj, "saturate", 1.0), 0.0, 2.0);
    v.opacity = std::clamp(MediaNumber(obj, "opacity", 1.0), 0.0, 1.0);
    v.blend = MediaText(obj, "blend");
    return v;
}

// ---- the pixel work those values describe ---------------------------------
// The engine's packed pixel is 0xAABBGGRR (Color::Pack), the same convention
// Effects.cpp's filter pass uses, so the channel helpers here are its own.
inline uint32_t PxChannel(uint32_t px, int shift) { return (px >> shift) & 0xFFu; }
inline uint32_t PxClamp(double v) {
    if (v <= 0.0) return 0;
    if (v >= 255.0) return 255;
    return static_cast<uint32_t>(v + 0.5);
}

rendering::RgbaImage CropPixels(const rendering::RgbaImage& src, const MediaItemValues& v) {
    if (!v.HasCrop() || src.width <= 0 || src.height <= 0)
        return src;
    const int w0 = src.width;
    const int h0 = src.height;
    const int x0 = std::clamp(static_cast<int>(w0 * v.cropLeft / 100.0), 0, w0 - 1);
    const int y0 = std::clamp(static_cast<int>(h0 * v.cropTop / 100.0), 0, h0 - 1);
    const int x1 = std::clamp(static_cast<int>(w0 * v.cropRight / 100.0), 0, w0 - 1);
    const int y1 = std::clamp(static_cast<int>(h0 * v.cropBottom / 100.0), 0, h0 - 1);
    const int w = w0 - x0 - x1;
    const int h = h0 - y0 - y1;
    if (w <= 0 || h <= 0)
        return src;
    rendering::RgbaImage out;
    out.width = w;
    out.height = h;
    out.pixels.resize(static_cast<size_t>(w) * h);
    for (int y = 0; y < h; ++y) {
        std::memcpy(&out.pixels[static_cast<size_t>(y) * w],
                    &src.pixels[static_cast<size_t>(y0 + y) * w0 + x0],
                    sizeof(uint32_t) * static_cast<size_t>(w));
    }
    return out;
}

rendering::RgbaImage FlipPixels(const rendering::RgbaImage& src, bool flipH, bool flipV) {
    if ((!flipH && !flipV) || src.width <= 0 || src.height <= 0)
        return src;
    rendering::RgbaImage out;
    out.width = src.width;
    out.height = src.height;
    out.pixels.resize(src.pixels.size());
    for (int y = 0; y < src.height; ++y) {
        const int sy = flipV ? src.height - 1 - y : y;
        for (int x = 0; x < src.width; ++x) {
            const int sx = flipH ? src.width - 1 - x : x;
            out.pixels[static_cast<size_t>(y) * src.width + x] =
                src.pixels[static_cast<size_t>(sy) * src.width + sx];
        }
    }
    return out;
}

// CSS sepia(amount): the published matrix, mixed by `amount` — the same op the
// canvas preview's shader runs, so the two surfaces agree on the tone.
void ApplySepiaPixels(rendering::RgbaImage& img, double amount) {
    if (amount <= 0.0 || img.empty())
        return;
    const double k = std::clamp(amount, 0.0, 1.0);
    for (uint32_t& px : img.pixels) {
        const double r = PxChannel(px, 0);
        const double g = PxChannel(px, 8);
        const double b = PxChannel(px, 16);
        const uint32_t a = PxChannel(px, 24);
        const double sr = 0.393 * r + 0.769 * g + 0.189 * b;
        const double sg = 0.349 * r + 0.686 * g + 0.168 * b;
        const double sb = 0.272 * r + 0.534 * g + 0.131 * b;
        px = (a << 24)
            | (PxClamp(b + (std::clamp(sb, 0.0, 255.0) - b) * k) << 16)
            | (PxClamp(g + (std::clamp(sg, 0.0, 255.0) - g) * k) << 8)
            | PxClamp(r + (std::clamp(sr, 0.0, 255.0) - r) * k);
    }
}

// The centre crop that makes a picture the box's own aspect ratio — how "cover"
// fills a box the engine cannot clip (see ContainRectIn's note): crop the SOURCE
// instead of painting outside the item, then contain-fit what is left, which is
// exactly the box.
rendering::RgbaImage CropToAspect(const rendering::RgbaImage& src, double boxW, double boxH) {
    if (src.width <= 0 || src.height <= 0 || boxW <= 0 || boxH <= 0)
        return src;
    const double srcAspect = static_cast<double>(src.width) / src.height;
    const double boxAspect = boxW / boxH;
    if (std::abs(srcAspect - boxAspect) < 0.001)
        return src;
    MediaItemValues crop;
    if (srcAspect > boxAspect) {
        // Wider than the box: take the sides in.
        const double keep = boxAspect / srcAspect;             // 0–1 of the width
        crop.cropLeft = crop.cropRight = (1.0 - keep) * 50.0;  // percentages
    } else {
        const double keep = srcAspect / boxAspect;
        crop.cropTop = crop.cropBottom = (1.0 - keep) * 50.0;
    }
    return CropPixels(src, crop);
}

// Crop, then flips, then the filter set in CSS order (hue, invert, blur,
// grayscale, sepia, brightness, contrast, saturate) — the order the canvas
// preview's shader applies, and the order FreeShow's CSS filter list does.
rendering::RgbaImage ApplyMediaPixelWork(rendering::RgbaImage picture, const MediaItemValues& v) {
    if (picture.empty())
        return picture;
    picture = CropPixels(picture, v);
    if (v.flipH || v.flipV)
        picture = FlipPixels(picture, v.flipH, v.flipV);
    if (v.filter.hueRotateDeg != 0 || v.filter.invert != 0 || v.filter.blurPx != 0
        || v.filter.grayscale != 0) {
        rendering::SlideFilterValues pre;
        pre.hueRotateDeg = v.filter.hueRotateDeg;
        pre.invert = v.filter.invert;
        pre.blurPx = v.filter.blurPx;
        pre.grayscale = v.filter.grayscale;
        rendering::ApplyFilters(picture, pre);
    }
    ApplySepiaPixels(picture, v.sepia);
    if (v.filter.brightness != 1.0 || v.filter.contrast != 1.0 || v.filter.saturate != 1.0) {
        rendering::SlideFilterValues post;
        post.brightness = v.filter.brightness;
        post.contrast = v.filter.contrast;
        post.saturate = v.filter.saturate;
        rendering::ApplyFilters(picture, post);
    }
    // The filter list's last row: the FILE's own opacity (CSS filter opacity).
    // Over the picture's alpha, which is what the canvas's item opacity does.
    if (v.opacity < 1.0) {
        for (uint32_t& px : picture.pixels) {
            const uint32_t a = PxChannel(px, 24);
            px = (PxClamp(a * v.opacity) << 24) | (px & 0x00FFFFFFu);
        }
    }
    return picture;
}

// Contain-fit math: the WHOLE image fits inside `stage`, centered, letterboxed
// by whatever is already painted underneath. This is what a media file on air
// gets: a take is a picture the operator chose, and cropping a portrait photo
// to a landscape stage (what cover-fit does) throws away most of it — the
// report "the image is portrait and you made it landscape". The QML tile draws
// the same file with PreserveAspectFit, so this is also the fit that keeps the
// tile and the outputs showing ONE picture.
rendering::Rect ContainRect(int imgW, int imgH, const rendering::Size& stage) {
    if (imgW <= 0 || imgH <= 0)
        return rendering::Rect(0, 0, static_cast<float>(stage.width), static_cast<float>(stage.height));
    const float scale = std::min(static_cast<float>(stage.width) / imgW,
                                 static_cast<float>(stage.height) / imgH);
    const float w = imgW * scale, h = imgH * scale;
    return rendering::Rect((stage.width - w) / 2.0f, (stage.height - h) / 2.0f, w, h);
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

// DOES THIS COMPOSITION PAINT ANYTHING AT ALL?
//
// A layout whose every text slot came out EMPTY after binding paints a blank
// frame: nothing draws an object, so the output is a black (or, under a
// transparent style, a fully see-through) picture while the app's own tiles —
// which draw the slide's own blocks — show the sermon. That is the report "the
// NDI feed says frames are going out and the monitor is black, it does not even
// show the background image": a style whose table/shows template is a design
// built out of palette FIELD tokens ("{table_text}", "{table_code_paragraphs}")
// owned the composition, its tokens had no values to fill, and the slide's own
// blocks — where the content actually is — were dropped for it.
//
// Non-text kinds always paint (a shape, a box, an image, a chip), and a text
// block paints when what is left in it is more than whitespace.
// ONE LINE PER SLIDE, not one per build: the live loop rebuilds a scene every
// time its fingerprint moves, and a diagnostic that repeats on each build is
// noise in the very log an operator is asked to read (measured: the style-less
// preview path rebuilt the same blank carrier scene several times a second).
std::mutex g_blankSlideMutex;
std::set<std::string> g_blankSlideLogged;

bool CompositionPaints(const std::vector<ContentBlock>& blocks) {
    for (const ContentBlock& b : blocks) {
        if (b.kind != "text") return true;
        if (b.text.find_first_not_of(" \t\r\n") != std::string::npos) return true;
    }
    return false;
}

// Can this slide feed a built-in PRESET? The preset path draws the slide's own
// title and body and NOTHING else (AddPresetTextObjects), so a slide that
// carries neither — content-family slides put their text in blocks — has
// nothing to give a preset, whatever layout it would have worn.
bool PresetHasContent(const Slide& slide) {
    return !slide.title.empty() || !slide.text.empty();
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

// A block's rect in OUTPUT pixels — the 754x428 design grid scaled onto the
// frame. ONE derivation, because every block pass (text/shape, and a bus
// window's live plane) has to land on exactly the rectangle the design names.
rendering::Rect BlockRect(const ContentBlock& block, const rendering::Size& size) {
    constexpr double kStageW = bps::library::kStageWidth, kStageH = bps::library::kStageHeight;
    return rendering::Rect(
        static_cast<float>(block.x / kStageW * size.width),
        static_cast<float>(block.y / kStageH * size.height),
        static_cast<float>(block.width / kStageW * size.width),
        static_cast<float>(block.height / kStageH * size.height));
}

// Defined below (one line per scene per STATE CHANGE). Declared here because
// AddStageBlocks is where a build reports the split it got from
// SceneBuilder::PlanBlockLayers.
void ReportLayerState(const std::string& sceneId, const std::string& label, std::string state);

int AddStageBlocks(rendering::RenderEngine& engine, const std::string& sceneId,
                   const std::vector<ContentBlock>& blocks,
                   const rendering::Size& size, std::string_view tag,
                   std::string_view layer = "text",
                   const std::string& flowText = {},
                   float readingFontPx = 0.0f) {
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
    // The line height a block gets when its meta names none — or names 0, which
    // is what the Text panel writes for "the font's own spacing": a
    // PROPORTIONAL 1.0 of that font's natural line height, nothing added. THE
    // one value, mirrored by qml/settings/MetadataFields.qml's
    // naturalLineHeight (the panel, the canvas and the tiles all read it there);
    // the derivation below is where it becomes an additive lineSpacing.
    constexpr double kNaturalLineHeight = template_text_metrics::kNaturalLineHeight;
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
        // The block's rectangle on the frame — see BlockRect.
        const rendering::Rect rect = BlockRect(block, size);
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
        // A placed media FILE — the two kinds that paint a picture. "audio" is
        // deliberately not one of them: sound has nothing to draw, and a slide
        // whose item is a song draws the slide's other blocks over an empty box
        // exactly as the operator laid it out.
        const bool mediaKind = (block.kind == "image" || block.kind == "video");
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
            // "TEXT ON ONE LINE" (TextItemPanel's Text card): the canvas draws
            // this item with Text.NoWrap, so the output must too. NoWrap alone
            // is not enough — a literal newline in the content still breaks a
            // line under NoWrap — so the line breaks are folded to spaces HERE,
            // in the one string every later stage reads (layout, measure, draw,
            // and the same text the QML surfaces show folded).
            const bool oneLine = MetaBool(block.metaJson, "oneLine");
            std::string textBody = block.text;
            if (oneLine) {
                for (char& ch : textBody)
                    if (ch == '\n' || ch == '\r') ch = ' ';
            }
            const bool isFlowBlock =
                !flowText.empty() && !textBody.empty() && flowText.rfind(textBody, 0) == 0;
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
            auto styleComp = std::make_shared<rendering::TextStyleComponent>();
            // SCROLL'S OWN SIZE wins for the reading block (OutputStyleSpec::readingFontPx): the
            // app solved it once per style from the max-words budget, and every surface draws the
            // passage at it. Any other block keeps its declared size.
            styleComp->style.size = static_cast<float>(
                ((isFlowBlock && readingFontPx > 0.0f)
                     ? static_cast<double>(readingFontPx)
                     : MetaNumber(block.metaJson, "fontSize", kDefaultTextFontSize))
                * size.height / kBlockStageH);
            styleComp->style.color = StyleBuilder::ParseColor(
                MetaString(block.metaJson, "color").empty()
                    ? "#ffffff" : MetaString(block.metaJson, "color"));
            styleComp->style.align = ParseAlign(MetaString(block.metaJson, "align"));
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
            // fontId: the real system font TextEngine's Qt backend tries
            // first (RenderEngine::DrawTextObject / GlyphAtlas), degrading
            // to the builtin bitmap font only when it can't resolve — the
            // block's own fontFamily when the canvas set one, else the
            // show/template CONTENT default so plain/legacy blocks with no
            // family still get real typography instead of reading as "no
            // font chosen, so no real font" and falling all the way back.
            //
            // FONT-AGNOSTIC BY CONSTRUCTION: the block's family stays the
            // REQUEST here and every consumer resolves it through
            // FontManager::SubstituteFamily, so a design that names a font this
            // machine lacks draws with an installed face at the same size
            // instead of collapsing into the 5x7 bitmap (the giant pixel-block
            // verse reported from another install).
            //
            // The no-family default is a LITERAL name now ("Cambria"), not
            // the host's UI face (RenderEngine::...::DefaultFamily()) — user
            // call: "make the app[']s shows or default template['s font] be
            // Cambria, Segoe UI is for the app['s] UI". Content and chrome
            // are two different questions: Theme.fontFamily (every QML
            // button/label) still reads DefaultFamily() directly and is
            // untouched by this; this is ONLY the render path a slide's own
            // text takes, so the canvas, the preview and the real output
            // cannot disagree about which font a plain block draws with —
            // the exact mismatch FontManager/FontService exist to prevent
            // (see qml/services/FontService.h and its defaultContentFamily,
            // the QML-side twin of this literal).
            {
                const std::string family = MetaString(block.metaJson, "fontFamily");
                styleComp->style.fontId = family.empty() ? "Cambria" : family;
            }
            styleComp->style.italic = MetaBool(block.metaJson, "italic");
            styleComp->style.wrap = !oneLine;
            styleComp->style.wrapWidth = oneLine ? 0.0f : textBounds.width;
            // The typography knobs the canvas/DesignPreview honor and the
            // engine silently dropped — lineHeight (no added spacing: the
            // font's own line height, an empty line of black serif made a
            // verse box overflow at output scale) and letterSpacing.
            // letter-spacing is PER GAP (n-1 gaps for n glyphs), the same
            // convention TextLayout's width math uses.
            //
            // TemplateTextMetrics owns the line-height defaults and conversion
            // used by the QML preview, live fit solver, and this renderer.
            // Each renderer still supplies its own natural font measurement;
            // the shared helper applies the same multiplier and spacing rule.
            {
                const double lh = template_text_metrics::lineHeightMultiplier(
                    MetaNumber(block.metaJson, "lineHeight", kNaturalLineHeight));
                const float sizePx = styleComp->style.size;   // the output-scaled font size
                // THE FACE IT WILL ACTUALLY BE DRAWN WITH, not the raw name: a
                // design that names a font this machine does not have is painted
                // with the substitute (FontManager::SubstituteFamily), so the
                // spacing must come from THAT font — measuring the missing name
                // returns no metrics and falls back to the authored-size estimate.
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
            // Word spacing: the canvas's own value is in STAGE px (QML Text's
            // font.wordSpacing), so it scales to the output exactly as
            // letterSpacing and fontSize do.
            {
                const double ws = MetaNumber(block.metaJson, "wordSpacing", 0.0);
                styleComp->style.wordSpacing = static_cast<float>(ws * size.width / kBlockStageW);
            }
            // LINE SPACING — blank air BETWEEN line boxes, on top of the line
            // height above (FreeShow's own split: line-height sizes the line,
            // line spacing opens the gap). This is a vertical stage length, so
            // map it by the output height.
            {
                const double gap = MetaNumber(block.metaJson, "lineSpacing", 0.0);
                styleComp->style.lineGap = static_cast<float>(gap * size.height / kBlockStageH);
            }
            // LINE BACKGROUND — a band behind EACH line (TextStyle::lineBg),
            // not behind the block: the block's own fill is the box above.
            // An empty/unparseable value leaves alpha 0, i.e. no band at all.
            styleComp->style.lineBg =
                StyleBuilder::ParseColor(MetaString(block.metaJson, "lineBg"));
            // ...and how round those bands are (FreeShow's lineRadius). Stage px,
            // scaled like every other length here, so the canvas radius and the
            // output radius are the same number at two sizes.
            {
                const double lr = MetaNumber(block.metaJson, "lineRadius", 0.0);
                styleComp->style.lineRadius =
                    static_cast<float>(std::max(0.0, lr) * size.width / kBlockStageW);
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
            // line" (the old renderer used different font advances than the
            // QML renderer, so a size that fit 754×428 left the 1920×1080
            // re-layout wrapping more lines and pushing the tail off the
            // frame). Shared Qt metrics make the solve consistent.
            // ALWAYS solving (with the set size as the ceiling) makes
            // the engine agree with both QML surfaces by construction.
            // A scrolling reading block normally keeps its declared size: fitting
            // each page independently would make the type jump as the passage
            // advances. An explicit growToFit is different — it is the design's
            // instruction to fill this box, and the QML preview applies that fit
            // even when the block is the reading column. Honor it here too.
            const std::string autoSize = MetaString(block.metaJson, "autoSize");
            const bool growToFit = autoSize == "grow" || autoSize == "growToFit";
            const float minPx =
                static_cast<float>(3.0 * size.height / kBlockStageH);   // the canvas solver's own 3px floor, output-scaled
            // (A solved scroll size means NO auto-fit at all, grow-to-fit included: Scroll takes
            // the budget's size as given, as the monitor surfaces do.)
            if (isFlowBlock && (!growToFit || readingFontPx > 0.0f)) {
                // Left exactly as the per-block default set it above
                // (MetaNumber(..., "fontSize", ...) * size.height / kBlockStageH):
                // the template's own declared size, not a page-specific bisection.
            } else if (growToFit) {
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
        } else if (mediaKind) {
            // A MEDIA ITEM THE OPERATOR PLACED (the Edit canvas's Media chip ->
            // Picture / Video): the FILE's own picture, in the item's box. Until
            // this branch existed the block fell through the kind switch and
            // every real output — NDI, HDMI, recordings — showed an empty hole
            // where the canvas showed the picture; only the in-app previews
            // drew anything, and they drew a stand-in card.
            //
            // A picture paints its decoded pixels; a video paints its poster
            // frame (see LoadVideoPosterCached — this engine has no file-video
            // transport, the UI's decoder owns that and hands over the
            // compositor's frame). An item with no file paints nothing: a
            // guessed box would be a lie about content that is not there.
            const std::string src = MediaSrcOf(block);
            rendering::RgbaImage picture;
            if (!src.empty())
                picture = (block.kind == "video") ? LoadVideoPosterCached(src)
                                                  : LoadImageCached(src);
            if (picture.width > 0 && picture.height > 0) {
                // THE ITEM'S OWN MEDIA VALUES (the Edit screen's MEDIA tab):
                // crop, flips and the filter set are applied to the DECODED
                // PIXELS here, and `fit` decides the box they end up in — so the
                // picture the outputs draw is the one the canvas drew, without a
                // backend having to learn a new transform.
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
                    // Blur fill (FreeShow's fourth fit): the file fills the box
                    // softened and a sharp copy sits on top of it. The backdrop is
                    // its own object, painted first on the same layer.
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
                // THE MEDIA TAB'S BLEND MODE, on the OBJECT: the rasterizer
                // composites this picture against the frame already under it,
                // so Screen/Multiply/Overlay actually change the broadcast
                // picture instead of being a stored-but-ignored value. The
                // blur-fill backdrop above stays plain alpha — it is the box's
                // own backdrop, not the item's picture.
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
                      const rendering::Rect& bounds, uint64_t sourceSeq = 0,
                      rendering::BlendMode blend = rendering::BlendMode::Alpha) {
    auto existing = engine.GetObject(sceneId, objId);
    if (existing.ok()) {
        if (auto* v = dynamic_cast<rendering::VideoObject*>(existing.value())) {
            v->SetFrame(img);
            v->SetBounds(bounds);
            v->SetSourceSeq(sourceSeq);
            // A BUS WINDOW'S BLEND MODE (the MEDIA tab's row): set on every
            // upsert, so picking Screen/Multiply on a window changes the frame it
            // composites into instead of being a stored value nothing reads.
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

// ---- DRAW A LIVE LAYER ONLY WHEN ITS PICTURE MOVED -------------------------
//
// The compositor layers are refreshed on EVERY build, cache hits included (a
// cached scene still has to show the CURRENT frame), and one tick builds a
// scene per output — the program pass plus a gated pass per output. Each
// refresh copies the source frame twice (compositor/PAL -> RgbaImage -> the
// object) and re-uploads its texture, whether or not anything arrived since:
// at 1080p that is ~24 MB of memcpy per layer per output per frame for a
// picture that is pixel-identical to the one already on the object.
//
// Every source has a CHEAP counter (the PAL tap's PreviewFrameSeq, the
// compositor's per-frame seq), so "is this the frame I already drew?" is an
// integer compare. Under load — when a capture device runs SLOWER than the
// composition, which is the reported "HDMI video lags when the machine is
// stressed" — it answers "yes" for most builds and those copies disappear
// entirely. The counter alone is not enough to skip on: a scene rebuilt from
// scratch, or one whose object was refused, has no picture to keep, so the
// object must exist and must carry pixels.
// MEASUREMENT HOOK, not a setting: with BPS_LAYER_REFRESH_ALWAYS set, every
// layer is re-copied and re-uploaded on every build exactly as it was before
// this guard existed. tools/selftest/run_videoperf_test.sh uses it to measure
// the same machine, the same feed and the same window WITH and WITHOUT the
// skip, which is the only honest way to report what the skip is worth.
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
// THE CANVAS IS NOT THE OUTPUT: a design block of kind "bus" draws the bus's
// live feed inside its rectangle on the Edit canvas, and until now every real
// output drew NOTHING there — the block fell through AddStageBlocks' kind
// switch (text / box / shape, everything else ignored), so NDI and HDMI showed
// an empty hole where the operator's canvas showed video. A bus block is a
// WINDOW, not a picture of one: its own geometry belongs to the design, so the
// plane is painted at the block's rect on the compositor's VIDEO layer ("media"
// — under every text/shape block, which is what a caption over a bus feed is)
// instead of full-frame like the taken input and media-on-air layers.
//
// Geometry is decided ONCE per build (RecordBusPlanes) because a scene's block
// layout is part of what the cached scene means; the PIXELS are refreshed on
// every build, cache hit included, exactly like AddCompositorLayers — a scene
// that is already built must still show the bus's current frame, not the one it
// had when it was built.
struct BusPlane {
    int busId = 0;
    std::string objId;          // stable per block, so the frame upserts in place
    rendering::Rect rect;
    // THE WINDOW'S OWN MEDIA VALUES (the Edit screen's MEDIA tab, which a bus
    // item reuses): Fit / Crop / Flips / Filters, read off the block's meta at
    // RECORD time — geometry is decided once per build, and these ride the same
    // decision because they decide the picture's box too. The pixel work is
    // applied to each NEW frame in RefreshBusPlanes (identity values cost
    // nothing), so the window on air is the picture the operator edited.
    MediaItemValues media;
};
std::mutex g_busPlaneMutex;
std::map<std::string, std::vector<BusPlane>> g_busPlanes;

// The bus a block carries: its meta `busId` is the bus's STABLE number (see
// BusSourceModal / BusMonitor — never a roster row, which moves). 0 = not a bus
// block, or one whose meta lost its id (nothing to show, and nothing to guess).
int BusIdOf(const ContentBlock& block) {
    if (block.kind != "bus")
        return 0;
    return static_cast<int>(MetaNumber(block.metaJson, "busId", 0.0));
}

// Records where each bus block of THIS scene sits. Called from the one place
// that knows a scene's block layout (the build path of BuildSlideScene); a bus
// removed from a design replaces the list, so its plane stops being refreshed.
void RecordBusPlanes(const std::string& sceneId, const std::vector<ContentBlock>& blocks,
                     const rendering::Size& size, const OutputStyleSpec* gate = nullptr) {
    std::vector<BusPlane> planes;
    for (size_t i = 0; i < blocks.size(); ++i) {
        const ContentBlock& block = blocks[i];
        const int busId = BusIdOf(block);
        if (busId <= 0)
            continue;
        // WIRED OR NOTHING (OutputStyleSpec::gateVideoBuses): a bus window is a
        // live feed on this design, and an output with no wire to that bus has no
        // business painting it. Skipping here — rather than blanking a plane —
        // means an unwired output's pass records no bus plane at all, so its frame
        // is the design MINUS the window, not the design with a black hole in it.
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

// THE WINDOWS OF A TEMPLATE BUS (see BuildTemplateBusScene). A template design's
// `bus` blocks, in document order, each recorded with the plane it paints:
//
//   - the block's OWN assignment (meta busId) when it names one — a window's
//     bus is configured on the window, independently of every other window, so
//     two windows on one template can show two different buses;
//   - otherwise the template bus's own routed video source for that window's
//     POSITION (CompositorState::TemplateWindowPlaneKey) — the "route cameras in
//     and they fill the windows" fallback.
//
// Nothing is stored on the bus: the windows ARE the template, so an edited
// template (a window moved, added or re-assigned) reaches the bus's picture
// without recreating the bus — the caller bakes the blocks fresh on every push.
void RecordTemplateWindows(const std::string& sceneId, const std::vector<ContentBlock>& blocks,
                           const rendering::Size& size, int busNumber)
{
    std::vector<BusPlane> planes;
    // `route` counts OPEN windows only — it indexes the bus's routed video
    // sources, which LiveOutputService publishes as TemplateWindowPlaneKey(bus, i)
    // for route i. Counting every bus block here instead put the first route in
    // a window a named block occupied: on a template of [named window, open
    // window] route 0 landed on a key nothing reads and route 1 painted the only
    // open window, so routing one camera showed the next one's feed (or nothing).
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

// Repaints every recorded bus plane with the bus's CURRENT frame — on every
// build, cache hits included (see BusPlane). A bus whose feed stopped gets an
// empty frame, which UpsertVideoLayer already turns into "invisible" rather
// than a frozen last picture.
// What a window shows while its bus has no picture: a flat plate the size of the
// window, so an unfed window reads as a window and not as a hole in the design.
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

// Where a window's picture sits once its FIT is honoured — the same four choices
// a placed picture has (the MEDIA tab's Fit row, which a bus item reuses). The
// engine cannot clip, so cover crops the SOURCE to the window's own aspect; blur
// keeps the sharp feed contain-fitted, because a per-frame 24px blur over a live
// feed is the faint difference the canvas already draws in its preview only.
rendering::Rect BusFitRect(const rendering::RgbaImage& img, const rendering::Rect& box,
                           const std::string& fit) {
    if (fit == "fill")
        return box;
    return ContainRectIn(img.width, img.height, box);
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
        // THE CHEAP QUESTION (see LayerFrameCurrent): the compositor's push
        // counter for this bus. A plane whose object already shows this frame
        // is left completely alone — no 8 MB copy, no re-upload — which is
        // most builds whenever the bus's source runs slower than the
        // composition (a throttled capture device: the reported lag).
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
            // THE WINDOW'S MEDIA VALUES, on the pixels the output draws — the
            // same work a placed picture gets (ApplyMediaPixelWork), so a flip, a
            // crop or a filter set on the window is the same picture in preview
            // and on air. Identity values skip the pass entirely: a live feed runs
            // this on every new frame, and nothing should pay for "unchanged".
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
    // THE ENGINE HALF OF THE BUS-WINDOW DIAGNOSTIC. "the canvas has the bus but
    // the output is empty" is answered here: a plane count of 0 means the scene
    // never recorded a bus block, a count with no frames means the feed never
    // reached CompositorState (its tap, not the renderer, is quiet), and
    // frames-with-plane means the fault is downstream (texture/draw order).
    ReportLayerState(sceneId, "bus video",
                     std::format("{} of {} bus plane(s) have a frame", withFrame, planes.size()));
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
    // TRUE while a thread is building this scene's overlays OUTSIDE the lock
    // (see AddCompositorLayers). The signature stays empty until that build
    // lands, so a second caller during the same window returns instead of
    // stacking a duplicate set of overlay objects onto the same scene.
    bool building = false;
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
//
// THE ONE EXCEPTION IS A CLEAR CARRIER (`Slide::clearStage`, the Blackout
// control): nothing being composited is the POINT of it, so its callers skip
// this pass entirely (see ComposeSlide's clear branch). Leaving the layers in
// would make "nothing at all on screen" still show a taken camera in the middle
// of a service — and skipping it leaves the state itself untouched, so the
// blackout can be lifted with the feed and overlays exactly as they were.
void AddCompositorLayers(rendering::RenderEngine& engine, const std::string& sceneId,
                         const rendering::Size& size, bool showOverlay = true) {
    // Taken camera/screen input: pulled straight from the PAL's own
    // thread-safe tap (this runs on SceneBuilder's own worker thread) —
    // CompositorState only mirrors WHICH device, not its pixels, so a live
    // feed never goes stale waiting on a push from the UI thread.
    const auto taken = presentation::CompositorState::Instance().GetTakenInput();
    rendering::RgbaImage takenImg;
    // THE CHEAP QUESTION FIRST (see LayerFrameCurrent): the tap's own frame
    // counter, so a build that finds no new frame skips the pull, both copies
    // and the texture upload. A build that finds a NEW one — or no counter at
    // all — does the whole refresh exactly as before.
    uint64_t takenSeq = 0;
    bool takenRefresh = true;
    if (!taken.deviceId.empty() && bps::platform::PlatformAccessor::Installed()) {
        takenSeq = bps::platform::PlatformAccessor::Get().Video().PreviewFrameSeq(taken.deviceId);
        takenRefresh = !LayerFrameCurrent(engine, sceneId, "takeninput", takenSeq);
    }
    if (takenRefresh) {
        if (!taken.deviceId.empty() && bps::platform::PlatformAccessor::Installed()) {
            const auto frame = bps::platform::PlatformAccessor::Get().Video()
                                   .PreviewFramePixels(taken.deviceId);
            takenImg = ToRgbaImage(frame.width, frame.height, frame.rgba);
        }
        UpsertVideoLayer(engine, sceneId, "takeninput", takenImg,
                         rendering::Rect(0, 0, size.width, size.height), takenSeq);
    }

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
    if (takenRefresh) {
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
    // here). CONTAIN, not cover: a taken file is the operator's picture, so its
    // own aspect is kept (letterboxed on the stage's background) exactly as the
    // QML tile draws it with PreserveAspectFit. Cover-fit used to crop a
    // portrait photo down to the middle of a landscape stage — the report "the
    // image is portrait and you made it landscape".
    // MEDIA ON AIR — the same cheap question the taken input asks above (see
    // LayerFrameCurrent): a 30 fps file is pushed half as often as a 60 Hz
    // composition builds, so half of those builds were re-copying a picture
    // that had not changed. The FIT travels with the frame and a fit change
    // bumps the compositor's counter (CompositorState::SetMediaFit), so the
    // layout can never be skipped while the pixels it belongs to stayed put.
    const auto& compositorState = presentation::CompositorState::Instance();
    const uint64_t mediaSeq = compositorState.GetMediaSeq();
    const bool mediaRefresh = !LayerFrameCurrent(engine, sceneId, "mediaonair", mediaSeq);
    const auto media = mediaRefresh ? compositorState.GetMediaFrame()
                                    : presentation::CompositorState::MediaFrame{};
    const rendering::RgbaImage mediaImg = mediaRefresh
        ? ToRgbaImage(media.width, media.height, media.rgba)
        : rendering::RgbaImage{};
    // ONE PICTURE, ONE SHAPE: the FILE's own fit (a per-media choice is the
    // operator saying "this picture, like this", and it travels with the frame
    // — CompositorState::MediaFrame::fit), else CONTAIN. Nothing about the
    // OUTPUT decides a take's shape any more.
    //
    // This is where two outputs came to show one take as two pictures: the
    // per-output "Fill" setting (Settings · Outputs) was the fallback, so a
    // screen left on "cover" cropped the photo while its neighbour letterboxed
    // it — one monitor wall, two looks, for one file. The setting also could not
    // be corrected on the ACTIVE output at all (PresentationEngine
    // ::SetActiveOutputStyle compared every spec field EXCEPT mediaFit, so a
    // Fill change on it was read as a no-op push and never rebuilt a frame —
    // "why can't I change the fits?" for the Main Output while the others
    // obeyed). A take's shape belongs to the PICTURE; the roster's old per-output
    // value is still loaded and saved, and no longer composes anything.
    const std::string_view effectiveFit = media.fit.empty() ? std::string_view("contain")
                                                            : std::string_view(media.fit);
    rendering::Rect mediaRect(0, 0, size.width, size.height);
    if (!mediaImg.empty()) {
        if (effectiveFit == "cover") {
            mediaRect = CoverRect(mediaImg.width, mediaImg.height, size);
        } else if (effectiveFit == "fill") {
            mediaRect = rendering::Rect(0, 0, size.width, size.height);
        } else {
            mediaRect = ContainRect(mediaImg.width, mediaImg.height, size);
        }
    }
    // Letterboxed: the margins stay EMPTY (the frame's own transparency / the
    // style colour underneath), and the picture is only the picture. An earlier
    // cut drew a blown-up copy of the same frame under it to "fill" the bars,
    // which reads as two rectangles ("the app is now rendering two rect full and
    // 16:1 — if the image is 16:1 I expect the transparent bg around it, that is
    // EMPTY SPACE"). Empty space is empty space; no extra layer, and one less
    // full-stage blit per frame.
    if (mediaRefresh)
        UpsertVideoLayer(engine, sceneId, "mediaonair", mediaImg, mediaRect, mediaSeq);

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
    if (mediaRefresh) {
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

    // THE CACHE LOCK GUARDS THE BOOKKEEPING, NEVER THE BUILD. It used to be
    // held across AddStageBlocks, which reaches RenderEngine::Fonts() (Qt font
    // metrics) and the scene graph — so one slow overlay build parked every
    // other scene's overlay pass (the exact lock pile-up the lock-scope guard
    // exists to stop). The atomicity that mattered is kept: ids are claimed
    // under the lock (the previous set is taken out, `building` set, signature
    // cleared), the engine work runs unlocked, and the new ids land under the
    // lock again. A second caller for the SAME scene during the build sees
    // `building` (or the empty signature) and returns — the next frame finds
    // the finished build — instead of interleaving AddObject calls, which is
    // what left duplicate overlay objects on an output.
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
            // A text block whose own style carries a box fill is TWO objects
            // (AddStageBlocks adds `<id>box` under the text) — registering only
            // the text would leave every previous overlay's box in the scene
            // when the overlay set changes (AddObject has no upsert; the diff
            // loop above is the ONLY thing that removes them). A missing id is
            // a harmless no-op for the removals.
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
// ---------------------------------------------------------------------------
// THE STYLE'S OWN CHAR -> LINE TABLE for a passage (see ApplyReadingFlow).
//
// The offset the surfaces share is a Qt/QML character of the passage (UTF-16
// code units), not a UTF-8 byte; which LINE that character sits on depends on
// the style's type size and wrap, and this output is the only place that answer
// exists for THIS style. Built once per scene by mapping each folded engine
// line back to the passage — the per-frame read is then a binary search
// (TextObject::FlowLineForChar), because a scroll in flight maps the position
// sixty times a second.
//
// A line the wrapper TRUNCATED or rewrote (no exact slice at or after the
// cursor) keeps the previous line's start: the table stays monotonic, and the
// mapping degrades to "the line before it", which is the ordering the old
// line-number path assumed anyway.
//
// A free function rather than a lambda in the two callers that need it: a scene
// BUILD attaches the flow (ApplyReadingFlow) and so does a WINDOW that moved
// under an already-built scene (RefreshReadingFlow), and the table has to be the
// same table in both or the position means two different things.
static std::vector<int> FlowCharStartsFor(const std::string& flowText,
                                          const rendering::TextStyle& style,
                                          const rendering::FontManager& fonts)
{
    return rendering::TextLayout::FlowCharacterStarts(flowText, style, fonts);
}

// THE READING FLOW (Alt+Down / Alt+Up — see Slide::flowText): hand the passage
// to the ONE text object that draws this slide's reading text, so the outputs
// scroll it under the block's own box instead of the step replacing the whole
// picture with a re-fitted window of it.
//
// WHICH OBJECT. The reading text is the LONGEST text in the composition — the
// same rule the QML stepper (LiveOutputService::contentTextBlock) and the
// monitor tile (DesignPreview::readingBlock) use to choose the block they move.
// It has to be found by a rule rather than an id, because there are three
// carriers for one idea: a show's own slide keeps its words in one of its own
// blocks (id <blk3>…), a Table/Bible slide receives them through the style's
// template slot (<tplf2>…), and the legacy preset draws them as "body".
//
// THE GUARD IS A PREFIX TEST: the flow must BEGIN with the page the object is
// showing. If it does not — a style that rewrote the words, a flow left over
// from another pick — the object is left alone and the outputs draw the page
// exactly as before, which is the honest answer: a scroll with the wrong words
// in it would be worse than no scroll at all.
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
    // WHICH BOX GOT PICKED, every time this runs — the engine's own twin of
    // DesignPreview's readingBlockIndex (see that property's own note: a
    // fill-built page can carry the SAME paragraph in two blocks, a content
    // slot and a band, and "longest text wins" can pick a DIFFERENT one of
    // them page to page if their lengths aren't always in the same order —
    // which reads as the box/size changing, because it would genuinely be a
    // different box. QML's own pick is logged separately (ReadFitNote); the
    // two should always name the same box for the same page, and this is
    // the only way to see them agree or disagree without guessing.
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
    if (slide.flowText.rfind(reading->Text(), 0) != 0) {
        // ONE LINE PER SCENE per state change — the live loop rebuilds on every
        // fingerprint move, so a repeating diagnostic would flood the log the
        // operator is asked to read (the blank-slide report above says the same).
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
    // HOW MANY LINES ONE PAGE TAKES is MEASURED, here, with the style the object
    // actually draws with — not counted by the front end. The block's own
    // vertical alignment keeps using the page's extent (the box was fitted to a
    // page, not to the passage), so this number has to be the engine's own
    // reading of that page or a centred block would drift off its design.
    const auto* styleComp = reading->FindComponent<rendering::TextStyleComponent>();
    const rendering::TextStyle style = styleComp ? styleComp->style : rendering::TextStyle{};
    const auto pageLayout = rendering::TextLayout::Measure(reading->Text(), style, engine.Fonts());
    const int pageLines = static_cast<int>(pageLayout.lines.size());
    // The PAGE stays the layout's reference (the fit solver already sized it), and
    // the passage becomes what is drawn: one layout, one size, one offset into it
    // (RenderOptions::contentScrollLines, resting at Slide::flowScroll).
    // ...AND THIS STYLE'S OWN LINE TABLE, so the position can be a CHARACTER
    // OFFSET rather than a line number (see FlowCharStartsFor).
    reading->SetFlowCharTable(FlowCharStartsFor(slide.flowText, style, engine.Fonts()));
    reading->SetReadingFlow(pageLines, slide.flowChar < 0.0f ? 0.0f : slide.flowChar);
    reading->SetText(slide.flowText);
    // ONE LINE PER SCENE (the live loop rebuilds on every fingerprint move): the
    // numbers a "the step does nothing / jumps a page" report turns on — how many
    // lines the page takes, and how much passage it was given to scroll into.
    {
        static std::mutex flowLogMutex;
        static std::set<std::string> flowLogged;
        std::lock_guard<std::mutex> lock(flowLogMutex);
        if (flowLogged.insert(sceneId).second)
            Logger::Instance().Debug(std::format(
                "reading flow attached: page {} line(s), passage {} char(s), resting at char {:.2f}",
                pageLines, slide.flowText.size(), slide.flowChar), "SceneBuilder");
    }
}

// RE-ATTACH A MOVED READING WINDOW (see SceneBuilder.hpp). The scene is the one
// that is already on screen: its reading block keeps its box, its type size and
// its page, and only the passage beneath the page and the position in it change
// — which is exactly what a page commit inside one passage does.
bool SceneBuilder::RefreshReadingFlow(rendering::RenderEngine& engine,
                                      const std::string& sceneId, const Slide& slide) {
    // THE OBJECT THAT ALREADY DRAWS THE FLOW — its HasReadingFlow() is the mark
    // ApplyReadingFlow leaves, so this is the one case where the longest-text
    // rule above must NOT be re-run: the reading object's text is now the whole
    // passage, and it would win against itself only by accident.
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
        // A previous mode change may have detached the flow from this existing
        // scene. Re-attach it to the original page object instead of leaving NDI
        // static while the QML preview has resumed Scroll mode.
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
    const auto* styleComp = reading->FindComponent<rendering::TextStyleComponent>();
    const rendering::TextStyle style = styleComp ? styleComp->style : rendering::TextStyle{};
    reading->SetFlowCharTable(FlowCharStartsFor(slide.flowText, style, engine.Fonts()));
    // THE PAGE'S LINE COUNT IS KEPT, not re-measured: it is the measure of the
    // page the box was fitted to (see ApplyReadingFlow), and the page is the same
    // one — the window only moved which words follow it.
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

    // ---- A CLEAR PAINTS NOTHING AT ALL -------------------------------------
    // Clear all (and the per-layer slide clear) sends a CLEAR carrier: no
    // content AND no style. The obvious shape — an empty slide, composed the
    // same way every other slide is — is what made a clear look like it did not
    // clear: the style paints onto an empty slide exactly as it does onto a
    // full one (that is the whole point of the blank GO-LIVE carrier), so the
    // screens went from the slide to the style's own background IMAGE and the
    // report was "clear all doesn't clear: the bg brings back the background
    // image and clears the slide".
    //
    // So the clear is its own composition, decided HERE rather than by every
    // output's style: nothing of the style's (colour, image), nothing of the
    // slide's, and an explicitly empty frame — `explicitTransparent` marks it
    // as a CHOSEN empty frame, which is what the monitor tile needs to show its
    // transparency convention instead of painting the style's colour as the
    // pane's base.
    //
    // GO LIVE with nothing staged does NOT come through here: that carrier is
    // deliberately not a clear, and keeps the output's own look (see
    // LiveOutputService::goLiveBlank).
    //
    // "NOTHING" ALSO INCLUDES THE COMPOSITOR'S OWN LAYERS: a taken camera or
    // screen, a media file on air and the overlay stack are composited INTO the
    // scene by AddCompositorLayers, and that pass used to run for this carrier
    // like any other slide — so the Blackout control's "nothing at all on
    // screen" still showed the camera in the middle of a service. The callers
    // skip that pass for a Cleared composition (see BuildSlideScene /
    // BuildGatedSlideScene); the layers themselves are untouched, so ending the
    // blackout brings the feed and the overlays back exactly as they were.
    if (slide.clearStage) {
        comp.source = SlideComposition::Source::Cleared;
        comp.paintBackground = false;
        comp.explicitTransparent = true;
        comp.styleImageSuppressed = true;
        return comp;
    }

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

    // ---- IS THIS FRAME TRANSPARENT WHERE NOTHING PAINTS? --------------------
    // The style/slide half of the ONE answer (see frameTransparent): a frame that
    // CHOSE transparency, or one that names no background at all — a keyable
    // frame: what is keyed, recorded or composited behind it shows through. Both
    // are transparency; only this composition can tell them from a frame that is
    // merely still empty, which is why the answer is composed here and read by
    // the surfaces instead of re-derived by each of them.
    comp.frameTransparent = comp.explicitTransparent || !comp.paintBackground;

    // ---- A TAKE CLEARS THE STYLE'S BACKGROUND -------------------------------
    // "Anything that wants to show removes the style's bg." The rule already
    // existed for a taken INPUT (the tile's `videoUnderneath`) and for text
    // (clearBackgroundOnText); a file on air is the same situation and was
    // missing, so a letterboxed photo sat inside the style's colour/image.
    //
    // Safe against the cache because the media bit is part of the SCENE ID (see
    // StyledSceneIdFor's "@m:on" token): a build with a take is a DIFFERENT
    // scene from one without, so clearing the take re-keys back to the scene
    // that still has its backdrop — the composition and the cache can never
    // disagree about it.
    if (presentation::CompositorState::Instance().HasMediaFrame()) {
        comp.paintBackground = false;
        comp.background.clear();
        comp.explicitTransparent = true;
        comp.styleImageSuppressed = true;
        comp.clearedByTake = true;
        // THE PICTURE'S OWN ALPHA, not the take's existence. A video, an opaque
        // photo and an audio file are OPAQUE frames: the margins a contain-fit
        // leaves around them are the output's empty pixels, not the picture's
        // transparency, and reading them as transparency is what painted
        // checkered pillarbox bars on the monitor wall that no output ever had.
        // A genuinely transparent still (a keyed PNG, an alpha WebP) keeps it.
        comp.frameTransparent = presentation::CompositorState::Instance().MediaFrameIsTransparent();
    } else if (!presentation::CompositorState::Instance().GetTakenInput().deviceId.empty()) {
        // A TAKEN CAMERA/SCREEN FEED IS AN OPAQUE PICTURE: it replaces the frame
        // the style/slide above described, exactly as a media file does (the two
        // are mutually exclusive — see LiveOutputService's take).
        comp.frameTransparent = false;
    }

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

    // ---- A SHOW'S SLIDE IS ITS OWN DESIGN ---------------------------------
    // The SCRIPTURE and TABLE tabs build their slides FROM a design: the blocks
    // a pick carries are that design's field slots, holding content for an
    // output style to lay out its own way — which is why the branches below
    // hand such a slide to the style's template first.
    //
    // A SHOW is the other way round. Its slide's blocks are the design itself:
    // every item was placed on the Edit canvas by the operator (a title item, a
    // text band, a shape), and nothing about them is a slot for a style to
    // re-fill. An output style's WHOLE-STYLE template is not keyed to a
    // content family at all (style.contentType empty), so it claimed these
    // slides too and replaced the authored layout with its own: the outputs
    // (NDI, HDMI, the distributed frames) drew the style's bound body — the
    // hymn lines, no title, no authored band — while the monitor tile and the
    // Edit canvas, which draw the slide's own blocks, showed the whole design.
    // That is the report "the preview shows all the content but the NDI/HDMI
    // output does not" for shows only; scripture and The Table were never
    // affected because their style templates are the fix for THOSE families.
    //
    // So an untagged slide (the shows family — see SlideRules::FamilyTemplateSlot)
    // that carries its own blocks keeps them under the style's WHOLE-STYLE
    // template (templateKey/templateBlocks), which names no family at all.
    // That is the template that claimed these slides and drew "the style's
    // bound body — the hymn lines, no title, no authored band" while the tile
    // and the canvas showed the whole design. The style still supplies the
    // BACKGROUND (colour and image, decided above, so a branded backdrop still
    // lands). The style's gates are untouched (StyleAdmits, above): a style
    // that switches the shows family OFF still composes background-only for
    // these slides.
    //
    // A show's explicit Preview Template override is also a choice: its resolved
    // blocks carry `templateOverride`, so they remain the slide layout even when
    // Styles has its own Shows template selected.
    //
    // THE OTHER EXCEPTION IS A CHOICE, NOT AN INFERENCE: a template picked in the
    // style's OWN Shows slot (Settings · Styles → "Template for Shows" →
    // Change, i.e. familyTemplateKeys/familyTemplateBlocks[shows]) is the
    // operator saying "lay my shows out with THIS design" — the picker's own
    // contract ("the engine renders that design as the style's layout"). That
    // explicit pick wins over the authored layout, exactly like a
    // scripture/table pick does for its family, and like those it can still
    // hand the slide back when the design paints nothing (see
    // fallBackToSlideBlocks). The report "I chose Lower Third for style 2 and
    // it does the opposite" is exactly this distinction: the general template
    // is not a layout request for shows, the Shows slot is.
    const bool slideIsShowsContent =
        comp.contentType.empty() || comp.contentType == "shows";
    // THE PICK IS THE KEY, not the blocks: a family slot can carry BAKED blocks
    // it never selected — the front-end's shows fallback fills the shows slot
    // with the Scripture design whenever the style names no shows template
    // (StyleSpecBuilder::bakeFamilyTemplates, its "Bible fallback for show
    // content"), and the whole-style inherit pass fills the other families —
    // so "has blocks" would be true for every style and would hand every
    // authored show to a design its operator never chose. Only a key names a
    // choice; the blocks are then its baked form.
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

    // A LAYOUT THAT WOULD PAINT NOTHING HANDS THE COMPOSITION BACK TO THE SLIDE.
    //
    // The branches below pick a layout and give it the slide; each of them can
    // come out BLANK for content-family slides, because those carry their text
    // in their own blocks while a template/preset renders its own slots (or the
    // slide's title/body, which such a slide has none of). A blank composition
    // is a blank output — the tiles draw the slide's blocks themselves and look
    // right while the NDI feed and the HDMI window show black, and under a
    // transparent style the frame is fully see-through, which is the report "it
    // is sending frames but nothing arrives, not even the background image".
    // FreeShow's precedence (output style > slide) is kept exactly where the
    // layout has something to draw; where it does not, the slide's own blocks
    // are the only content there is.
    const auto fallBackToSlideBlocks = [&comp, &slide, &style](bool layoutPaints,
                                                              std::string_view what) {
        if (layoutPaints)
            return;
        if (slide.blocks.empty()) {
            // Nothing of the slide's own to hand the composition to either:
            // the layout and the slide are BOTH empty, which is a different
            // bug — the content never reached the engine. Named out loud
            // because the app's own panes draw their picture from the model
            // and look populated while every output stays blank.
            //
            // Only when the frame ends up with NOTHING in it at all: a blank
            // GO LIVE carrier reaches this same branch on purpose, and it is
            // not a fault — the style's image (or the style colour) still
            // paints there, which is exactly the difference this asks about.
            const bool frameIsEmpty = !comp.paintBackground
                && (style.backgroundImage.empty() || comp.styleImageSuppressed);
            // A style with no name is the engine's own style-less preview build
            // (no output wears it), so a blank frame there is not a fault.
            if (!frameIsEmpty || style.name.empty())
                return;
            {
                std::lock_guard<std::mutex> lock(g_blankSlideMutex);
                if (!g_blankSlideLogged.insert(slide.id).second)
                    return;   // already said so for this slide
            }
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
            // A Shows template is a layout for the show's authored items.
            // Match text items into its text boxes (id first, then document
            // order) so a template's sample/title binding cannot replace the
            // actual slide text with labels such as "Verse 2". This applies
            // equally to a style's general Shows template and its category
            // override: the style selects the layout, the show supplies text.
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
    // SCROLL'S TYPE SIZE (OutputStyleSpec::readingFontPx) changes the picture, so it keys the scene:
    // a re-solved size must rebuild, not be served the scene built at the old one. Absent at 0, so
    // every id without a scroll size is byte-identical to before.
    if (style.readingFontPx > 0.0f)
        id += std::format("@rf:{:.2f}", static_cast<double>(style.readingFontPx));
    // A TAKE IS PART OF THE COMPOSITION, so it is part of the KEY: with a file on
    // air the style's colour and background image step aside (ComposeSlide), and
    // a cache hit only refreshes the compositor's dynamic layers — so without
    // this token a scene built before the take would keep its backdrop under the
    // file, and clearing the file would leave a scene built without one.
    //
    // HasMediaFrame(), not GetMediaFrame(): the question is "is one up", and the
    // latter COPIES the decoded frame (megabytes, at the preview's frame rate).
    if (presentation::CompositorState::Instance().HasMediaFrame())
        id += "@m:on";
    // WHICH BUSES MAY PAINT (OutputStyleSpec::gateVideoBuses). A gated build is a
    // DIFFERENT PICTURE from an ungated one even under the same style, so it has to
    // be a different scene: without this token the program's own cached scene —
    // bus windows and all — would be served to an unwired output as a cache hit,
    // which is exactly the bug this key exists to prevent.
    //
    // Sorted and de-duplicated: the order wires happen to arrive in is not part of
    // what the picture means, so re-wiring the same set must not rebuild.
    if (style.gateVideoBuses) {
        std::vector<int> buses = style.allowedVideoBuses;
        std::sort(buses.begin(), buses.end());
        buses.erase(std::unique(buses.begin(), buses.end()), buses.end());
        std::string token = "@gb:";
        for (size_t i = 0; i < buses.size(); ++i) {
            if (i > 0)
                token += ",";
            token += std::to_string(buses[i]);
        }
        if (buses.empty())
            token += "none";
        id += token;
    }
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
    if (!slide.clearStage)
        AddCompositorLayers(engine, sceneId, size, style.showOverlay);
    Logger::Instance().Debug(std::format("Gated scene built: {} (family refused by style '{}')",
                                        sceneId, style.name), "SceneBuilder");
    return sceneId;
}

// ---------------------------------------------------------------------------
// A WIRED VIDEO BUS AS AN OUTPUT'S OWN PICTURE (Settings · Routing)
// ---------------------------------------------------------------------------
// The wiring canvas has always been able to DRAW a bus into an output, and the
// engine has always STORED that (ProductionGraph::AssignOutputBuses validates
// it and the production document persists it) — but nothing composed it, so the
// wire was a note rather than a route: every screen, NDI sender and recording
// kept showing the program feed. This is the composer half.
//
// It reuses the ONE per-bus picture source the rest of the engine already
// draws from (CompositorState::GetBusFrame, keyed by the bus's stable number —
// the same frames a design's `bus` block paints and Media › Inputs' bus windows
// show), so an output wired to "Camera" shows exactly what the camera's window
// in the pane shows, and a Table bus shows the Table. Nothing here starts a
// device or a tap: a bus with no feed simply has no frame, which paints nothing.
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

    // The picture, on EVERY call — cache hit included. A scene that already
    // exists must still show the bus's CURRENT frame; the seq compare is what
    // keeps that honest without copying an unchanged 1080p frame per output per
    // tick (see LayerFrameCurrent).
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
    // The bus-feed half of the routing diagnostic: "the wire is drawn but the
    // screen is black" is answered here — a quiet bus (no seq) means the feed
    // never reached the compositor, a live one means the fault is downstream.
    ReportLayerState(sceneId, "bus feed",
                     std::format("bus {} {}", busId, seq == 0 ? "has no frame" : "live"));
    return sceneId;
}

// ---------------------------------------------------------------------------
// A TEMPLATE BUS AS AN OUTPUT'S OWN PICTURE
// ---------------------------------------------------------------------------
// A bus created from a template is a DESIGN with holes in it: its `bus` blocks
// are windows, and the bus's picture is that design with each window filled.
// This is the composer half — the routing canvas can already wire a template bus
// to an output, and this is what makes the wire a picture instead of a note.
//
// It is deliberately NOT BuildBusFeedScene (one full-frame layer): a template
// bus's whole point is its layout. Nor is it BuildSlideScene (which composes
// CONTENT — text bound to a slide, family gates, a style's template): a template
// bus carries no slide content, and its windows are its own.
//
// The windows are refreshed on EVERY call, cache hit included — the geometry is
// the scene's, the pixels are the feeds' (see BusPlane), so a window that stops
// producing frames goes blank rather than freezing on its last picture.
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
    // THE ID CARRIES THE LAYOUT, not just the bus and the size (the bus-feed
    // scene's rule, plus one more thing): the windows' GEOMETRY is part of what
    // the scene means, so a template edit that moves, adds, removes or re-assigns
    // a window has to re-key the scene — otherwise the engine serves the old
    // layout as a cache hit and the operator's edit reaches nothing, while only
    // the pixels would have been refreshed. Hashing the blocks (ids, kinds, rects,
    // assignment) keeps this the SAME scene while nothing about the layout moved,
    // which is the every-frame case.
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
        // Stage units, quantised to a thousandth — far finer than a pixel on any
        // output, and it keeps two rects that differ only in float noise from
        // re-keying the scene every frame.
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
        // The design's own content (text, bands, images) — the same block pass
        // every design renders through — and then the windows' geometry. A `bus`
        // block paints no text, so AddStageBlocks skips it and
        // RecordTemplateWindows owns it: one block, one owner.
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

    // Already built (cache hit) — the STRUCTURE (which text/shape objects
    // exist for this slide+style) is reused as-is, but the compositor
    // layers (a taken camera/screen input, a playing media file, active
    // overlays) are dynamic, continuously-updating state that lives OUTSIDE
    // this fingerprint — they must still refresh on every call, cache hit
    // or not, or a taken camera/media freezes on whatever frame existed the
    // moment this scene was first built (see AddCompositorLayers/
    // UpsertVideoLayer's own comments).
    if (engine.GetScene(sceneId).ok()) {
        // A CLEAR CARRIER OWNS NO COMPOSITOR LAYERS (see ComposeSlide): no
        // taken input, no media file, no overlay stack — the blackout is a
        // program black, not a slide whose background happens to be empty.
        if (!slide.clearStage) {
            AddCompositorLayers(engine, sceneId, size, style.showOverlay);
            RefreshBusPlanes(engine, sceneId);   // bus windows keep flowing on a cache hit (see BusPlane)
        }
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
    // path gets these layers regardless of which one executes — EXCEPT the
    // clear carrier, whose whole meaning is that nothing is composited (see
    // ComposeSlide): not the taken input, not the media file, not the overlays.
    if (comp.source != SlideComposition::Source::Cleared) {
        AddCompositorLayers(engine, sceneId, size, style.showOverlay);

        // Bus windows: a "bus" block is a live feed in the design's own
        // rectangle, so its plane is recorded from the composition that owns
        // this scene's layout (the style's template when one owns the
        // composition, the slide's own blocks otherwise — the SAME selection
        // slideToVariantMap hands the QML preview surfaces) and painted
        // straight away.
        RecordBusPlanes(sceneId,
                        comp.source == SlideComposition::Source::SlideBlocks ? slide.blocks
                                                                            : comp.blocks,
                        size, &style);
        RefreshBusPlanes(engine, sceneId);
    }

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
    // A LAMBDA, not a bare switch: the reading flow has to be attached AFTER
    // whichever layout branch ran (see ApplyReadingFlow) — and every branch used
    // to return straight out of this function with the scene finished.
    auto addContent = [&]() {
        switch (comp.source) {
        case SlideComposition::Source::FamilyTemplate: {
            const int n = AddStageBlocks(engine, sceneId, comp.blocks, size, "tplf", "text", slide.flowText,
                                         style.readingFontPx);
            Logger::Instance().Debug(std::format("Scene built: {} (style '{}', family {} template {} blocks)",
                                                sceneId, style.name, comp.familyIdx, n), "SceneBuilder");
            return;
        }
        case SlideComposition::Source::StyleTemplate: {
            const int n = AddStageBlocks(engine, sceneId, comp.blocks, size, "tpl", "text", slide.flowText,
                                         style.readingFontPx);
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
                                         style.readingFontPx);
            Logger::Instance().Debug(std::format("Scene built: {} (style '{}', {} blocks)",
                                                sceneId, style.name, n), "SceneBuilder");
            return;
        }
        case SlideComposition::Source::Cleared:
            // A CLEAR: the composition already switched the style's colour and
            // image off (see ComposeSlide), and there are no blocks to draw, so
            // the scene is left EMPTY — the frame the outputs show is
            // keyable/black, which is what clearing means. Logged because "the
            // output is black" and "the output shows nothing on purpose" have
            // to be told apart.
            Logger::Instance().Debug(std::format("Scene built: {} (clear — the frame paints nothing)", sceneId),
                                     "SceneBuilder");
            return;
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
    };
    addContent();
    ApplyReadingFlow(engine, sceneId, slide);
    return sceneId;
}

} // namespace bps::presentation
