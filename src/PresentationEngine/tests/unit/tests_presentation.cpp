// Unit tests: Presentation Engine (docs/specs/19).
// Split from tests/unit/main.cpp so a single phase can run alone:
//   ./bps_unit_tests presentation
#include "TestHarness.hpp"

void TestPresentationStateMachine() {
    p::PresentationStateMachine sm;
    CHECK(sm.State() == p::PresentationState::Created);
    CHECK(sm.TransitionTo(p::PresentationState::Loaded).ok());
    CHECK(sm.TransitionTo(p::PresentationState::Validated).ok());
    CHECK(sm.TransitionTo(p::PresentationState::Compiled).ok());
    CHECK(sm.TransitionTo(p::PresentationState::Prepared).ok());
    CHECK(sm.TransitionTo(p::PresentationState::Ready).ok());
    CHECK(sm.TransitionTo(p::PresentationState::Live).ok());
    CHECK(sm.TransitionTo(p::PresentationState::Paused).ok());
    CHECK(sm.TransitionTo(p::PresentationState::Live).ok());   // resume
    CHECK(sm.TransitionTo(p::PresentationState::Finished).ok());
    CHECK(!sm.TransitionTo(p::PresentationState::Live).ok());  // Finished -> Live invalid
    // Direct Created -> Live must be rejected.
    p::PresentationStateMachine sm2;
    CHECK(!sm2.TransitionTo(p::PresentationState::Live).ok());
    // Closed reachable from anywhere.
    CHECK(sm.TransitionTo(p::PresentationState::Closed).ok());
}
void TestPresentationNavigator() {
    p::Presentation pres;
    pres.id = "pres-1";
    pres.name = "Sunday";
    p::Slide s1;
    s1.id = "s1";
    s1.title = "Intro";
    s1.tags = {"opening"};
    s1.sections = {"A"};
    p::Slide s2;
    s2.id = "s2";
    s2.title = "Amazing Grace";
    s2.text = "Amazing grace how sweet the sound";
    s2.tags = {"hymn", "grace"};
    p::Slide hidden;
    hidden.id = "hidden";
    hidden.title = "Hidden";
    hidden.hidden = true;
    pres.slides = {s1, hidden, s2};
    p::PresentationNavigator nav;
    nav.Open(pres);
    CHECK(nav.Count() == 2);   // hidden skipped
    CHECK(nav.Current() == 0);
    CHECK(nav.Next().ok());
    CHECK(nav.CurrentSlide() && nav.CurrentSlide()->id == "s2");
    CHECK(!nav.Next().ok());   // already last
    CHECK(nav.Previous().ok());
    CHECK(nav.CurrentSlide()->id == "s1");
    CHECK(nav.JumpById("s2").ok());
    CHECK(nav.JumpByTag("hymn").ok());
    CHECK(nav.JumpBySection("A").ok());
    auto found = nav.Search("grace");
    CHECK(found.ok() && found.value() == 2);
    CHECK(nav.Back().ok());
    CHECK(nav.HistoryDepth() > 0);
}
void TestPresentationTimeline() {
    p::PresentationTimeline timeline;
    int fired = 0;
    auto cue = std::make_shared<p::ScriptCue>(
        "cue1", 1.5,
        [&](p::CueContext&) { ++fired; return Ok(); });
    CHECK(timeline.AddCue(cue).ok());
    CHECK(!timeline.AddCue(cue).ok());   // duplicate id rejected
    CHECK(timeline.Size() == 1);
    p::CueContext ctx;
    CHECK(timeline.Tick(1.0, ctx) == 0);       // not due yet
    CHECK(timeline.Tick(2.0, ctx) == 1);       // fires once
    CHECK(timeline.Tick(3.0, ctx) == 0);       // never fires again
    CHECK(fired == 1);
    timeline.Reset();
    CHECK(timeline.Tick(2.0, ctx) == 1);       // rewind
    CHECK(fired == 2);
}
void TestPresentationEngine() {
    auto& eng = p::PresentationEngine::Instance();
    CHECK(eng.Initialize().ok());
    CHECK(eng.Start().ok());
    auto created = eng.CreatePresentation("Sunday Service");
    CHECK(created.ok() && !created.value().empty());
    const std::string id = created.value();
    p::Slide s1;
    s1.id = "slide-1";
    s1.title = "Welcome";
    s1.text = "Good morning!";
    CHECK(eng.AddSlide(id, s1).ok());
    p::Slide s2;
    s2.id = "slide-2";
    s2.title = "Amazing Grace";
    s2.text = "Amazing grace how sweet the sound";
    s2.durationMs = 3000;
    CHECK(eng.AddSlide(id, s2).ok());
    CHECK(eng.PresentationCount() == 1);
    // Full pipeline: validate -> compile -> prepare -> go live.
    auto issues = eng.Validate(id);
    CHECK(issues.ok() && !p::PresentationValidator::HasErrors(issues.value()));
    CHECK(eng.Compile(id).ok());
    CHECK(eng.Prepare(id).ok());
    CHECK(eng.State() == p::PresentationState::Ready);
    CHECK(eng.GoLive().ok());
    CHECK(eng.State() == p::PresentationState::Live);
    // Navigation publishes SlideChanged.
    int slideEvents = 0;
    Subscription sub = EventBus::Instance().Subscribe<events::SlideChanged>(
        [&](const events::SlideChanged&) { ++slideEvents; }, 0);
    CHECK(eng.Next().ok());
    CHECK(eng.CurrentIndex() == 1);
    CHECK(eng.CurrentSlide() && eng.CurrentSlide()->id == "slide-2");
    CHECK(slideEvents >= 1);
    (void)EventBus::Instance().Unsubscribe(sub);
    // Timeline cue + playback tick.
    auto cue = std::make_shared<p::SlideCue>("jump", 0.5, "slide-1");
    CHECK(eng.AddCue(cue).ok());
    auto fired = eng.Tick(1.0);
    CHECK(fired.ok() && fired.value() == 1);
    CHECK(eng.CurrentIndex() == 0);   // slide cue jumped back
    // Pause/resume/stop.
    CHECK(eng.Pause().ok());
    CHECK(eng.State() == p::PresentationState::Paused);
    CHECK(eng.Resume().ok());
    CHECK(eng.StopPlayback().ok());
    CHECK(eng.State() == p::PresentationState::Stopped);
    // Session + recovery.
    CHECK(eng.JumpTo(1).ok());
    CHECK(eng.SaveSession().ok());
    auto snap = eng.Recover();
    CHECK(snap.ok() && snap.value().presentationId == id);
    CHECK(snap.ok() && snap.value().slideIndex == 1);
    // Duplicate + delete.
    CHECK(eng.Duplicate(id).ok());
    CHECK(eng.PresentationCount() == 2);
    CHECK(eng.Delete(id).ok());
    CHECK(eng.PresentationCount() == 1);
    CHECK(eng.Stop().ok());
    CHECK(eng.Shutdown().ok());
}

// ===========================================================================
// Phase 9 — Search & Indexing Engine (docs/specs/20)
// ===========================================================================
