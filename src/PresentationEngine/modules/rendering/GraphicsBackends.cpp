#include "modules/rendering/GraphicsBackends.hpp"

#include "core/logging/Logger.hpp"
#include "modules/rendering/ImageScale.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <format>
#include <mutex>
#include <set>
#include <vector>

namespace bps::rendering {

// ===========================================================================
// Shared pixel helpers
// ===========================================================================

namespace {

inline float Clamp01(float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }
inline uint8_t To8(float v) {
    return static_cast<uint8_t>(v < 0.0f ? 0.0f : (v > 1.0f ? 255.0f : v * 255.0f));
}

// Straight-alpha blend helpers.
inline uint32_t Pack8(uint32_t r, uint32_t g, uint32_t b, uint32_t a) {
    return (a << 24) | (b << 16) | (g << 8) | r;
}

// Scales a packed colour's alpha — the coverage of a partially covered pixel
// (anti-aliasing) or an edge feather.
inline uint32_t ScaleAlpha(uint32_t px, float k) {
    const float a = static_cast<float>((px >> 24) & 0xFF) * k;
    const uint32_t ai = static_cast<uint32_t>(a < 0.0f ? 0.0f : (a > 255.0f ? 255.0f : a));
    return (px & 0x00FFFFFFu) | (ai << 24);
}

} // namespace

uint32_t SoftwareGraphicsBackend::BlendPixel(uint32_t dst, uint32_t src, BlendMode mode) {
    const float sa = static_cast<float>((src >> 24) & 0xFF) / 255.0f;
    const float da = static_cast<float>((dst >> 24) & 0xFF) / 255.0f;
    const float sr = static_cast<float>((src >> 0) & 0xFF) / 255.0f;
    const float sg = static_cast<float>((src >> 8) & 0xFF) / 255.0f;
    const float sb = static_cast<float>((src >> 16) & 0xFF) / 255.0f;
    const float dr = static_cast<float>((dst >> 0) & 0xFF) / 255.0f;
    const float dg = static_cast<float>((dst >> 8) & 0xFF) / 255.0f;
    const float db = static_cast<float>((dst >> 16) & 0xFF) / 255.0f;

    float or_, og, ob, oa;
    switch (mode) {
        case BlendMode::Additive:
            or_ = dr + sr * sa;
            og = dg + sg * sa;
            ob = db + sb * sa;
            oa = std::max(da, sa);
            break;
        case BlendMode::Multiply:
            or_ = dr * (sr + (1.0f - sr) * (1.0f - sa));
            og = dg * (sg + (1.0f - sg) * (1.0f - sa));
            ob = db * (sb + (1.0f - sb) * (1.0f - sa));
            oa = std::max(da, sa);
            break;
        case BlendMode::Alpha:
        default: {
            const float outA = sa + da * (1.0f - sa);
            if (outA <= 0.0f) return 0;
            or_ = (sr * sa + dr * da * (1.0f - sa)) / outA;
            og = (sg * sa + dg * da * (1.0f - sa)) / outA;
            ob = (sb * sa + db * da * (1.0f - sa)) / outA;
            oa = outA;
            break;
        }
    }
    return Pack8(To8(or_), To8(og), To8(ob), To8(oa));
}

void SoftwareGraphicsBackend::ApplyEffect(RgbaImage& img, const Rect& region, int effect,
                                          float param) {
    if (img.empty()) return;
    const int x0 = std::max(0, static_cast<int>(region.x));
    const int y0 = std::max(0, static_cast<int>(region.y));
    const int x1 = std::min(img.width, static_cast<int>(region.Right()));
    const int y1 = std::min(img.height, static_cast<int>(region.Bottom()));
    if (x1 <= x0 || y1 <= y0) return;

    switch (effect) {
        // EffectType::Brightness / Contrast / Saturation / Opacity
        case 3: {   // Opacity — scale alpha in region
            const float k = Clamp01(param);
            for (int y = y0; y < y1; ++y)
                for (int x = x0; x < x1; ++x) {
                    uint32_t& px = img.pixels[static_cast<size_t>(y) * img.width + x];
                    uint32_t a = static_cast<uint32_t>(
                        static_cast<float>((px >> 24) & 0xFF) * k);
                    px = (px & 0x00FFFFFFu) | (a << 24);
                }
            break;
        }
        case 4: {   // Brightness [0..2]
            const float k = param;
            for (int y = y0; y < y1; ++y)
                for (int x = x0; x < x1; ++x) {
                    uint32_t& px = img.pixels[static_cast<size_t>(y) * img.width + x];
                    const auto r = static_cast<uint32_t>(
                        std::min(255.0f, static_cast<float>((px >> 0) & 0xFF) * k));
                    const auto g = static_cast<uint32_t>(
                        std::min(255.0f, static_cast<float>((px >> 8) & 0xFF) * k));
                    const auto b = static_cast<uint32_t>(
                        std::min(255.0f, static_cast<float>((px >> 16) & 0xFF) * k));
                    px = Pack8(r, g, b, (px >> 24) & 0xFF);
                }
            break;
        }
        case 5: {   // Contrast [0..2]; 1 = identity
            const float c = std::max(0.0f, param);
            for (int y = y0; y < y1; ++y)
                for (int x = x0; x < x1; ++x) {
                    uint32_t& px = img.pixels[static_cast<size_t>(y) * img.width + x];
                    auto adj = [c](uint32_t v) {
                        const float f = static_cast<float>(v) / 255.0f;
                        return To8((f - 0.5f) * c + 0.5f);
                    };
                    px = Pack8(adj((px >> 0) & 0xFF), adj((px >> 8) & 0xFF),
                               adj((px >> 16) & 0xFF), (px >> 24) & 0xFF);
                }
            break;
        }
        case 6: {   // Saturation [0..2]; 1 = identity
            const float s = std::max(0.0f, param);
            for (int y = y0; y < y1; ++y)
                for (int x = x0; x < x1; ++x) {
                    uint32_t& px = img.pixels[static_cast<size_t>(y) * img.width + x];
                    const float r = static_cast<float>((px >> 0) & 0xFF) / 255.0f;
                    const float g = static_cast<float>((px >> 8) & 0xFF) / 255.0f;
                    const float b = static_cast<float>((px >> 16) & 0xFF) / 255.0f;
                    const float l = 0.2126f * r + 0.7152f * g + 0.0722f * b;
                    px = Pack8(To8((l + (r - l) * s)), To8((l + (g - l) * s)),
                               To8((l + (b - l) * s)), (px >> 24) & 0xFF);
                }
            break;
        }
        case 0: {   // Blur (box blur, param = radius)
            const int radius = std::max(1, static_cast<int>(param));
            RgbaImage tmp = img;
            for (int y = y0; y < y1; ++y)
                for (int x = x0; x < x1; ++x) {
                    long sumR = 0, sumG = 0, sumB = 0, sumA = 0, n = 0;
                    for (int dy = -radius; dy <= radius; ++dy)
                        for (int dx = -radius; dx <= radius; ++dx) {
                            const int sx = x + dx, sy = y + dy;
                            if (sx < 0 || sx >= img.width || sy < 0 || sy >= img.height)
                                continue;
                            const uint32_t sp =
                                img.pixels[static_cast<size_t>(sy) * img.width + sx];
                            sumR += (sp >> 0) & 0xFF;
                            sumG += (sp >> 8) & 0xFF;
                            sumB += (sp >> 16) & 0xFF;
                            sumA += (sp >> 24) & 0xFF;
                            ++n;
                        }
                    if (n == 0) continue;
                    tmp.pixels[static_cast<size_t>(y) * img.width + x] =
                        Pack8(static_cast<uint32_t>(sumR / n),
                              static_cast<uint32_t>(sumG / n),
                              static_cast<uint32_t>(sumB / n),
                              static_cast<uint32_t>(sumA / n));
                }
            img.pixels.swap(tmp.pixels);
            break;
        }
        default:
            break;   // Glow/Shadow/Crop/Mask handled as composite ops upstream
    }
}

// ===========================================================================
// NullGraphicsBackend
// ===========================================================================

Result<TextureId> NullGraphicsBackend::CreateTexture(const RgbaImage& image) {
    std::lock_guard<std::mutex> lock(mutex_);
    TextureId id{nextTexture_++};
    textures_[id.id] = image;
    textureBytes_ += image.ByteSize();
    return id;
}

Result<void> NullGraphicsBackend::UpdateTexture(TextureId id, const RgbaImage& image) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = textures_.find(id.id);
    if (it == textures_.end()) return Error::Make(Err::Render_TextureNotFound, "Render", "no texture");
    textureBytes_ -= it->second.ByteSize();
    it->second = image;
    textureBytes_ += image.ByteSize();
    return Ok();
}

