#pragma once

#include "core/common/Common.hpp"
#include "core/events/EventBus.hpp"
#include "core/events/Events.hpp"
#include "modules/presentation/IPresentationCue.hpp"
#include "modules/presentation/PresentationCompiler.hpp"
#include "modules/presentation/PresentationNavigator.hpp"
#include "modules/presentation/PresentationStateMachine.hpp"
#include "modules/presentation/PresentationTimeline.hpp"
#include "modules/presentation/PresentationTypes.hpp"

#include <atomic>
#include <map>
#include <string>
#include <vector>

namespace bps::rendering {
class RenderEngine;
}
namespace bps::presentation {
class SceneBuilder;
class PresentationValidator;

class PresentationRuntime {
public:
    PresentationRuntime();

    Result<void> Open(const Presentation& presentation);
    Result<void> Close();
    bool IsOpen() const { return pres_ != nullptr; }
    const Presentation* ActivePresentation() const { return pres_; }
    std::string ActivePresentationId() const;

    // --- Pipeline -------------------------------------------------------------
    Result<std::vector<ValidationIssue>> Validate(const PresentationValidator& validator);
    Result<void> Compile(const PresentationCompiler& compiler,
                         const std::map<std::string, std::string, std::less<>>* sceneIds = nullptr);
    Result<void> Prepare(SceneBuilder& builder, rendering::RenderEngine& engine,
                         const OutputStyleSpec& style = OutputStyleSpec{});

    Result<void> RebuildScenes(SceneBuilder& builder, rendering::RenderEngine& engine,
                               const OutputStyleSpec& style);

    Result<void> SwapLive(const Presentation& content, const PresentationCompiler& compiler);

    void AdvanceStyleRevision() noexcept { styleRevision_.fetch_add(1, std::memory_order_relaxed); }
    uint64_t StyleRevision() const noexcept { return styleRevision_.load(std::memory_order_relaxed); }
    // Timed slides advance themselves on their own durationMs (see Tick). The app turns that OFF for every
    // show that was not given the timer, so one show's timings never drive another.
    void SetTimedAdvance(bool on) noexcept { timedAdvance_ = on; }
    bool TimedAdvance() const noexcept { return timedAdvance_; }
    Result<void> GoLive();
    Result<void> Pause();
    Result<void> Resume();
    Result<void> Stop();
    Result<void> Finish();

    // --- Controller commands ----------------------------------------------------
    Result<void> Next();
    Result<void> Previous();
    Result<void> First();
    Result<void> Last();
    Result<void> JumpTo(size_t index);
    Result<void> JumpById(std::string_view slideId);
    Result<void> JumpByTag(std::string_view tag);
    Result<size_t> Search(std::string_view query) const;
    Result<void> Back();

    Result<size_t> Tick(double dt);

    // --- State ----------------------------------------------------------------
    PresentationState State() const;
    size_t CurrentIndex() const;                 // visible index
    const Slide* CurrentSlide() const;
    const CompiledPresentation& Compiled() const { return compiled_; }
    const std::vector<ValidationIssue>& LastIssues() const { return lastIssues_; }
    double PlaybackTimeSec() const { return clockSec_.load(); }
    PresentationTimeline& Timeline() { return timeline_; }
    const PresentationTimeline& Timeline() const { return timeline_; }
    void ClearTimeline() { timeline_.Clear(); }
    PresentationStateMachine& StateMachine() { return sm_; }

    // --- Cue hooks (media/audio cues delegate here) ------------------------------
    Result<void> FireMediaCue(std::string_view assetId);
    Result<void> FireAudioCue(std::string_view assetId);

    // Recovery: jump to a saved slide and re-enter Ready.
    Result<void> RecoverTo(size_t slideIndex);

    // Event wiring helpers (used by the PresentationEngine facade).
    void WireEvents();
    void UnwireEvents();
    void OnConfigReload(const events::ConfigHotReload& e);

private:
    void PublishSlideChanged(size_t prevIndex, size_t newIndex, const Slide& slide);

    PresentationStateMachine sm_;
    const Presentation* pres_ = nullptr;
    PresentationNavigator nav_;
    PresentationTimeline timeline_;
    CompiledPresentation compiled_;
    std::vector<ValidationIssue> lastIssues_;
    // Style-change epoch (AdvanceStyleRevision/StyleRevision).
    std::atomic<uint64_t> styleRevision_{0};
    std::atomic<double> clockSec_{0.0};
    double autoAdvanceAccum_ = 0.0;
    bool timedAdvance_ = true;   // false: a timed slide waits for the operator
    std::vector<Subscription> subscriptions_;
    bool eventsWired_ = false;
};
} // namespace bps::presentation