#include "modules/rendering/Animation.hpp"

#include <algorithm>
#include <cmath>

namespace bps::rendering {

namespace {
double Clamp01(double v) { return v < 0 ? 0.0 : (v > 1 ? 1.0 : v); }
double Lerp(double a, double b, double t) { return a + (b - a) * t; }
} // namespace

double Ease(Easing e, double t) {
    t = Clamp01(t);
    switch (e) {
        case Easing::Linear:    return t;
        case Easing::EaseIn:    return t * t;
        case Easing::EaseOut:   return 1.0 - (1.0 - t) * (1.0 - t);
        case Easing::EaseInOut: return t < 0.5 ? 2.0 * t * t : 1.0 - std::pow(-2.0 * t + 2.0, 2) / 2.0;
        case Easing::BackIn: {
            constexpr double c1 = 1.70158, c3 = c1 + 1.0;
            return c3 * t * t * t - c1 * t * t;
        }
        case Easing::BackOut: {
            constexpr double c1 = 1.70158, c3 = c1 + 1.0;
            const double u = t - 1.0;
            return 1.0 + c3 * u * u * u + c1 * u * u;
        }
        case Easing::BackInOut: {
            constexpr double c1 = 1.70158, c2 = c1 * 1.525;
            return t < 0.5
                       ? (std::pow(2.0 * t, 2) * ((c2 + 1.0) * 2.0 * t - c2)) / 2.0
                       : (std::pow(2.0 * t - 2.0, 2) * ((c2 + 1.0) * (t * 2.0 - 2.0) + c2) + 2.0) / 2.0;
        }
        case Easing::ElasticIn: {
            constexpr double c4 = (2.0 * 3.14159265358979) / 3.0;
            return t == 0.0 ? 0.0
                   : t == 1.0 ? 1.0
                              : -std::pow(2.0, 10.0 * t - 10.0) *
                                    std::sin((t * 10.0 - 10.75) * c4);
        }
        case Easing::ElasticOut: {
            constexpr double c4 = (2.0 * 3.14159265358979) / 3.0;
            return t == 0.0 ? 0.0
                   : t == 1.0 ? 1.0
                              : std::pow(2.0, -10.0 * t) *
                                    std::sin((t * 10.0 - 0.75) * c4) + 1.0;
        }
        case Easing::BounceOut: {
            const double n1 = 7.5625, d1 = 2.75;
            if (t < 1.0 / d1) return n1 * t * t;
            if (t < 2.0 / d1) { t -= 1.5 / d1; return n1 * t * t + 0.75; }
            if (t < 2.5 / d1) { t -= 2.25 / d1; return n1 * t * t + 0.9375; }
            t -= 2.625 / d1;
            return n1 * t * t + 0.984375;
        }
    }
    return t;
}

// ---------------------------------------------------------------------------
// AnimTrack
// ---------------------------------------------------------------------------

double AnimTrack::Sample(double t) const {
    if (keyframes.empty()) return 0.0;
    if (keyframes.size() == 1) return keyframes[0].value;
    if (t <= keyframes.front().time) return keyframes.front().value;
    if (t >= keyframes.back().time) return keyframes.back().value;
    for (size_t i = 1; i < keyframes.size(); ++i) {
        const auto& k0 = keyframes[i - 1];
        const auto& k1 = keyframes[i];
        if (t <= k1.time) {
            const double span = k1.time - k0.time;
            const double local = span > 0 ? (t - k0.time) / span : 1.0;
            return Lerp(k0.value, k1.value, Ease(k1.ease, local));
        }
    }
    return keyframes.back().value;
}

double AnimTrack::Duration() const {
    return keyframes.empty() ? 0.0 : keyframes.back().time;
}

// ---------------------------------------------------------------------------
// Animator
// ---------------------------------------------------------------------------

void Animator::AddTrack(AnimTrack track) {
    if (!track.keyframes.empty()) tracks_.push_back(std::move(track));
}

double Animator::Duration() const {
    double d = 0.0;
    for (const auto& t : tracks_) d = std::max(d, t.Duration());
    return d;
}

void Animator::Apply(const AnimTrack& track, RenderObject* obj) const {
    if (!obj) return;
    const double v = track.Sample(t_);
    switch (track.property) {
        case AnimProperty::PositionX: obj->Bounds().x = static_cast<float>(v); break;
        case AnimProperty::PositionY: obj->Bounds().y = static_cast<float>(v); break;
        case AnimProperty::ScaleX: {
            auto& b = obj->Bounds();
            const float cx = b.x + b.width / 2.0f;
            b.width = static_cast<float>(v);
            b.x = cx - b.width / 2.0f;
            break;
        }
        case AnimProperty::ScaleY: {
            auto& b = obj->Bounds();
            const float cy = b.y + b.height / 2.0f;
            b.height = static_cast<float>(v);
            b.y = cy - b.height / 2.0f;
            break;
        }
        case AnimProperty::Rotation: obj->SetRotationRad(static_cast<float>(v)); break;
        case AnimProperty::Opacity:  obj->SetOpacity(static_cast<float>(Clamp01(v))); break;
        case AnimProperty::ColorR:   obj->Tint().r = static_cast<float>(v); break;
        case AnimProperty::ColorG:   obj->Tint().g = static_cast<float>(v); break;
        case AnimProperty::ColorB:   obj->Tint().b = static_cast<float>(v); break;
        case AnimProperty::ColorA:   obj->Tint().a = static_cast<float>(v); break;
        case AnimProperty::Mask: {
            // Clamp 0..1; used by wipe/reveal compositing.
            obj->SetOpacity(static_cast<float>(Clamp01(v)));
            break;
        }
        case AnimProperty::Custom:
        default:
            break;
    }
}

void Animator::Update(double dt,
                      const std::function<RenderObject*(std::string_view)>& lookup) {
    if (!playing_) return;
    t_ += dt;
    const double d = Duration();
    if (d > 0.0 && t_ >= d) {
        t_ = d;
        playing_ = false;
    }
    for (const auto& track : tracks_) {
        RenderObject* obj = lookup(track.objectId);
        if (obj) Apply(track, obj);
    }
}

// ---------------------------------------------------------------------------
// TransitionEngine
// ---------------------------------------------------------------------------

TransitionState TransitionEngine::Evaluate(const TransitionSpec& spec, double t) const {
    TransitionState st;
    st.progress = Normalize(t, spec.durationSec);
    const double p = st.progress;
    const double eased = Ease(spec.ease, p);

    switch (spec.type) {
        case TransitionType::Fade:
            st.inOpacity = static_cast<float>(eased);
            st.outOpacity = static_cast<float>(1.0 - eased);
            break;
        case TransitionType::CrossFade:
            st.inOpacity = static_cast<float>(Clamp01(eased * 2.0));
            st.outOpacity = static_cast<float>(Clamp01(1.0 - eased * 2.0));
            break;
        case TransitionType::Slide: {
            const float dx = (spec.direction == TransitionDirection::Right)  ? 1.0f :
                             (spec.direction == TransitionDirection::Left)   ? -1.0f : 0.0f;
            const float dy = (spec.direction == TransitionDirection::Down)   ? 1.0f :
                             (spec.direction == TransitionDirection::Up)     ? -1.0f : 0.0f;
            const float off = 1.0f - static_cast<float>(eased);
            st.inOffset = {dx * off, dy * off};   // in world units (scene size × fraction)
            st.inOpacity = 1.0f;
            st.outOpacity = 1.0f;
            break;
        }
        case TransitionType::Push: {
            const float dx = (spec.direction == TransitionDirection::Right)  ? 1.0f :
                             (spec.direction == TransitionDirection::Left)   ? -1.0f : 0.0f;
            const float dy = (spec.direction == TransitionDirection::Down)   ? 1.0f :
                             (spec.direction == TransitionDirection::Up)     ? -1.0f : 0.0f;
            st.inOffset = {dx * static_cast<float>(eased - 1.0), dy * static_cast<float>(eased - 1.0)};
            st.inOpacity = 1.0f;
            st.outOpacity = 1.0f;
            break;
        }
        case TransitionType::Zoom:
            st.inScale = static_cast<float>(0.8 + 0.2 * eased);
            st.inOpacity = static_cast<float>(eased);
            st.outOpacity = static_cast<float>(1.0 - eased);
            break;
        case TransitionType::Reveal:
        case TransitionType::Wipe:
            st.wipe = static_cast<float>(eased);
            st.inOpacity = 1.0f;
            st.outOpacity = 1.0f;
            break;
        case TransitionType::Custom:
        default:
            st.inOpacity = static_cast<float>(eased);
            break;
    }
    return st;
}

} // namespace bps::rendering
