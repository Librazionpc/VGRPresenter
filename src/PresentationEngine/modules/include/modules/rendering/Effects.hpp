#pragma once

// Effects engine (docs/specs/17 §Effects Engine). Blur, glow, shadow, opacity,
// crop, mask, brightness, contrast, saturation. Effects attach to objects or
// layers and are applied as pixel operations by the backend; the numbers below
// are the canonical EffectType ids the backend understands.

#include "core/common/Common.hpp"
#include "modules/rendering/IGraphicsBackend.hpp"  // RgbaImage
#include "modules/rendering/RenderTypes.hpp"

#include <string>
#include <vector>

namespace bps::rendering {

// Canonical effect ids (match SoftwareGraphicsBackend::ApplyEffect switch).
enum class EffectType : int {
    Blur = 0,       // param = radius px
    Glow = 1,       // param = radius px (composite glow)
    Shadow = 2,     // param = distance (composite offset shadow)
    Opacity = 3,    // param = [0..1]
    Brightness = 4, // param = [0..2], 1 = identity
    Contrast = 5,   // param = [0..2], 1 = identity
    Saturation = 6, // param = [0..2], 1 = identity
    Crop = 7,       // region bounds define the crop (rect)
    Mask = 8,       // param = mask progress [0..1]
};

inline const char* ToString(EffectType e) {
    switch (e) {
        case EffectType::Blur:        return "Blur";
        case EffectType::Glow:        return "Glow";
        case EffectType::Shadow:      return "Shadow";
        case EffectType::Opacity:     return "Opacity";
        case EffectType::Brightness:  return "Brightness";
        case EffectType::Contrast:    return "Contrast";
        case EffectType::Saturation:  return "Saturation";
        case EffectType::Crop:        return "Crop";
        case EffectType::Mask:        return "Mask";
    }
    return "Unknown";
}

struct Effect {
    EffectType type = EffectType::Opacity;
    float param = 1.0f;      // per-type parameter (see enum docs)
    bool enabled = true;
    Effect() = default;
    Effect(EffectType t, float p) : type(t), param(p) {}
};

// ---------------------------------------------------------------------------
// Slide-wide filters
// ---------------------------------------------------------------------------
// FreeShow's edit-panel filter set (its edit/values/filters.ts), with CSS
// filter semantics because that is the model it was ported from: hue rotate,
// invert, blur, grayscale, brightness, contrast, saturate — applied IN THAT
// ORDER, exactly as a CSS `filter:` list applies its functions left to right.
//
// These are SLIDE-level, not per-object defs: the values ride the slide and the
// filter pass runs on the composed frame for that slide, once, so every output
// (HDMI/NDI/preview) shows the same picture and no object has to know about
// filters at all. `FilterValues::IsIdentity()` lets the live path skip the pass
// entirely for the ordinary unfiltered slide — the cost only exists while a
// filter is actually dialled in.
//
// Units match the UI, not the internals: hue in DEGREES (0–360), blur in PIXELS,
// invert and grayscale as 0–1 fractions, and brightness/contrast/saturate as
// multipliers where 1.0 is identity (the rows show them as 0–200%).
struct SlideFilterValues {
    double hueRotateDeg = 0.0;   // 0–360
    double invert = 0.0;         // 0–1
    double blurPx = 0.0;         // 0–100
    double grayscale = 0.0;      // 0–1
    double brightness = 1.0;     // 0–2 (1 = identity)
    double contrast = 1.0;       // 0–2 (1 = identity)
    double saturate = 1.0;       // 0–2 (1 = identity)

    bool operator==(const SlideFilterValues&) const = default;

    // True when every value is its identity — the pass is a no-op then.
    bool IsIdentity() const {
        return hueRotateDeg == 0.0 && invert == 0.0 && blurPx == 0.0 && grayscale == 0.0
            && brightness == 1.0 && contrast == 1.0 && saturate == 1.0;
    }
};

// Applies `f` to every pixel of `img`, in the CSS order above. The blur is the
// same box blur the effect path uses (a separable-friendly average over the
// radius), so the software and GPU composites agree on what "blur 8px" is.
// No-op for an identity filter set, and never touches alpha except through the
// colour channels it transforms.
void ApplyFilters(RgbaImage& img, const SlideFilterValues& f);

// EffectStack: ordered list applied to an object/layer at composite time.
class EffectStack {
public:
    void Add(Effect e) { effects_.push_back(std::move(e)); }
    void Clear() { effects_.clear(); }
    void SetEnabled(size_t index, bool enabled);
    size_t Count() const { return effects_.size(); }
    const std::vector<Effect>& Effects() const { return effects_; }
    bool Has(EffectType t) const;

    // Convenience builders.
    void AddBlur(float radius) { Add(Effect(EffectType::Blur, radius)); }
    void AddGlow(float radius) { Add(Effect(EffectType::Glow, radius)); }
    void AddShadow(float distance) { Add(Effect(EffectType::Shadow, distance)); }
    void AddOpacity(float o) { Add(Effect(EffectType::Opacity, o)); }
    void AddBrightness(float b) { Add(Effect(EffectType::Brightness, b)); }
    void AddContrast(float c) { Add(Effect(EffectType::Contrast, c)); }
    void AddSaturation(float s) { Add(Effect(EffectType::Saturation, s)); }
    void AddCrop(const Rect& region) {
        Effect e(EffectType::Crop, 0.0f);
        crop_ = region;
        effects_.push_back(e);
    }
    Rect CropRegion() const { return crop_; }

private:
    std::vector<Effect> effects_;
    Rect crop_;
};

} // namespace bps::rendering
