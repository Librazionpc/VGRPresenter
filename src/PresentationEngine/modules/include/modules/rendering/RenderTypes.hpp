#pragma once

// Core rendering types (docs/specs/17). The renderer is fully backend-agnostic:
// these POD types describe geometry/color in engine space; the IGraphicsBackend
// decides how they become pixels.

#include <cmath>
#include <cstdint>
#include <string>
#include <string_view>

namespace bps::rendering {

// ---------------------------------------------------------------------------
// Math primitives
// ---------------------------------------------------------------------------
struct Vec2 {
    float x = 0.0f;
    float y = 0.0f;
    Vec2() = default;
    Vec2(float xx, float yy) : x(xx), y(yy) {}
};

struct Size {
    float width = 0.0f;
    float height = 0.0f;
    Size() = default;
    Size(float w, float h) : width(w), height(h) {}
};

struct Rect {
    float x = 0.0f;
    float y = 0.0f;
    float width = 0.0f;
    float height = 0.0f;
    Rect() = default;
    Rect(float xx, float yy, float w, float h) : x(xx), y(yy), width(w), height(h) {}

    float Right() const { return x + width; }
    float Bottom() const { return y + height; }
    bool Contains(float px, float py) const {
        return px >= x && px < Right() && py >= y && py < Bottom();
    }
};

// SNAP A RECT'S EDGES, NEVER ITS SIZE. A design's blocks are authored in stage
// units, so a shared edge (a table's code chip sitting exactly on its paragraph
// band: both say y = 303.6428572) lands on a FRACTIONAL frame coordinate once
// the block is mapped to 1080p. Rounding x and width independently — the
// backends' old `static_cast<int>(rect.x)` / `static_cast<int>(rect.width)` — let
// two abutting boxes each give up a fraction of that edge in opposite
// directions, leaving a ONE-PIXEL hole of live video between them. Over a bright
// camera feed that reads as a light seam around the item, exactly like an
// unfilled border; the canvas (Qt, which snaps a Rectangle's four edges) has no
// such seam, so it only ever showed on the outputs.
//
// Both edges are rounded here, and the size is derived from them: two rects that
// share an edge in stage units therefore share the same pixel boundary, whatever
// the fractional offset. Callers must use this for FILLED boxes (a shape, or a
// text block's own background), not for blits whose size is also their source
// sampling (a glyph cell, a 1:1 image) — those keep their exact geometry.
inline Rect SnapRectToPixelEdges(const Rect& r) {
    const float x0 = std::round(r.x);
    const float y0 = std::round(r.y);
    const float x1 = std::round(r.Right());
    const float y1 = std::round(r.Bottom());
    return Rect(x0, y0, x1 > x0 ? x1 - x0 : 0.0f, y1 > y0 ? y1 - y0 : 0.0f);
}

// 2D transform: translate + rotate (radians) + scale. Applied as
// position → rotate → scale when composing object/local transforms.
struct Transform {
    Vec2 position;
    float rotationRad = 0.0f;
    Vec2 scale{1.0f, 1.0f};

    Transform() = default;
    Transform(Vec2 p) : position(p) {}
};

// ---------------------------------------------------------------------------
// Color
// ---------------------------------------------------------------------------
struct Color {
    float r = 0.0f;
    float g = 0.0f;
    float b = 0.0f;
    float a = 1.0f;
    Color() = default;
    Color(float rr, float gg, float bb, float aa = 1.0f)
        : r(rr), g(gg), b(bb), a(aa) {}

    static Color White() { return {1, 1, 1, 1}; }
    static Color Black() { return {0, 0, 0, 1}; }
    static Color Red() { return {1, 0, 0, 1}; }
    static Color Green() { return {0, 1, 0, 1}; }
    static Color Blue() { return {0, 0, 1, 1}; }
    static Color Transparent() { return {0, 0, 0, 0}; }

