#pragma once

// Core rendering types (docs/specs/17). The renderer is fully backend-agnostic:
// these POD types describe geometry/color in engine space; the IGraphicsBackend
// decides how they become pixels.

#include <cstdint>
#include <string>

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
enum class BlendMode : int { Alpha = 0, Additive, Multiply };

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
    uint64_t frames = 0;
};

} // namespace bps::rendering
