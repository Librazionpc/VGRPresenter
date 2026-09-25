#include "modules/presentation/PresentationRuntime.hpp"

#include "core/events/Events.hpp"
#include "core/logging/Logger.hpp"
#include "modules/presentation/PresentationValidator.hpp"
#include "modules/presentation/SceneBuilder.hpp"

namespace bps::presentation {

PresentationRuntime::PresentationRuntime() = default;

// ---------------------------------------------------------------------------
// Binding
// ---------------------------------------------------------------------------
Result<void> PresentationRuntime::Open(const Presentation& presentation) {
    if (pres_)
        return Error::Make(Err::Presentation_AlreadyOpen, "PresentationRuntime",
                           "a presentation is already open");
    pres_ = &presentation;
    nav_.Open(presentation);
    sm_.Reset(PresentationState::Created);
    clockSec_.store(0.0);
    autoAdvanceAccum_ = 0.0;
    timeline_.Clear();
    compiled_ = CompiledPresentation{};
    lastIssues_.clear();
    auto r = sm_.TransitionTo(PresentationState::Loaded);
    if (!r.ok()) return r;
    Logger::Instance().Info("Presentation opened: '" + presentation.name + "' (" +
                                std::format("{} slides)", nav_.Count()),
                            "PresentationRuntime");
    return Ok();
}

Result<void> PresentationRuntime::Close() {
    if (!pres_) return Ok();
    (void)Stop();
    pres_ = nullptr;
    sm_.Reset(PresentationState::Closed);
    nav_.ClearHistory();
    timeline_.Clear();
    compiled_ = CompiledPresentation{};
    lastIssues_.clear();
    return Ok();
}

std::string PresentationRuntime::ActivePresentationId() const {
    return pres_ ? pres_->id : std::string{};
}

// ---------------------------------------------------------------------------
// Pipeline
// ---------------------------------------------------------------------------
Result<std::vector<ValidationIssue>> PresentationRuntime::Validate(
    const PresentationValidator& validator) {
    if (!pres_) return Error::Make(Err::Presentation_NotOpen, "PresentationRuntime",
                                   "no presentation open");
    lastIssues_ = validator.Validate(*pres_);
    auto r = sm_.TransitionTo(PresentationState::Validated);
    if (!r.ok()) return r.error();
    return lastIssues_;
}

Result<void> PresentationRuntime::Compile(
    const PresentationCompiler& compiler,
    const std::map<std::string, std::string, std::less<>>* sceneIds) {
    if (!pres_) return Error::Make(Err::Presentation_NotOpen, "PresentationRuntime",
                                   "no presentation open");
    compiled_ = compiler.Compile(*pres_, lastIssues_, sceneIds);
    if (!compiled_.ok)
        return Error::Make(Err::Presentation_CompileFailed, "PresentationRuntime",
                           "compilation failed (no slides?)");
    if (PresentationValidator::HasErrors(lastIssues_))
        return Error::Make(Err::Presentation_ValidationFailed, "PresentationRuntime",
                           "presentation has validation errors");
    auto r = sm_.TransitionTo(PresentationState::Compiled);
    if (!r.ok()) return r;
    return Ok();
}

Result<void> PresentationRuntime::Prepare(SceneBuilder& builder, rendering::RenderEngine& engine,
                                          const OutputStyleSpec& style) {
    if (!pres_) return Error::Make(Err::Presentation_NotOpen, "PresentationRuntime",
                                   "no presentation open");
    auto r = sm_.TransitionTo(PresentationState::Prepared);
    if (!r.ok()) return r;
    // Pre-build every compiled slide's scene (cache). Styled builds key on a
    // fingerprint of `style` (StyledSceneIdFor), so a style change lands as
    // new scene ids — nothing stale is served and nothing needs purging.
    for (auto& cs : compiled_.slides) {
        auto it = std::find_if(pres_->slides.begin(), pres_->slides.end(),
                               [&](const Slide& s) { return s.id == cs.slideId; });
        if (it != pres_->slides.end()) {
            auto scene = builder.BuildSlideScene(*pres_, *it, style, engine);
            if (scene.ok()) cs.sceneId = scene.value();
        }
    }
    return sm_.TransitionTo(PresentationState::Ready);
}

Result<void> PresentationRuntime::RebuildScenes(SceneBuilder& builder,
                                                rendering::RenderEngine& engine,
                                                const OutputStyleSpec& style) {
    if (!pres_) return Error::Make(Err::Presentation_NotOpen, "PresentationRuntime",
                                   "no presentation open");
    if (compiled_.slides.empty()) return Ok();

    // Same build loop as Prepare, but NO state transitions — legal while the
    // presentation is LIVE (the state machine forbids Live → Prepared, and a
    // style change must never bounce the show off air). cs.sceneId moves to
    // the new style's fingerprinted id; RenderOnce reads it through Compiled()
    // so the very next frame carries the new look.
    for (auto& cs : compiled_.slides) {
        auto it = std::find_if(pres_->slides.begin(), pres_->slides.end(),
                               [&](const Slide& s) { return s.id == cs.slideId; });
        if (it != pres_->slides.end()) {
            auto scene = builder.BuildSlideScene(*pres_, *it, style, engine);
            if (scene.ok()) cs.sceneId = scene.value();
        }
    }
    return Ok();
}

Result<void> PresentationRuntime::SwapLive(const Presentation& content,
                                           const PresentationCompiler& compiler) {
    if (sm_.State() != PresentationState::Live)
        return Error::Make(Err::Presentation_InvalidState, "PresentationRuntime",
                           "SwapLive is only legal while live");
    // Rebind the raw pointer + navigator, reset the clock, recompile — with the
    // state machine never leaving Live. Content is owned by the caller and must
    // outlive the run (same contract as Open).
    pres_ = &content;
    nav_.Open(content);
    clockSec_.store(0.0);
    autoAdvanceAccum_ = 0.0;
    timeline_.Clear();
    compiled_ = compiler.Compile(content, lastIssues_);
    if (!compiled_.ok)
        return Error::Make(Err::Presentation_CompileFailed, "PresentationRuntime",
                           "live swap compile failed");
    return Ok();
}

Result<void> PresentationRuntime::GoLive() {
    if (!pres_) return Error::Make(Err::Presentation_NotOpen, "PresentationRuntime",
                                   "no presentation open");
    auto r = sm_.TransitionTo(PresentationState::Live);
    if (!r.ok()) return r;
    clockSec_.store(0.0);
    autoAdvanceAccum_ = 0.0;
    timeline_.Reset();
    const Slide* slide = CurrentSlide();
    Logger::Instance().Info("Presentation LIVE: '" + pres_->name + "'", "PresentationRuntime");
    (void)EventBus::Instance().Publish(
        events::PresentationStarted{pres_->id, static_cast<int>(CurrentIndex())});
    if (slide)
        (void)EventBus::Instance().Publish(events::SlideChanged{
            static_cast<int>(CurrentIndex()), static_cast<int>(nav_.Count()), slide->title});
    return Ok();
}

Result<void> PresentationRuntime::Pause() {
    auto r = sm_.TransitionTo(PresentationState::Paused);
    if (r.ok())
        (void)EventBus::Instance().Publish(
            events::PresentationPaused{ActivePresentationId()});
    return r;
}

Result<void> PresentationRuntime::Resume() {
    auto r = sm_.TransitionTo(PresentationState::Live);
    if (r.ok())
        (void)EventBus::Instance().Publish(
            events::PresentationResumed{ActivePresentationId()});
    return r;
}

Result<void> PresentationRuntime::Stop() {
    if (sm_.State() != PresentationState::Live && sm_.State() != PresentationState::Paused)
        return Ok();
    auto r = sm_.TransitionTo(PresentationState::Stopped);
    if (r.ok())
        (void)EventBus::Instance().Publish(
            events::PresentationStopped{ActivePresentationId()});
    return r;
}

Result<void> PresentationRuntime::Finish() {
    auto r = sm_.TransitionTo(PresentationState::Finished);
    if (r.ok())
        (void)EventBus::Instance().Publish(
            events::PresentationCompleted{ActivePresentationId()});
    return r;
}

// ---------------------------------------------------------------------------
// Controller commands
// ---------------------------------------------------------------------------
void PresentationRuntime::PublishSlideChanged(size_t prevIndex, size_t newIndex,
                                              const Slide& slide) {
    (void)EventBus::Instance().Publish(events::PresentationTransitionStarted{
        ActivePresentationId(), static_cast<int>(prevIndex), static_cast<int>(newIndex),
        ToString(slide.transitionIn)});
    (void)EventBus::Instance().Publish(events::SlideChanged{
        static_cast<int>(newIndex), static_cast<int>(nav_.Count()), slide.title});
    (void)EventBus::Instance().Publish(events::PresentationTransitionCompleted{
        ActivePresentationId(), static_cast<int>(newIndex)});
}

Result<void> PresentationRuntime::Next() {
    const size_t prev = CurrentIndex();
    auto r = nav_.Next();
    if (!r.ok()) return r;
    if (const Slide* s = CurrentSlide()) PublishSlideChanged(prev, CurrentIndex(), *s);
    return Ok();
}

Result<void> PresentationRuntime::Previous() {
    const size_t prev = CurrentIndex();
    auto r = nav_.Previous();
    if (!r.ok()) return r;
    if (const Slide* s = CurrentSlide()) PublishSlideChanged(prev, CurrentIndex(), *s);
    return Ok();
}

Result<void> PresentationRuntime::First() {
    const size_t prev = CurrentIndex();
    auto r = nav_.First();
    if (!r.ok()) return r;
    if (const Slide* s = CurrentSlide()) PublishSlideChanged(prev, CurrentIndex(), *s);
    return Ok();
}

Result<void> PresentationRuntime::Last() {
    const size_t prev = CurrentIndex();
    auto r = nav_.Last();
    if (!r.ok()) return r;
    if (const Slide* s = CurrentSlide()) PublishSlideChanged(prev, CurrentIndex(), *s);
    return Ok();
}

Result<void> PresentationRuntime::JumpTo(size_t index) {
    const size_t prev = CurrentIndex();
    auto r = nav_.JumpTo(index);
    if (!r.ok()) return r;
    if (const Slide* s = CurrentSlide()) PublishSlideChanged(prev, CurrentIndex(), *s);
    return Ok();
}

Result<void> PresentationRuntime::JumpById(std::string_view slideId) {
    const size_t prev = CurrentIndex();
    auto r = nav_.JumpById(slideId);
    if (!r.ok()) return r;
    if (const Slide* s = CurrentSlide()) PublishSlideChanged(prev, CurrentIndex(), *s);
    return Ok();
}

Result<size_t> PresentationRuntime::Search(std::string_view query) const {
    return nav_.Search(query);
}

Result<void> PresentationRuntime::Back() {
    const size_t prev = CurrentIndex();
    auto r = nav_.Back();
    if (!r.ok()) return r;
    if (const Slide* s = CurrentSlide()) PublishSlideChanged(prev, CurrentIndex(), *s);
    return Ok();
}

// ---------------------------------------------------------------------------
// Playback clock
// ---------------------------------------------------------------------------
Result<size_t> PresentationRuntime::Tick(double dt) {
    if (sm_.State() != PresentationState::Live) return size_t{0};
    clockSec_.store(clockSec_.load() + dt);
    CueContext ctx;
    ctx.runtime = this;
    size_t fired = timeline_.Tick(clockSec_.load(), ctx);

    // Auto-advance for Timed/Automatic slides with a duration.
    const Slide* slide = CurrentSlide();
    if (slide && slide->durationMs > 0.0) {
        autoAdvanceAccum_ += dt;
        if (autoAdvanceAccum_ >= slide->durationMs / 1000.0) {
            autoAdvanceAccum_ = 0.0;
            auto r = nav_.Next();
            if (r.ok()) {
                if (const Slide* s = CurrentSlide())
                    PublishSlideChanged(CurrentIndex() - 1, CurrentIndex(), *s);
            } else if (pres_ && pres_->mode == PlaybackMode::Loop) {
                (void)nav_.First();
                if (const Slide* s = CurrentSlide())
                    PublishSlideChanged(CurrentIndex(), CurrentIndex(), *s);
            }
        }
    }
    return fired;
}

// ---------------------------------------------------------------------------
// Cue hooks
// ---------------------------------------------------------------------------
Result<void> PresentationRuntime::FireMediaCue(std::string_view assetId) {
    // Media playback is the Media Engine's job (Phase 10); here we record the
    // trigger. Returns Ok — never fails the timeline.
    (void)assetId;
    return Ok();
}

Result<void> PresentationRuntime::FireAudioCue(std::string_view assetId) {
    (void)assetId;
    return Ok();
}

// ---------------------------------------------------------------------------
// Recovery
// ---------------------------------------------------------------------------
Result<void> PresentationRuntime::RecoverTo(size_t slideIndex) {
    if (!pres_) return Error::Make(Err::Presentation_RecoveryNotFound,
                                   "PresentationRuntime", "no presentation open");
    auto r = sm_.TransitionTo(PresentationState::Recovering);
    if (!r.ok()) return r;
    if (slideIndex >= nav_.Count()) slideIndex = 0;
    (void)nav_.JumpTo(slideIndex);
    (void)sm_.TransitionTo(PresentationState::Ready);
    (void)EventBus::Instance().Publish(
        events::PresentationRecovered{pres_->id, static_cast<int>(slideIndex)});
    Logger::Instance().Info(std::format("Presentation recovered at slide {}", slideIndex),
                            "PresentationRuntime");
    return Ok();
}

// ---------------------------------------------------------------------------
// State accessors
// ---------------------------------------------------------------------------
PresentationState PresentationRuntime::State() const {
    return sm_.State();
}

size_t PresentationRuntime::CurrentIndex() const {
    return nav_.Current();
}

const Slide* PresentationRuntime::CurrentSlide() const {
    return nav_.CurrentSlide();
}

// ---------------------------------------------------------------------------
// Events
// ---------------------------------------------------------------------------
void PresentationRuntime::WireEvents() {
    if (eventsWired_) return;
    eventsWired_ = true;
    subscriptions_.push_back(EventBus::Instance().Subscribe<events::ConfigHotReload>(
        [this](const events::ConfigHotReload& e) { OnConfigReload(e); }, 0));
}

void PresentationRuntime::UnwireEvents() {
    if (!eventsWired_) return;
    eventsWired_ = false;
    for (auto& s : subscriptions_)
        if (s.Valid()) (void)EventBus::Instance().Unsubscribe(s);
    subscriptions_.clear();
}

void PresentationRuntime::OnConfigReload(const events::ConfigHotReload&) {
    // Nothing to reconfigure at runtime level today (themes are Phase 11).
}

} // namespace bps::presentation
