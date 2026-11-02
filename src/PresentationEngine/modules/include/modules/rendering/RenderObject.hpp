#pragma once

// Render objects (docs/specs/17 §Renderable Objects). Every visible object
// derives from RenderObject. Nothing is hardcoded for a "Song" or "Bible" —
// these are generic: text, image, video, shape, background, gradient, overlay,
// countdown, clock. Components (text style, image source, ...) attach data.

#include "core/common/Common.hpp"
#include "modules/rendering/IGraphicsBackend.hpp"
#include "modules/rendering/RenderTypes.hpp"
#include "modules/rendering/Scene.hpp"

#include <algorithm>
#include <cmath>
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

    // HOW THIS OBJECT COMPOSITES onto the pixels already in the frame — the
    // Edit screen's MEDIA tab "Blend mode". The default is Alpha, so an object
    // that never asked for a blend draws exactly as it always did; the
    // rasterizer reads this out of the DrawCommand the object produces, which
    // is why a media item's Screen/Multiply is a property of the OBJECT and not
    // of one draw call.
    BlendMode Blend() const { return blend_; }
    void SetBlendMode(BlendMode b) { blend_ = b; }

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
    BlendMode blend_ = BlendMode::Alpha;
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

    // ---- THE READING FLOW (see SceneBuilder::ApplyReadingFlow) -------------
    //
    // A block whose words were cut out of a LONGER passage: its box is a
    // VIEWPORT onto one continuous layout of Text(), and `flowPageLines_` is
    // how many visual lines one page of that passage takes (the size the fit
    // solver was measured against). 0 = an ordinary text object: nothing clips,
    // nothing scrolls, and its drawing is byte-for-byte what it always was.
    //
    // WHY IT LIVES ON THE OBJECT. The reading step (Alt+Down / Alt+Up) can only
    // scroll if the lines arriving from below EXIST in the layout — a block that
    // holds just the page has nothing to show under its last line, so every
    // earlier attempt cut a fresh window of text per press. That re-ran the fit
    // solver, so the type size and the wrapping changed while the words were
    // supposed to travel (the reported "it scrolls then snaps" and "it messed
    // with the template"): one layout, one size, one offset into it is the only
    // shape that scrolls. The offset itself is per-frame (RenderOptions), not
    // part of this object — a scene is built once and rasterised by every
    // output's pass.
    // `pageLines` is how many visual lines ONE page of the passage takes — the
    // layout the fit solver sized the box against, which is what the block's own
    // vertical alignment has to keep using (the box holds a page, not a book).
    // `charAt` is WHERE IN THE PASSAGE the reader is: a character offset into the
    // flow text, not a line. Lines are per-STYLE ("line 7" is a different word in
    // a style that wraps differently), so a line number sent to two outputs of
    // different types sizes put different sentences at the top of each box —
    // measured on the operator's own two styles: a line or two apart, half the
    // time, which is the "the left is still showing while the right has finished"
    // report. A character offset means the same words everywhere; each style turns
    // it into its own line with FlowLineForChar below.
    void SetReadingFlow(int pageLines, float charAt) {
        if (pageLines > 0 && flowPageLines_ == 0)
            pageText_ = text_;
        flowPageLines_ = pageLines > 0 ? pageLines : 0;
        flowCharAt_ = charAt;
    }
    void ClearReadingFlow() {
        if (flowPageLines_ > 0)
            text_ = std::move(pageText_);
        pageText_.clear();
        flowPageLines_ = 0;
        flowCharAt_ = 0.0f;
        flowCharStarts_.clear();
    }
    bool HasReadingFlow() const { return flowPageLines_ > 0; }
    // SCROLL'S MAX-WORDS COLUMN: the passage fills the WHOLE text area from its top edge, with no
    // vertical alignment — the same rule the QML surfaces apply when the reading column is solved
    // from the max-words budget (DesignPreview's flowWindow). Without it the engine centred or
    // bottomed a window one PAGE tall, so an output's words sat differently from the monitor's.
    // Set by the scene builder (OutputStyleSpec::readingFontPx > 0); survives Set/ClearReadingFlow.
    void SetFlowFillsBox(bool on) { flowFillsBox_ = on; }
    bool FlowFillsBox() const { return flowFillsBox_; }
    int FlowPageLines() const { return flowPageLines_; }
    float FlowCharAt() const { return flowCharAt_; }
    // THE PASSAGE'S LINES AS THIS OBJECT'S OWN STYLE WRAPS THEM, as character
    // offsets (one entry per visual line). Built once from the layout the scene
    // already measured (SceneBuilder::ApplyReadingFlow), because the mapping is
    // needed on EVERY frame — a scroll in flight re-reads it sixty times a second —
    // and re-laying out the passage per frame is exactly the cost this file's
    // one-layout design exists to avoid. Empty = no mapping (an ordinary text
    // object, or a slide that carries no passage).
    void SetFlowCharTable(std::vector<int> lineStarts) { flowCharStarts_ = std::move(lineStarts); }
    bool HasFlowCharTable() const { return !flowCharStarts_.empty(); }
    // The position in the char offset, expressed in THIS style's line heights:
    // the line that holds that character, plus the fraction of the move still in
    // flight so a glide stays smooth on every surface. A char offset is a float
    // whose integer part is a character and whose fraction is that progress.
    float FlowLineForChar(float charAt) const {
        if (flowCharStarts_.empty())
            return charAt;   // no table: the caller is already speaking lines
        const float whole = std::floor(charAt);
        const int target = static_cast<int>(whole);
        const auto it = std::upper_bound(flowCharStarts_.begin(), flowCharStarts_.end(), target);
        const int index = (it == flowCharStarts_.begin())
                              ? 0
                              : static_cast<int>(it - flowCharStarts_.begin()) - 1;
        return static_cast<float>(index) + (charAt - whole);
    }