Result<void> NullGraphicsBackend::DestroyTexture(TextureId id) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = textures_.find(id.id);
    if (it == textures_.end()) return Error::Make(Err::Render_TextureNotFound, "Render", "no texture");
    textureBytes_ -= it->second.ByteSize();
    textures_.erase(it);
    return Ok();
}

RgbaImage NullGraphicsBackend::ReadTexture(TextureId id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = textures_.find(id.id);
    return it == textures_.end() ? RgbaImage{} : it->second;
}

Result<uint32_t> NullGraphicsBackend::CreateShader(std::string_view name,
                                                   std::string_view src) {
    std::lock_guard<std::mutex> lock(mutex_);
    const uint32_t id = nextShader_++;
    shaders_[id] = std::string(src);
    (void)name;
    return id;
}

Result<void> NullGraphicsBackend::DestroyShader(uint32_t id) {
    std::lock_guard<std::mutex> lock(mutex_);
    shaders_.erase(id);
    return Ok();
}

Result<void> NullGraphicsBackend::BeginFrame(int width, int height) {
    std::lock_guard<std::mutex> lock(mutex_);
    frameWidth_ = width;
    frameHeight_ = height;
    return Ok();
}

Result<void> NullGraphicsBackend::Submit(const std::vector<DrawCommand>& commands) {
    std::lock_guard<std::mutex> lock(mutex_);
    submitCount_ += commands.size();
    return Ok();
}

