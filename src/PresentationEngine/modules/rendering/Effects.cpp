#include "modules/rendering/Effects.hpp"

namespace bps::rendering {

void EffectStack::SetEnabled(size_t index, bool enabled) {
    if (index < effects_.size()) effects_[index].enabled = enabled;
}

bool EffectStack::Has(EffectType t) const {
    for (const auto& e : effects_)
        if (e.type == t && e.enabled) return true;
    return false;
}

} // namespace bps::rendering
