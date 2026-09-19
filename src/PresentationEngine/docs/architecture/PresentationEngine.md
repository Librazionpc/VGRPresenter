# Presentation Engine (Phase 8) — Architecture

> Spec: docs/specs/19-presentation-engine.md

## Role

The Presentation Engine orchestrates live presentations: shows, slides, timelines,
transitions, cues, playback, navigation and recovery. It never renders pixels
(Phase 6 does), never routes outputs (Phase 7 does) — it is the conductor.

```text
PresentationManager (facade)
   ├── PresentationRegistry        create/open/save/duplicate/delete/index
   ├── PresentationValidator       warnings, not crashes
   ├── PresentationCompiler        → compiled runtime object (scenes, caches)
   ├── PresentationRuntime         current state (slide/scene/outputs/media/timeline/cues)
   ├── PresentationController      Next/Prev/Jump/GoLive/Black/Logo/Pause/Resume/Stop
   ├── PresentationStateMachine    validated transitions
   ├── PresentationNavigator       id/tag/section/search jumps
   ├── PresentationTimeline        sequential/parallel/delayed/loops
   ├── PresentationQueue           queued presentations
   ├── PresentationHistory         slide + navigation history
   ├── PresentationSession         active presentation + runtime + outputs
   ├── PresentationRecovery        crash restore
   ├── PresentationPreloader       next slides/media/fonts
   ├── PresentationCache           render-ready scenes, layouts, transitions
   ├── PresentationProfileManager  playback profiles
   └── SceneBuilder                presentation data → rendering::Scene
```

## Design rules

- **Orchestration only**: no rendering, no display routing, no UI.
- **Interface-driven**: `IPresentationCue`, `IPresentationTransition`,
  `IPresentationTimeline`, `IPresentationCompiler`, `IPresentationValidator`,
  `IPresentationSerializer`, `IPresentationAction` — plugins extend without edits.
- **State-driven**: every transition validated by the state machine.
- **Compiled before live**: runtime executes compiled data, not raw files.
- **Preloaded**: next slides/media are ready before they become live.
- **Stable IDs**: slides are referenced by UUID, never array indexes.
- **Adaptive-aware**: cache size / preload depth come from the Adaptive Runtime.

## File map

| File | Purpose |
|---|---|
| `PresentationTypes.hpp` | `Slide`, `Section`, `Group`, `Presentation`, `PlaybackMode`, `CueType`, state enums |
| `PresentationStateMachine.hpp/.cpp` | validated state transitions |
| `PresentationTimeline.hpp/.cpp` | timeline items (sequential/parallel/delayed/loop) |
| `PresentationCues.hpp/.cpp` | `IPresentationCue` + built-in cue types |
| `PresentationNavigator.hpp/.cpp` | navigation + history |
| `PresentationValidator.hpp/.cpp` | validation warnings |
| `PresentationCompiler.hpp/.cpp` | compile → runtime object + cache |
| `PresentationSerializer.hpp/.cpp` | JSON save/load |
| `PresentationSession.hpp/.cpp` | session + recovery |
| `PresentationEngine.hpp/.cpp` | facade: manager + runtime + controller wiring |

## Integration

- **Kernel**: boots at step 19 (after DisplayEngine 18), shuts down first in the
  feature group.
- **Render Engine**: SceneBuilder produces `rendering::Scene` objects; transitions
  evaluated by the Phase 6 TransitionEngine.
- **Display Engine**: outputs stay assigned via the DisplayEngine.
- **EventBus**: publishes `presentation.started/paused/stopped/completed/recovered`,
  `presentation.slide_changed` (existing `SlideChanged`), `transition.started/
  completed`, `cue.triggered`, `presentation.black_screen/logo_shown`.
- **Notification**: publishes notification-worthy events; the Notification Service
  decides display.
- **Adaptive Runtime**: cache/preload recommendations.