Result<RgbaImage> NullGraphicsBackend::Readback() const {
    std::lock_guard<std::mutex> lock(mutex_);
    RgbaImage img;
    img.width = frameWidth_;
    img.height = frameHeight_;
    img.pixels.assign(static_cast<size_t>(frameWidth_) * frameHeight_, 0u);
    return img;
}

Result<void> NullGraphicsBackend::Shutdown() {
    std::lock_guard<std::mutex> lock(mutex_);
    textures_.clear();
    shaders_.clear();
    textureBytes_ = 0;
    return Ok();
}

// ===========================================================================
// SoftwareGraphicsBackend
// ===========================================================================

Result<TextureId> SoftwareGraphicsBackend::CreateTexture(const RgbaImage& image) {
    std::lock_guard<std::mutex> lock(mutex_);
    TextureId id{nextTexture_++};
    textures_[id.id] = image;
    textureBytes_ += image.ByteSize();
    return id;
}

Result<void> SoftwareGraphicsBackend::UpdateTexture(TextureId id, const RgbaImage& image) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = textures_.find(id.id);
    if (it == textures_.end()) return Error::Make(Err::Render_TextureNotFound, "Render", "no texture");
    textureBytes_ -= it->second.ByteSize();
    it->second = image;
    textureBytes_ += image.ByteSize();
    return Ok();
}

Result<void> SoftwareGraphicsBackend::DestroyTexture(TextureId id) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = textures_.find(id.id);
    if (it == textures_.end()) return Error::Make(Err::Render_TextureNotFound, "Render", "no texture");
    textureBytes_ -= it->second.ByteSize();
    textures_.erase(it);
    return Ok();
}

RgbaImage SoftwareGraphicsBackend::ReadTexture(TextureId id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = textures_.find(id.id);
    return it == textures_.end() ? RgbaImage{} : it->second;
}

Result<uint32_t> SoftwareGraphicsBackend::CreateShader(std::string_view name,
                                                       std::string_view src) {
    std::lock_guard<std::mutex> lock(mutex_);
    const uint32_t id = nextShader_++;
    shaders_[id] = std::string(src);
    (void)name;
    return id;
}

Result<void> SoftwareGraphicsBackend::DestroyShader(uint32_t id) {
    std::lock_guard<std::mutex> lock(mutex_);
    shaders_.erase(id);
    return Ok();
}

