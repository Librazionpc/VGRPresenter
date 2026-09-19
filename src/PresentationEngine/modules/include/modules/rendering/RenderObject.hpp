#pragma once

// Render objects (docs/specs/17 §Renderable Objects). Every visible object
// derives from RenderObject. Nothing is hardcoded for a "Song" or "Bible" —
// these are generic: text, image, video, shape, background, gradient, overlay,
// countdown, clock. Components (text style, image source, ...) attach data.

#include "core/common/Common.hpp"
#include "modules/rendering/IGraphicsBackend.hpp"
#include "modules/rendering/RenderTypes.hpp"
#include "modules/rendering/Scene.hpp"

#include <memory>
#include <string>
#include <vector>

namespace bps::rendering {

// Component carried by image/video objects: where the pixels come from.
struct ImageSource {
    std::string assetId;      // CAMS uuid (for async load)
    std::string name;
    int width = 0;
    int height = 0;
    RgbaImage image;          // decoded pixels (uploaded by the engine)
    bool loaded = false;
};

struct ImageSourceComponent : Component {
    const char* TypeName() const noexcept override { return "ImageSource"; }
    ImageSource source;
};

// Text content + style component (styles live in TextEngine.hpp; forward decl).
struct TextStyleComponent;   // defined in TextEngine.hpp

struct ShapeComponent : Component {
    const char* TypeName() const noexcept override { return "Shape"; }
    ShapeKind kind = ShapeKind::Rectangle;
    Color fill = Color::White();
    Color stroke = Color::Transparent();
    float strokeWidth = 0.0f;
    float radius = 0.0f;            // rounded-rect corner radius (Rectangle)
    std::vector<Vec2> points;       // Polygon/Path/Bezier control points
};

struct GradientComponent : Component {
    const char* TypeName() const noexcept override { return "Gradient"; }
    bool vertical = true;
    Color top{1, 1, 1, 1};
    Color bottom{0, 0, 0, 1};
};

// Countdown: renders the remaining time as text until targetMs.
struct CountdownComponent : Component {
    const char* TypeName() const noexcept override { return "Countdown"; }
    int64_t targetEpochMs = 0;
    std::string format = "MM:SS";
};

// Clock: renders the current time (UTC/local) as text.
struct ClockComponent : Component {
    const char* TypeName() const noexcept override { return "Clock"; }
    bool local = true;
    std::string format = "HH:MM:SS";
};

// ---------------------------------------------------------------------------
// RenderObject base
// ---------------------------------------------------------------------------
class RenderObject : public Component {
public:
    RenderObject() = default;
    RenderObject(std::string id, std::string name, ObjectKind kind)
        : id_(std::move(id)), name_(std::move(name)), kind_(kind) {}

    virtual ~RenderObject() = default;

    const std::string& Id() const { return id_; }
    const std::string& Name() const { return name_; }
    ObjectKind Kind() const { return kind_; }
    virtual const char* TypeName() const noexcept = 0;

    // Placement (world-space for simple objects; children may nest via nodes).
    Rect& Bounds() { return bounds_; }
    const Rect& Bounds() const { return bounds_; }
    void SetBounds(const Rect& r) { bounds_ = r; }

    Color& Tint() { return tint_; }
    const Color& Tint() const { return tint_; }
    void SetTint(const Color& c) { tint_ = c; }

    float Opacity() const { return opacity_; }
    void SetOpacity(float o) { opacity_ = o; }
    bool Visible() const { return visible_; }
    void SetVisible(bool v) { visible_ = v; }
    float RotationRad() const { return rotationRad_; }
    void SetRotationRad(float r) { rotationRad_ = r; }

    const std::string& LayerId() const { return layerId_; }
    void SetLayer(std::string layerId) { layerId_ = std::move(layerId); }

    // Components (generic data attachment).
    template <typename T>
    T* FindComponent() {
        for (auto& c : components_)
            if (auto* t = dynamic_cast<T*>(c.get())) return t;
        return nullptr;
    }
    template <typename T>
    const T* FindComponent() const {
        for (const auto& c : components_)
            if (auto* t = dynamic_cast<T*>(c.get())) return t;
        return nullptr;
    }
    void AddComponent(std::shared_ptr<Component> c) {
        if (c) components_.push_back(std::move(c));
    }
    const std::vector<std::shared_ptr<Component>>& Components() const {
        return components_;
    }

