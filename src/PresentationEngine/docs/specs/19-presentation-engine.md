# 19. Presentation Engine (Phase 8)

## Objective

Build the engine responsible for **running live presentations**. The Presentation
Engine is not responsible for rendering pixels (Phase 6) or displaying outputs
(Phase 7). It orchestrates the presentation itself — managing shows, slides,
timelines, transitions, cues, playback, and interaction. It is the **conductor of
the orchestra**.

The renderer draws. The Display Engine outputs. The Presentation Engine orchestrates.
Each system performs only its own responsibility; the Presentation Engine never
contains rendering, display-routing, WinUI, XAML, HWND, or dialog code.

## Architecture

```text
Project → PresentationManager → PresentationRuntime → PresentationController
         → SceneBuilder → RenderingEngine → DisplayEngine
```

## Core components

```text
PresentationManager      facade: create/open/save/duplicate/delete/validate/compile/register/close
PresentationRuntime      current slide/scene/outputs/media/timeline/cues/playback state
PresentationController   Next/Previous/JumpTo/GoLive/Black/Logo/Pause/Resume/Stop
PresentationStateMachine Created→Loaded→Validated→Compiled→Prepared→Ready→Live→Paused→Stopped→Finished→Recovering→Closed
PresentationQueue        queued presentations
PresentationTimeline     sequential/parallel/delayed/scheduled/loops
PresentationNavigator    Next/Prev/First/Last/Jump-by-ID/tag/section/search
PresentationHistory      slide + navigation history ("Back" and recovery)
PresentationSession      active presentation, runtime, outputs, position
PresentationRecovery     restore presentation/slide/playback state after crashes
PresentationValidator    missing media/fonts, broken refs, duplicate IDs, warnings
PresentationSerializer   save/load presentations (JSON)
PresentationCompiler     validate assets, resolve refs, pre-build scenes, cache layouts
PresentationCache        render-ready scenes, slide/text layouts, transitions, timelines
PresentationRegistry     presentation index (fast lookup)
PresentationProfileManager  presentation/playback profiles
```

## Interfaces

```cpp
IPresentation, IPresentationController, IPresentationPlayer, IPresentationAction,
IPresentationTransition, IPresentationTimeline, IPresentationCue,
IPresentationCompiler, IPresentationValidator, IPresentationSerializer
```

Everything is interface-driven; plugins extend cue types, playback modes, timeline
processors, validators, compilers, transitions, and presentation formats without
modifying the engine.

## State machine

```text
Created → Loaded → Validated → Compiled → Prepared → Ready → Live → Paused →
Stopped → Finished → Recovering → Closed
```

Every transition is validated; no invalid transitions are possible.

## Slide system

- Unlimited slides; sections; nested groups; hidden slides; notes; tags; metadata.
- Slides have **stable IDs** (UUIDs), never array indexes.
- 10 / 500 / 5,000 slide presentations all work — no redesign.

## Scene Builder

Converts presentation data into renderable scenes. The Presentation Engine never
manipulates GPU objects directly — it builds `rendering::Scene` objects (Phase 6)
and hands them to the Render Engine.

## Timeline system

- Sequential playback, parallel actions, delayed actions, timed events, scheduled
  events, loops. Deterministic and easy to extend.

## Cue system

Cue types (each implements `IPresentationCue`): Slide, Media, Audio, Timer,
Countdown, Script, MIDI, Plugin. Adding a cue type never requires changing existing
code.

## Navigation

Next, Previous, First, Last, Jump by ID, Jump by tag, Jump by section, Search and
jump — all instant.

## Transitions

Fade, Slide, Push, Zoom, Wipe, Crossfade, Custom plugin transitions. Configurable
per-slide or globally. (Evaluated by the Phase 6 TransitionEngine.)

## Playback modes

Manual, Automatic, Timed, Loop, Repeat, Playlist.

## Compiler

Before going live the presentation is compiled into an optimized runtime
representation: validate references, resolve assets, prepare scenes, preload
transitions, build caches. The live runtime executes compiled data, not raw files.

## Validator

Detects missing images/videos/fonts, broken references, duplicate IDs, invalid
layouts — reporting warnings without crashing the engine.

## Preloader

Before a slide becomes live the engine already has: images loaded, fonts ready,
video prepared, layout calculated, scene built. Slide changes feel instantaneous.

## Recovery

```text
Restart → Recover Session → Restore Presentation → Restore Slide →
Restore Playback State → Continue
```

Minimal user intervention.

## EventBus integration

Consumes: PresentationOpened/Closed/Compiled/Validated, DisplayReady, MediaLoaded.

Publishes: PresentationStarted/Paused/Stopped/Completed/Recovered, SlideChanged,
TransitionStarted/Completed, CueTriggered, BlackScreen, LogoShown.

## Notification integration

Publishes events (missing media, validation warnings, compilation success,
recovery complete); the Notification Service (Phase 4) decides how and when to
notify the user.

## Adaptive Runtime integration

The Presentation Engine never decides cache size, memory usage, thread count, or
preload depth — it requests recommendations from the Adaptive Runtime
(`GetCacheBytes`, `GetRenderCacheBytes`, `ShouldPreloadFrequentlyUsed`).

## Performance expectations

- Instant slide switching; preloaded upcoming assets.
- Thousands of slides; smooth transitions.
- Never blocks the rendering thread; recovers gracefully.

## Testing expectations

Presentation creation, validation, compilation, runtime playback, navigation,
timeline execution, cue execution, recovery, large presentations (5,000 slides),
performance, concurrent commands, state-machine correctness.

## Definition of Done

- [ ] Presentations can be created, compiled, validated, and executed.
- [ ] Navigation is reliable and state-driven (validated state machine).
- [ ] Timelines and cues execute correctly; new cue types plug in without edits.
- [ ] Slide changes and transitions are smooth (compiled + preloaded).
- [ ] Recovery restores the live presentation accurately.
- [ ] Plugins can extend the presentation workflow.
- [ ] No rendering or display logic inside the Presentation Engine.
- [ ] EventBus + Notification + AdaptiveRuntime integration complete.
- [ ] Clean build, unit tested, ASan clean.