Result<void> SoftwareGraphicsBackend::BeginFrame(int width, int height) {
    std::lock_guard<std::mutex> lock(mutex_);
    fbWidth_ = width;
    fbHeight_ = height;
    fb_.width = width;
    fb_.height = height;
    fb_.pixels.assign(static_cast<size_t>(width) * height, 0u);
    pending_ = PendingEffect{};
    return Ok();
}

Result<void> SoftwareGraphicsBackend::Submit(const std::vector<DrawCommand>& commands) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& cmd : commands) Execute(cmd);
    return Ok();
}

Result<RgbaImage> SoftwareGraphicsBackend::Readback() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return fb_;
}

Result<void> SoftwareGraphicsBackend::Shutdown() {
    std::lock_guard<std::mutex> lock(mutex_);
    textures_.clear();
    shaders_.clear();
    textureBytes_ = 0;
    fb_ = RgbaImage{};
    return Ok();
}

// ---------------------------------------------------------------------------
// Command execution
// ---------------------------------------------------------------------------

void SoftwareGraphicsBackend::Execute(const DrawCommand& cmd) {
    switch (cmd.type) {
        case DrawCommand::Type::Clear: {
            std::fill(fb_.pixels.begin(), fb_.pixels.end(), cmd.color.Pack());
            break;
        }
        case DrawCommand::Type::SetBlend:
            break;   // per-command blend stored in cmd.blend
        case DrawCommand::Type::FillRect:
            FillRectBlended(static_cast<int>(cmd.rect.x), static_cast<int>(cmd.rect.y),
                            static_cast<int>(cmd.rect.width),
                            static_cast<int>(cmd.rect.height), cmd.color,
                            static_cast<BlendMode>(cmd.blend));
            break;
        case DrawCommand::Type::BlitImage: {
            auto it = textures_.find(cmd.texture.id);
            if (it == textures_.end()) {
                // ONE-SHOT PER TEXTURE ID: a BlitImage whose texture the backend
                // does not hold draws NOTHING, so a stale id (the resource
                // manager's id table outliving a backend rebind) silently turns
                // a fully composed scene into background-only pixels — the
                // "preview fine, outputs black" shape. Says so once.
                static std::mutex m;
                static std::set<uint64_t> seen;
                bool first = false;
                {
                    std::lock_guard<std::mutex> lock(m);
                    first = seen.insert(cmd.texture.id).second;
                }
                if (first)
                    Logger::Instance().Warning(
                        std::format("blit: texture id {} not held by this backend — nothing drawn",
                                    cmd.texture.id),
                        "Graphics");
                break;
            }
            const RgbaImage& src = it->second;
            Blit(static_cast<int>(cmd.rect.x), static_cast<int>(cmd.rect.y),
                 static_cast<int>(cmd.rect.width), static_cast<int>(cmd.rect.height),
                 src, cmd.srcRect, cmd.color, static_cast<BlendMode>(cmd.blend));
            break;
        }
        case DrawCommand::Type::DrawGlyph: {
            auto it = textures_.find(cmd.texture.id);
            if (it == textures_.end()) break;
            const RgbaImage& src = it->second;
            // Glyphs live in a SHARED ATLAS: the pipeline emits the glyph's
            // cell in cmd.srcRect. Blitting the whole texture (the old
            // behavior) squashed the entire glyph sheet into every character
            // cell — on-air text rendered as dense noise.
            const Rect cell = (cmd.srcRect.width > 0.0f && cmd.srcRect.height > 0.0f)
                                  ? cmd.srcRect
                                  : Rect(0, 0, static_cast<float>(src.width),
                                         static_cast<float>(src.height));
            Blit(static_cast<int>(cmd.rect.x), static_cast<int>(cmd.rect.y),
                 static_cast<int>(cmd.rect.width), static_cast<int>(cmd.rect.height),
                 src, cell, cmd.color, static_cast<BlendMode>(cmd.blend));
            break;
        }
        case DrawCommand::Type::DrawShape:
            RasterizeShape(cmd);
            break;
        case DrawCommand::Type::PushEffect:
            pending_.active = true;
            pending_.region = cmd.rect;
            pending_.effect = cmd.effect;
            pending_.param = cmd.effectParam;
            break;
        case DrawCommand::Type::PopEffect:
            if (pending_.active) {
                ApplyEffect(fb_, pending_.region, pending_.effect, pending_.param);
                pending_ = PendingEffect{};
            }
            break;
    }
}

