#pragma once

// Animation + Transition engines (docs/specs/17 §Animation Engine / §Transition
// Engine). Timeline-based property animation (position/scale/rotation/opacity/
// color/mask/custom) with easing, plus the built-in transition types
// (fade/slide/push/zoom/reveal/wipe/crossfade). All interpolation is
// deterministic and unit-testable.

#include "core/common/Common.hpp"
#include "modules/rendering/RenderObject.hpp"
#include "modules/rendering/RenderTypes.hpp"

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace bps::rendering {

// --- Easing functions ------------------------------------------------------
enum class Easing : int {
    Linear = 0,
    EaseIn, EaseOut, EaseInOut,
    BackIn, BackOut, BackInOut,
    ElasticIn, ElasticOut,
    BounceOut,
};

double Ease(Easing e, double t);   // t in [0,1] → eased value

// --- Animatable property ---------------------------------------------------
enum class AnimProperty : int {
    PositionX, PositionY,
    ScaleX, ScaleY,
    Rotation,
    Opacity,
    ColorR, ColorG, ColorB, ColorA,
    Mask,        // 0..1 clip/wipe progress
    Custom,      // generic float channel
};

struct Keyframe {
    double time = 0.0;         // seconds
    double value = 0.0;
    Easing ease = Easing::Linear;
};

struct AnimTrack {
    std::string objectId;      // RenderObject id
    AnimProperty property = AnimProperty::Opacity;
    std::vector<Keyframe> keyframes;
    // Value at time t (clamped to the track's span).
    double Sample(double t) const;
    double Duration() const;
};

// Animator: plays a set of tracks against a collection of objects.
class Animator {
public:
    void AddTrack(AnimTrack track);
    void Clear() { tracks_.clear(); playing_ = false; }

    void Play() { t_ = 0.0; playing_ = true; }
    void Pause() { playing_ = false; }
    void Stop() { playing_ = false; t_ = 0.0; }
    void Seek(double t) { t_ = t < 0 ? 0 : t; }
    bool Playing() const { return playing_; }
    double Time() const { return t_; }
    double Duration() const;             // longest track
    // Completed when the clock passed the last keyframe (Update stops playback
    // at the end, so this does not require playing_).
    bool Finished() const { return Duration() > 0 && t_ >= Duration(); }

    // Advances time and applies tracks to the given object lookup.
    void Update(double dt, const std::function<RenderObject*(std::string_view)>& lookup);

    size_t TrackCount() const { return tracks_.size(); }

private:
    void Apply(const AnimTrack& track, RenderObject* obj) const;

    std::vector<AnimTrack> tracks_;
    double t_ = 0.0;
    bool playing_ = false;
};

// --- Transitions -----------------------------------------------------------
enum class TransitionType : int {
    Fade = 0, Slide, Push, Zoom, Reveal, Wipe, CrossFade, Custom,
};

inline const char* ToString(TransitionType t) {
    switch (t) {
        case TransitionType::Fade:      return "Fade";
        case TransitionType::Slide:     return "Slide";
        case TransitionType::Push:      return "Push";
        case TransitionType::Zoom:      return "Zoom";
        case TransitionType::Reveal:    return "Reveal";
        case TransitionType::Wipe:      return "Wipe";
        case TransitionType::CrossFade: return "CrossFade";
        case TransitionType::Custom:    return "Custom";
    }
    return "Unknown";
}

enum class TransitionDirection : int { Forward = 0, Backward, Left, Right, Up, Down };

struct TransitionSpec {
    TransitionType type = TransitionType::Fade;
    TransitionDirection direction = TransitionDirection::Forward;
    double durationSec = 0.5;
    Easing ease = Easing::EaseInOut;
};

// Computed per-frame transition state for the incoming ("in") scene.
struct TransitionState {
    double progress = 0.0;      // 0..1
    float inOpacity = 1.0f;     // incoming scene opacity
    float outOpacity = 1.0f;    // outgoing scene opacity
    Vec2 inOffset;              // incoming offset (slides/pushes)
    float inScale = 1.0f;
    float wipe = 0.0f;          // 0..1 wipe coverage (Wipe/Reveal)
};

// TransitionEngine evaluates a transition spec at time t.
class TransitionEngine {
public:
    TransitionState Evaluate(const TransitionSpec& spec, double t) const;
    // Convenience: 0..1 normalized progress from time.
    static double Normalize(double t, double duration) {
        if (duration <= 0.0) return 1.0;
        double p = t / duration;
        return p < 0 ? 0.0 : (p > 1 ? 1.0 : p);
    }
};

} // namespace bps::rendering