    // Premultiplied-free RGBA8 pack (0xAABBGGRR, native little-endian order).
    uint32_t Pack() const {
        uint32_t rr = static_cast<uint32_t>(r * 255.0f + 0.5f) & 0xFF;
        uint32_t gg = static_cast<uint32_t>(g * 255.0f + 0.5f) & 0xFF;
        uint32_t bb = static_cast<uint32_t>(b * 255.0f + 0.5f) & 0xFF;
        uint32_t aa = static_cast<uint32_t>(a * 255.0f + 0.5f) & 0xFF;
        return (aa << 24) | (bb << 16) | (gg << 8) | rr;
    }
    static Color Unpack(uint32_t px) {
        Color c;
        c.r = static_cast<float>((px >> 0) & 0xFF) / 255.0f;
        c.g = static_cast<float>((px >> 8) & 0xFF) / 255.0f;
        c.b = static_cast<float>((px >> 16) & 0xFF) / 255.0f;
        c.a = static_cast<float>((px >> 24) & 0xFF) / 255.0f;
        return c;
    }
    Color WithAlpha(float aa) const { return {r, g, b, aa}; }
};

// ---------------------------------------------------------------------------
// Blending
// ---------------------------------------------------------------------------
// HOW A DRAW COMBINES WITH WHAT IS ALREADY ON THE FRAME.
//
// Alpha/Additive/Multiply are the engine's own three, and their VALUES are part
// of the contract (DrawCommand::blend carries the int across the backend seam),
// so the CSS-media modes the Edit screen's MEDIA tab offers are appended after
// them, never inserted. The names match CSS/FreeShow's own list, which is what
// makes "the dropdown says Overlay" mean the same picture in the app and on the
// outputs; GraphicsBackends' BlendPixel implements the W3C formulas.
enum class BlendMode : int {
    Alpha = 0,
    Additive,
    Multiply,
    Screen,
    Overlay,
    SoftLight,
    HardLight,
    ColorDodge,
    ColorBurn,
    Lighten,
    Darken,
    Difference,
    Exclusion,
    Hue,
    Saturation,
    Color,
    Luminosity,
};

// ---- blend modes travel as NAMES, render as numbers -------------------------
//
// The Edit screen's MEDIA tab stores FreeShow's own lower-case name
// ("normal", "soft-light", "color-dodge" …), that name is what a saved show
// carries, and only the rasterizer cares about the integer. Both directions
// live next to the enum so a new mode is added in ONE place and the QML list,
// the scene builder and the compositor cannot drift apart.
inline const char* BlendModeName(BlendMode m) {
    switch (m) {
        case BlendMode::Additive: return "additive";
        case BlendMode::Multiply: return "multiply";
        case BlendMode::Screen: return "screen";
        case BlendMode::Overlay: return "overlay";
        case BlendMode::SoftLight: return "soft-light";
        case BlendMode::HardLight: return "hard-light";
        case BlendMode::ColorDodge: return "color-dodge";
        case BlendMode::ColorBurn: return "color-burn";
        case BlendMode::Lighten: return "lighten";
        case BlendMode::Darken: return "darken";
        case BlendMode::Difference: return "difference";
        case BlendMode::Exclusion: return "exclusion";
        case BlendMode::Hue: return "hue";
        case BlendMode::Saturation: return "saturation";
        case BlendMode::Color: return "color";
        case BlendMode::Luminosity: return "luminosity";
        case BlendMode::Alpha:
        default: return "normal";
    }
}

// Case/separator tolerant: "soft-light", "SoftLight", "soft light" and
// "soft_light" all land on the same mode, so the name a stored show carries
// never depends on how a writer spelled it.
inline BlendMode ParseBlendMode(std::string_view name) {
    std::string k;
    k.reserve(name.size());
    for (const char ch : name) {
        if (ch == '-' || ch == '_' || ch == ' ')
            continue;
        k.push_back(static_cast<char>(ch >= 'A' && ch <= 'Z' ? ch - 'A' + 'a' : ch));
    }
    if (k.empty() || k == "normal" || k == "sourceover" || k == "alpha")
        return BlendMode::Alpha;
    if (k == "additive" || k == "add") return BlendMode::Additive;
    if (k == "multiply") return BlendMode::Multiply;
    if (k == "screen") return BlendMode::Screen;
    if (k == "overlay") return BlendMode::Overlay;
    if (k == "softlight") return BlendMode::SoftLight;
    if (k == "hardlight") return BlendMode::HardLight;
    if (k == "colordodge" || k == "dodge") return BlendMode::ColorDodge;
    if (k == "colorburn" || k == "burn") return BlendMode::ColorBurn;
    if (k == "lighten" || k == "lighter") return BlendMode::Lighten;
    if (k == "darken" || k == "darker") return BlendMode::Darken;
    if (k == "difference") return BlendMode::Difference;
    if (k == "exclusion") return BlendMode::Exclusion;
    if (k == "hue") return BlendMode::Hue;
    if (k == "saturation" || k == "saturate") return BlendMode::Saturation;
    if (k == "color") return BlendMode::Color;
    if (k == "luminosity" || k == "luminance") return BlendMode::Luminosity;
    return BlendMode::Alpha;   // unknown name = draw it normally, never hide it
}

// ---------------------------------------------------------------------------
// Shape / geometry kinds
// ---------------------------------------------------------------------------
enum class ShapeKind : int {
    Rectangle = 0,
    Circle,
    Ellipse,
    Polygon,   // vertex list
    Bezier,    // control points (cubic)
    Path,      // polyline (open)
};

// ---------------------------------------------------------------------------
// Render object kinds (generic — nothing presentation-specific)
// ---------------------------------------------------------------------------
enum class ObjectKind : int {
    Text = 0,
    Image,
    Video,
    Shape,
    Background,
    Gradient,
    Overlay,
    Countdown,
    Clock,
    Custom,
};

// ---------------------------------------------------------------------------
// Render statistics (docs/specs/17 Diagnostics)
// ---------------------------------------------------------------------------
struct RenderStats {
    double fps = 0.0;
    double frameMs = 0.0;        // last frame CPU time
    double gpuMs = 0.0;          // backend-reported GPU time (software: ~0)
    double cpuMs = 0.0;          // engine-side CPU time
    uint64_t gpuMemoryBytes = 0; // backend allocation accounting
    uint32_t drawCalls = 0;
    uint32_t triangles = 0;
    uint32_t batches = 0;
    uint32_t textureCount = 0;
    uint32_t shaderCount = 0;
    uint32_t frameDrops = 0;
    uint32_t renderQueueSize = 0;
    // HOW MANY DRAW COMMANDS the last frame built and handed to the backend. The
    // number that makes "a frame costs its viewport, not its passage" checkable
    // (see RenderEngine::DrawTextObject's line cull and the
    // rendering · reading-flow-draw-cost suite): a reading block must contribute the
    // visible lines' glyphs, never the whole passage's.
    uint32_t commands = 0;
    uint64_t frames = 0;
};

} // namespace bps::rendering
