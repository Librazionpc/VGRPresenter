#pragma once

// Effects engine (docs/specs/17 §Effects Engine). Blur, glow, shadow, opacity,
// crop, mask, brightness, contrast, saturation. Effects attach to objects or
// layers and are applied as pixel operations by the backend; the numbers below
// are the canonical EffectType ids the backend understands.

#include "core/common/Common.hpp"
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