private:
    std::string text_;
    std::string pageText_;
    int flowPageLines_ = 0;
    bool flowFillsBox_ = false;
    float flowCharAt_ = 0.0f;
    std::vector<int> flowCharStarts_;
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
        // Bumped on EVERY push so a renderer can tell a stale upload from a
        // current one. A live frame source (media on air, a camera tap) pushes
        // a new frame every tick through this SAME object, and the GPU texture
        // upload path is create-once/dedup-by-name — without a revision the
        // first frame ever uploaded stayed on the output forever (see
        // GPUResourceManager::UpdateTexture).
        ++frameRev_;
    }
    const RgbaImage& Frame() const { return frame_; }
    bool HasFrame() const { return !frame_.empty(); }
    // THE SOURCE FRAME THESE PIXELS CAME FROM (0 = an uncounted source).
    //
    // A live layer (a taken camera/screen input, a bus window, media on air) is
    // refreshed on every scene build — the pixel pass runs on cache hits too —
    // and one tick builds a scene per output. Re-copying and re-uploading an
    // 8 MB frame that has NOT changed is most of the per-frame cost of a live
    // program feed, and it is pure waste whenever the source runs slower than
    // the composition (which is exactly what a busy machine does to a capture
    // device). The producer's own counter is what lets the refresh tell "the
    // same picture" from "a new one" without comparing a single pixel; it is
    // stored here, on the object, because the object IS the picture being
    // asked about (see SceneBuilder::LayerFrameCurrent).
    uint64_t SourceSeq() const { return sourceSeq_; }
    void SetSourceSeq(uint64_t seq) { sourceSeq_ = seq; }
    // Monotonic version of the pixels currently in Frame() (0 = never pushed).
    uint64_t FrameRevision() const { return frameRev_; }

private:
    RgbaImage frame_;
    uint64_t frameRev_ = 0;
    uint64_t sourceSeq_ = 0;
};

class ShapeObject final : public RenderObject {
public:
    ShapeObject() { kind_ = ObjectKind::Shape; }
    ShapeObject(std::string id, std::string name)
        : RenderObject(std::move(id), std::move(name), ObjectKind::Shape) {}
    const char* TypeName() const noexcept override { return "Shape"; }
    ShapeComponent* Shape() { return FindComponent<ShapeComponent>(); }
    const ShapeComponent* Shape() const { return FindComponent<ShapeComponent>(); }
    // `cornerRadius` is in the shape's OWN (destination) pixels and is clamped
    // by the rasterizer to half the shorter side, exactly like Qt's Rectangle
    // in DesignPreview/the Edit canvas. Defaulted so the square-corner callers
    // (and the rendering phase's own tests) are untouched.
    void SetRectangle(Color fill, float cornerRadius = 0.0f) {
        auto c = std::make_shared<ShapeComponent>();
        c->kind = ShapeKind::Rectangle;
        c->fill = fill;
        c->radius = cornerRadius > 0.0f ? cornerRadius : 0.0f;
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
