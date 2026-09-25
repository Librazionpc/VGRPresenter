#pragma once

// PresentationRuntime (docs/specs/19 §Presentation Runtime + Controller). Runs a
// live presentation: owns the state machine, navigator, timeline, compiled
// form, playback clock and cue context. Receives controller commands
// (Next/Previous/JumpTo/GoLive/Pause/Resume/Stop) and advances playback.
// Orchestration only — no rendering, no display routing.

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

    // --- Binding --------------------------------------------------------------
    // Binds a presentation (state -> Loaded). Validates/compiles/prepares via
    // the explicit pipeline methods below.
    Result<void> Open(const Presentation& presentation);
    Result<void> Close();
    bool IsOpen() const { return pres_ != nullptr; }
    const Presentation* ActivePresentation() const { return pres_; }
    std::string ActivePresentationId() const;

    // --- Pipeline -------------------------------------------------------------
    Result<std::vector<ValidationIssue>> Validate(const PresentationValidator& validator);
    Result<void> Compile(const PresentationCompiler& compiler,
                         const std::map<std::string, std::string, std::less<>>* sceneIds = nullptr);
    // Prepares scenes through the SceneBuilder for every compiled slide.
    // `style` is the on-air output's OutputStyleSpec (empty = unstyled) —
    // passed through to every scene build so a style change made while live
    // is picked up on the next re-Prepare (styled scene ids fingerprint the
    // spec, so no invalidation pass is needed).
    Result<void> Prepare(SceneBuilder& builder, rendering::RenderEngine& engine,
                         const OutputStyleSpec& style = OutputStyleSpec{});

    // Rebuilds every compiled slide's scene under `style` WITHOUT touching the
    // state machine — legal while LIVE (Prepare's Prepared/Ready transitions
    // are not, and the live loop must never bounce through them). The live
    // controller calls this when the on-air style changes (FreeShow's
    // reactive output.style).
    Result<void> RebuildScenes(SceneBuilder& builder, rendering::RenderEngine& engine,
                               const OutputStyleSpec& style);

    // LIVE REPLACE (FreeShow's setOutput("slide", { id: "temp", ... })): rebinds
    // the runtime to new content WHILE LIVE — raw pointer + navigator + clock
    // reset, recompile, all with the state machine staying in Live (Compile's
    // own Compiled transition is illegal from Live). `content` must outlive the
    // run (the runtime stores a raw pointer). Call RebuildScenes afterwards to
    // build the new content's scenes.
    Result<void> SwapLive(const Presentation& content, const PresentationCompiler& compiler);

    // Style-change epoch: PresentationEngine::SetActiveOutputStyle advances
    // it; the live loop compares it per frame (one relaxed atomic read) and
    // rebuilds scenes when it moves — a style pushed mid-show lands within
    // one frame.
    void AdvanceStyleRevision() noexcept { styleRevision_.fetch_add(1, std::memory_order_relaxed); }
    uint64_t StyleRevision() const noexcept { return styleRevision_.load(std::memory_order_relaxed); }
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
    Result<size_t> Search(std::string_view query) const;
    Result<void> Back();

    // --- Playback clock ---------------------------------------------------------
    // Advances the timeline + auto-advance by dt seconds. Only meaningful in
    // Live/Paused states. Returns the number of cues fired.
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
    std::vector<Subscription> subscriptions_;
    bool eventsWired_ = false;
};

} // namespace bps::presentation