void SoftwareGraphicsBackend::FillRectBlended(int x, int y, int w, int h, Color c,
                                              BlendMode mode) {
    const int x0 = std::max(0, x);
    const int y0 = std::max(0, y);
    const int x1 = std::min(fbWidth_, x + w);
    const int y1 = std::min(fbHeight_, y + h);
    const uint32_t src = c.Pack();
    for (int yy = y0; yy < y1; ++yy)
        for (int xx = x0; xx < x1; ++xx) {
            uint32_t& dst = fb_.pixels[static_cast<size_t>(yy) * fbWidth_ + xx];
            dst = BlendPixel(dst, src, mode);
        }
}

// A ROUNDED-rect fill with anti-aliased edges — the engine twin of Qt's
// `Rectangle { radius: … }` that every QML surface uses (DesignPreview's block
// boxes, the Edit canvas, the template previews), so a template's rounded band
// or panel looks the same on the engine's outputs as it does in the app.
//
// Coverage comes from the rounded rect's signed distance: `dist` is NEGATIVE
// inside the shape (the min corner radius away from the boundary), so a pixel
// whose centre sits exactly on an edge lands on 0.5 and the transition band is
// one pixel wide. `radius` is clamped to half the shorter side, like Qt's own
// Rectangle (a radius of width/2 is a stadium/circle, never an over-round),
// and sub-half-pixel radii are routed to the plain rect by the caller — at
// that size a rounded corner is indistinguishable from a square one and the
// SDF would otherwise shade the whole interior below full coverage.
void SoftwareGraphicsBackend::FillRoundedRectBlended(int x, int y, int w, int h,
                                                     float radius, Color c,
                                                     BlendMode mode) {
    if (w <= 0 || h <= 0) return;
    const float fw = static_cast<float>(w);
    const float fh = static_cast<float>(h);
    const float r = std::min(radius, 0.5f * std::min(fw, fh));
    if (r < 0.5f) {
        FillRectBlended(x, y, w, h, c, mode);
        return;
    }
    const int x1 = std::min(fbWidth_, x + w);
    const int y1 = std::min(fbHeight_, y + h);
    const int x0 = std::max(0, x);
    const int y0 = std::max(0, y);
    if (x1 <= x0 || y1 <= y0) return;
    const float fx = static_cast<float>(x);
    const float fy = static_cast<float>(y);
    const uint32_t src = c.Pack();
    for (int yy = y0; yy < y1; ++yy) {
        for (int xx = x0; xx < x1; ++xx) {
            const float px = static_cast<float>(xx) + 0.5f;
            const float py = static_cast<float>(yy) + 0.5f;
            // The corner-arc centre this pixel's quadrant rounds around.
            const float cx = fx + std::clamp(px - fx, r, fw - r);
            const float cy = fy + std::clamp(py - fy, r, fh - r);
            const float dx = px - cx;
            const float dy = py - cy;
            const float dist = std::sqrt(dx * dx + dy * dy) - r;
            const float coverage = std::clamp(0.5f - dist, 0.0f, 1.0f);
            if (coverage <= 0.0f) continue;
            uint32_t& dst = fb_.pixels[static_cast<size_t>(yy) * fbWidth_ + xx];
            dst = BlendPixel(dst, coverage >= 1.0f ? src : ScaleAlpha(src, coverage), mode);
        }
    }
}

