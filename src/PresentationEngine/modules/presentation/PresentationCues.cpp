#include "modules/presentation/PresentationCues.hpp"

#include "core/events/EventBus.hpp"
#include "core/events/Events.hpp"
#include "modules/presentation/PresentationRuntime.hpp"

namespace bps::presentation {

namespace {

// Publishes the canonical presentation.cue_triggered event.
void PublishCue(CueContext& ctx, const char* id, CueKind kind, double atSec) {
    auto& bus = EventBus::Instance();
    (void)bus.Publish(events::PresentationCueTriggered{
        ctx.runtime ? ctx.runtime->ActivePresentationId() : std::string{}, std::string(id),
        static_cast<int>(kind), atSec});
}

} // namespace

Result<void> SlideCue::Execute(CueContext& ctx) {
    if (!ctx.runtime) return Error::Make(Err::Presentation_CueFailed, "SlideCue", "no runtime");
    PublishCue(ctx, id_.c_str(), Kind(), atSec_);
    return ctx.runtime->JumpById(target_);
}

Result<void> MediaCue::Execute(CueContext& ctx) {
    if (!ctx.runtime) return Error::Make(Err::Presentation_CueFailed, "MediaCue", "no runtime");
    PublishCue(ctx, id_.c_str(), Kind(), atSec_);
    return ctx.runtime->FireMediaCue(assetId_);
}

Result<void> AudioCue::Execute(CueContext& ctx) {
    if (!ctx.runtime) return Error::Make(Err::Presentation_CueFailed, "AudioCue", "no runtime");
    PublishCue(ctx, id_.c_str(), Kind(), atSec_);
    return ctx.runtime->FireAudioCue(assetId_);
}

Result<void> TimerCue::Execute(CueContext& ctx) {
    if (!ctx.runtime) return Error::Make(Err::Presentation_CueFailed, "TimerCue", "no runtime");
    PublishCue(ctx, id_.c_str(), Kind(), atSec_);
    return Ok();
}

Result<void> CountdownCue::Execute(CueContext& ctx) {
    if (!ctx.runtime) return Error::Make(Err::Presentation_CueFailed, "CountdownCue",
                                         "no runtime");
    PublishCue(ctx, id_.c_str(), Kind(), atSec_);
    return Ok();
}

Result<void> ScriptCue::Execute(CueContext& ctx) {
    if (!fn_) return Error::Make(Err::Presentation_CueFailed, "ScriptCue", "no callback");
    return fn_(ctx);
}

} // namespace bps::presentation
