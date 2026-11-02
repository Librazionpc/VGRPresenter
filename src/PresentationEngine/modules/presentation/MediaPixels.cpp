#include "modules/presentation/MediaPixels.hpp"

#include "modules/presentation/BlockMeta.hpp"

#include "platform/PlatformAccessor.hpp"
#include "modules/media/FrameSource.hpp"
#include "modules/rendering/PngCodec.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace bps::presentation {
// ---- style background image cache -----------------------------------------
std::mutex g_imageMutex;
std::map<std::string, rendering::RgbaImage> g_imageCache;

namespace {
rendering::RgbaImage LoadImageUncached(const std::string& path) {
    rendering::RgbaImage slot;
    std::string imagePath = path;
    auto& platform = bps::platform::PlatformAccessor::Get();
    if (!platform.Filesystem().Exists(imagePath)) {
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

    rendering::RgbaImage image = LoadImageUncached(path);
    std::lock_guard<std::mutex> lock(g_imageMutex);
    g_imageCache[path] = image;
    return image;
}

// ---- a placed video item's poster frame -----------------------------------
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

rendering::Rect ContainRectIn(int imgW, int imgH, const rendering::Rect& box) {
    if (imgW <= 0 || imgH <= 0 || box.width <= 0 || box.height <= 0)
        return box;
    const float scale = std::min(box.width / imgW, box.height / imgH);
    const float w = imgW * scale, h = imgH * scale;
    return rendering::Rect(box.x + (box.width - w) / 2.0f,
                           box.y + (box.height - h) / 2.0f, w, h);
}

std::string MediaSrcOf(const ContentBlock& block) {
    return MetaString(block.metaJson, "src");
}

// ---------------------------------------------------------------------------
// A placed media file's OWN values (the Edit screen's MEDIA tab)
// ---------------------------------------------------------------------------

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
} // namespace bps::presentation