void SoftwareGraphicsBackend::Blit(int x, int y, int w, int h, const RgbaImage& src,
                                   const Rect& srcRect, Color tint, BlendMode mode) {
    if (src.empty() || w <= 0 || h <= 0) return;
    const float sx0 = srcRect.x, sy0 = srcRect.y;
    const float sw = std::max(1.0f, srcRect.width);
    const float sh = std::max(1.0f, srcRect.height);
    const int x0 = std::max(0, x), y0 = std::max(0, y);
    const int x1 = std::min(fbWidth_, x + w), y1 = std::min(fbHeight_, y + h);
    // Full tint (RGB * alpha), not just alpha: the glyph atlas is WHITE — a
    // text's color arrives as this tint (DrawGlyph -> cmd.color). Scaling
    // alpha only painted every on-air text white and dropped styled colors
    // (the Table template's amber reference line among them).
    const bool tinted = tint.r < 1.0f || tint.g < 1.0f || tint.b < 1.0f || tint.a < 1.0f;
    const int tr = static_cast<int>(tint.r * 255.0f + 0.5f);
    const int tg = static_cast<int>(tint.g * 255.0f + 0.5f);
    const int tb = static_cast<int>(tint.b * 255.0f + 0.5f);
    const int ta = static_cast<int>(tint.a * 255.0f + 0.5f);

    // ---- THE FILTER -------------------------------------------------------
    // A draw at the source's OWN size is a straight copy, and it stays one:
    // that is the glyph atlas, a fullscreen image at the stage's resolution, a
    // 1:1 video — the pixel-exact path everything already relies on. Anything
    // else is resampled through ImageScale (bilinear when magnifying, a box
    // average when minifying) instead of the nearest-pixel grab that made media
    // and background pictures look blocky on the outputs (see ImageScale.hpp).
    const bool unitScale = w == static_cast<int>(sw) && h == static_cast<int>(sh);
    std::vector<ScaleTaps> xTaps, yTaps;
    if (!unitScale) {
        xTaps.resize(static_cast<size_t>(w));
        yTaps.resize(static_cast<size_t>(h));
        const double stepX = static_cast<double>(sw) / w;
        const double stepY = static_cast<double>(sh) / h;
        for (int dx = 0; dx < w; ++dx)
            BuildScaleTaps(sx0 + dx * stepX, stepX, src.width, xTaps[static_cast<size_t>(dx)]);
        for (int dy = 0; dy < h; ++dy)
            BuildScaleTaps(sy0 + dy * stepY, stepY, src.height, yTaps[static_cast<size_t>(dy)]);
    }

    for (int yy = y0; yy < y1; ++yy)
        for (int xx = x0; xx < x1; ++xx) {
            uint32_t sp = 0;
            if (unitScale) {
                const int sx = static_cast<int>(sx0) + (xx - x);
                const int sy = static_cast<int>(sy0) + (yy - y);
                if (sx < 0 || sx >= src.width || sy < 0 || sy >= src.height) continue;
                sp = src.pixels[static_cast<size_t>(sy) * src.width + sx];
            } else {
                sp = CombineTaps(src, xTaps[static_cast<size_t>(xx - x)],
                                 yTaps[static_cast<size_t>(yy - y)]);
            }
            if (tinted) {
                // Pack is 0xAABBGGRR — R in the low byte.
                const int cr = static_cast<int>((sp >> 0) & 0xFF);
                const int cg = static_cast<int>((sp >> 8) & 0xFF);
                const int cb = static_cast<int>((sp >> 16) & 0xFF);
                const int ca = static_cast<int>((sp >> 24) & 0xFF);
                sp = (static_cast<uint32_t>(ca * ta / 255) << 24)
                   | (static_cast<uint32_t>(cb * tb / 255) << 16)
                   | (static_cast<uint32_t>(cg * tg / 255) << 8)
                   | static_cast<uint32_t>(cr * tr / 255);
            }
            uint32_t& dst = fb_.pixels[static_cast<size_t>(yy) * fbWidth_ + xx];
            dst = BlendPixel(dst, sp, mode);
        }
}

void SoftwareGraphicsBackend::RasterizeShape(const DrawCommand& cmd) {
    const auto& r = cmd.rect;
    // Shapes are rasterized as filled rects/ellipses; polygons are handled by
    // the scene layer above as a list of rects in the common cases.
    if (cmd.color.a <= 0.0f) return;
    // A rounded rectangle takes the anti-aliased path; a square one (the
    // default, and every pre-radius caller) keeps the exact old fill.
    if (cmd.cornerRadius >= 0.5f) {
        FillRoundedRectBlended(static_cast<int>(r.x), static_cast<int>(r.y),
                               static_cast<int>(r.width), static_cast<int>(r.height),
                               cmd.cornerRadius, cmd.color, BlendMode::Alpha);
        return;
    }
    FillRectBlended(static_cast<int>(r.x), static_cast<int>(r.y),
                    static_cast<int>(r.width), static_cast<int>(r.height), cmd.color,
                    BlendMode::Alpha);
}

} // namespace bps::rendering