    // Opacity * tint alpha (effective alpha for compositing).
    float EffectiveAlpha() const { return opacity_ * tint_.a; }

protected:
    std::string id_;
    std::string name_;
    ObjectKind kind_ = ObjectKind::Custom;
    Rect bounds_;
    Color tint_{1, 1, 1, 1};
    float opacity_ = 1.0f;
    float rotationRad_ = 0.0f;
    bool visible_ = true;
    std::string layerId_;
    std::vector<std::shared_ptr<Component>> components_;
};

// ---------------------------------------------------------------------------
// Concrete objects
// ---------------------------------------------------------------------------
class TextObject final : public RenderObject {
public:
    TextObject() { kind_ = ObjectKind::Text; }
    TextObject(std::string id, std::string name, std::string text)
        : RenderObject(std::move(id), std::move(name), ObjectKind::Text),
          text_(std::move(text)) {}
    const char* TypeName() const noexcept override { return "Text"; }
    const std::string& Text() const { return text_; }
    void SetText(std::string t) { text_ = std::move(t); }
    // TextStyleComponent is set via AddComponent (defined in TextEngine.hpp).

private:
    std::string text_;
};

class ImageObject final : public RenderObject {
public:
    ImageObject() { kind_ = ObjectKind::Image; }
    ImageObject(std::string id, std::string name)
        : RenderObject(std::move(id), std::move(name), ObjectKind::Image) {}
    const char* TypeName() const noexcept override { return "Image"; }
    ImageSourceComponent* Source() { return FindComponent<ImageSourceComponent>(); }
    const ImageSourceComponent* Source() const {
        return FindComponent<ImageSourceComponent>();
    }
    void SetImage(RgbaImage img, std::string assetId = {}) {
        auto src = std::make_shared<ImageSourceComponent>();
        src->source.image = std::move(img);
        src->source.assetId = std::move(assetId);
        src->source.loaded = !src->source.image.empty();
        src->source.width = src->source.image.width;
        src->source.height = src->source.image.height;
        components_.push_back(std::move(src));
    }
};

class VideoObject final : public RenderObject {
public:
    VideoObject() { kind_ = ObjectKind::Video; }
    VideoObject(std::string id, std::string name)
        : RenderObject(std::move(id), std::move(name), ObjectKind::Video) {}
    const char* TypeName() const noexcept override { return "Video"; }
    // The engine treats video as a frame source: modules push decoded frames.
    void SetFrame(RgbaImage frame) {
        frame_ = std::move(frame);
    }
    const RgbaImage& Frame() const { return frame_; }
    bool HasFrame() const { return !frame_.empty(); }

private:
    RgbaImage frame_;
};

class ShapeObject final : public RenderObject {
public:
    ShapeObject() { kind_ = ObjectKind::Shape; }
    ShapeObject(std::string id, std::string name)
        : RenderObject(std::move(id), std::move(name), ObjectKind::Shape) {}
    const char* TypeName() const noexcept override { return "Shape"; }
    ShapeComponent* Shape() { return FindComponent<ShapeComponent>(); }
    const ShapeComponent* Shape() const { return FindComponent<ShapeComponent>(); }
    void SetRectangle(Color fill) {
        auto c = std::make_shared<ShapeComponent>();
        c->kind = ShapeKind::Rectangle;
        c->fill = fill;
        components_.push_back(std::move(c));
    }
};

class BackgroundObject final : public RenderObject {
public:
    BackgroundObject() { kind_ = ObjectKind::Background; }
    BackgroundObject(std::string id, std::string name, Color color)
        : RenderObject(std::move(id), std::move(name), ObjectKind::Background) {
        color_ = color;
    }
    const char* TypeName() const noexcept override { return "Background"; }
    Color BackgroundColor() const { return color_; }
    void SetBackgroundColor(Color c) { color_ = c; }

private:
    Color color_{0, 0, 0, 1};
};

class GradientObject final : public RenderObject {
public:
    GradientObject() { kind_ = ObjectKind::Gradient; }
    GradientObject(std::string id, std::string name)
        : RenderObject(std::move(id), std::move(name), ObjectKind::Gradient) {}
    const char* TypeName() const noexcept override { return "Gradient"; }
    GradientComponent* Gradient() { return FindComponent<GradientComponent>(); }
    const GradientComponent* Gradient() const {
        return FindComponent<GradientComponent>();
    }
    void SetVertical(Color top, Color bottom) {
        auto c = std::make_shared<GradientComponent>();
        c->vertical = true;
        c->top = top;
        c->bottom = bottom;
        components_.push_back(std::move(c));
    }
};

class OverlayObject final : public RenderObject {
public:
    OverlayObject() { kind_ = ObjectKind::Overlay; }
    OverlayObject(std::string id, std::string name, std::string assetId)
        : RenderObject(std::move(id), std::move(name), ObjectKind::Overlay),
          assetId_(std::move(assetId)) {}
    const char* TypeName() const noexcept override { return "Overlay"; }
    const std::string& AssetId() const { return assetId_; }
    RgbaImage& Image() { return image_; }
    const RgbaImage& Image() const { return image_; }

private:
    std::string assetId_;
    RgbaImage image_;
};

class CountdownObject final : public RenderObject {
public:
    CountdownObject() { kind_ = ObjectKind::Countdown; }
    CountdownObject(std::string id, std::string name, int64_t targetEpochMs)
        : RenderObject(std::move(id), std::move(name), ObjectKind::Countdown) {
        auto c = std::make_shared<CountdownComponent>();
        c->targetEpochMs = targetEpochMs;
        components_.push_back(std::move(c));
    }
    const char* TypeName() const noexcept override { return "Countdown"; }
    CountdownComponent* Countdown() { return FindComponent<CountdownComponent>(); }
    const CountdownComponent* Countdown() const {
        return FindComponent<CountdownComponent>();
    }
    // Formatted remaining time (MM:SS / HH:MM:SS).
    std::string FormatRemaining(int64_t nowMs) const;
};

class ClockObject final : public RenderObject {
public:
    ClockObject() {
        kind_ = ObjectKind::Clock;
        auto c = std::make_shared<ClockComponent>();
        components_.push_back(std::move(c));
    }
    ClockObject(std::string id, std::string name)
        : RenderObject(std::move(id), std::move(name), ObjectKind::Clock) {
        auto c = std::make_shared<ClockComponent>();
        components_.push_back(std::move(c));
    }
    const char* TypeName() const noexcept override { return "Clock"; }
    ClockComponent* Clock() { return FindComponent<ClockComponent>(); }
    const ClockComponent* Clock() const { return FindComponent<ClockComponent>(); }
};

} // namespace bps::rendering
