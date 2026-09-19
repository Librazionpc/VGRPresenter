#pragma once

// Built-in presentation cues (docs/specs/19 §Cue system). Each cue implements
// IPresentationCue. Adding a cue type never requires changing existing engine
// code — plugins register new cue implementations with the engine.

#include "core/common/Common.hpp"
#include "modules/presentation/IPresentationCue.hpp"

#include <functional>
#include <string>

namespace bps::presentation {

// Jumps the runtime to a slide by its stable id.
class SlideCue final : public IPresentationCue {
public:
    SlideCue(std::string id, double atSec, std::string targetSlideId)
        : id_(std::move(id)), atSec_(atSec), target_(std::move(targetSlideId)) {}

    CueKind Kind() const noexcept override { return CueKind::Slide; }
    const char* Id() const noexcept override { return id_.c_str(); }
    double AtSec() const noexcept override { return atSec_; }
    Result<void> Execute(CueContext& ctx) override;

private:
    std::string id_;
    double atSec_ = 0.0;
    std::string target_;
};

// Fires a payload event (media/audio/timer/countdown semantics are delegated to
// the payload holder — the engine only carries the trigger + id).
class MediaCue final : public IPresentationCue {
public:
    MediaCue(std::string id, double atSec, std::string assetId)
        : id_(std::move(id)), atSec_(atSec), assetId_(std::move(assetId)) {}

    CueKind Kind() const noexcept override { return CueKind::Media; }
    const char* Id() const noexcept override { return id_.c_str(); }
    double AtSec() const noexcept override { return atSec_; }
    Result<void> Execute(CueContext& ctx) override;

private:
    std::string id_;
    double atSec_ = 0.0;
    std::string assetId_;
};

class AudioCue final : public IPresentationCue {
public:
    AudioCue(std::string id, double atSec, std::string assetId)
        : id_(std::move(id)), atSec_(atSec), assetId_(std::move(assetId)) {}

    CueKind Kind() const noexcept override { return CueKind::Audio; }
    const char* Id() const noexcept override { return id_.c_str(); }
    double AtSec() const noexcept override { return atSec_; }
    Result<void> Execute(CueContext& ctx) override;

private:
    std::string id_;
    double atSec_ = 0.0;
    std::string assetId_;
};

// Timer cue: carries a duration (seconds); execution reports via payload.
class TimerCue final : public IPresentationCue {
public:
    TimerCue(std::string id, double atSec, double durationSec)
        : id_(std::move(id)), atSec_(atSec), durationSec_(durationSec) {}

    CueKind Kind() const noexcept override { return CueKind::Timer; }
    const char* Id() const noexcept override { return id_.c_str(); }
    double AtSec() const noexcept override { return atSec_; }
    Result<void> Execute(CueContext& ctx) override;

    double DurationSec() const noexcept { return durationSec_; }

private:
    std::string id_;
    double atSec_ = 0.0;
    double durationSec_ = 0.0;
};

// Countdown cue: same trigger semantics as a timer; kind identifies intent.
class CountdownCue final : public IPresentationCue {
public:
    CountdownCue(std::string id, double atSec, double durationSec)
        : id_(std::move(id)), atSec_(atSec), durationSec_(durationSec) {}

    CueKind Kind() const noexcept override { return CueKind::Countdown; }
    const char* Id() const noexcept override { return id_.c_str(); }
    double AtSec() const noexcept override { return atSec_; }
    Result<void> Execute(CueContext& ctx) override;

    double DurationSec() const noexcept { return durationSec_; }

private:
    std::string id_;
    double atSec_ = 0.0;
    double durationSec_ = 0.0;
};

// Script cue: runs an arbitrary callback (plugin hooks, automation).
class ScriptCue final : public IPresentationCue {
public:
    using Fn = std::function<Result<void>(CueContext&)>;
    ScriptCue(std::string id, double atSec, Fn fn)
        : id_(std::move(id)), atSec_(atSec), fn_(std::move(fn)) {}

    CueKind Kind() const noexcept override { return CueKind::Script; }
    const char* Id() const noexcept override { return id_.c_str(); }
    double AtSec() const noexcept override { return atSec_; }
    Result<void> Execute(CueContext& ctx) override;

private:
    std::string id_;
    double atSec_ = 0.0;
    Fn fn_;
};

} // namespace bps::presentation
