#pragma once

// IPresentationCue (docs/specs/19 §Cue system). Every cue type (Slide, Media,
// Audio, Timer, Countdown, Script, MIDI, Plugin) implements this interface.
// Adding a cue type never requires changing existing engine code — plugins
// register new cue implementations.

#include "core/common/Common.hpp"
#include "modules/presentation/PresentationTypes.hpp"

namespace bps::presentation {

// Context handed to a cue when it fires (19 §Cue system: modular cues).
class PresentationRuntime;
struct CueContext {
    PresentationRuntime* runtime = nullptr;
    void* payload = nullptr;   // opaque user payload (media frame, timer handle...)
};

class IPresentationCue {
public:
    virtual ~IPresentationCue() = default;

    virtual CueKind Kind() const noexcept = 0;
    virtual const char* Id() const noexcept = 0;

    // When the cue fires, relative to the presentation clock (seconds).
    virtual double AtSec() const noexcept = 0;

    // Execute the cue. Returns Ok on success, CueFailed otherwise.
    virtual Result<void> Execute(CueContext& ctx) = 0;
};

} // namespace bps::presentation
