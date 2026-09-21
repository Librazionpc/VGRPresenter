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

## Show structure: categories, templates, overlays

A show groups its slides into **categories** (songs, notes, pastor notes,
scripture...). Each category has a `contentType`, an assigned **template**, and an
optional output filter (e.g. pastor notes on `stage` only). Every slide in a
category is drawn through that category's template, so restyling the template
restyles them all — content never owns styling (docs/specs/23 §Themes). Slide
ORDER stays one flat sequence, so a show can interleave song / notes / song;
categories are a grouping and styling axis, not containers.

- `SlideTemplate`: content blocks whose `bind` names the slide field they show
  (`title`, `text`, `notes`, or meta fields such as `line1`/`ref`).
- `SlideResolver::Resolve(show, slideId, outputId)`: template blocks first (bound
  to the slide), a slide block with the same id replaces its template block,
  other slide blocks are extras; the slide's own background wins unless unset;
  overlays in scope are collected above.
- `Overlay`: a layer over slides with scope All / Category / Slide and an output
  filter. Lives on the show, so one overlay covers a whole scope.
- Templates live inside the show (so it opens anywhere) and as standalone `.vgr`
  Template documents. Dangling references (category -> missing template, ...) are
  reported as validator warnings and render unstyled — never a crash.
- **CRUD is the engine's job (`ShowEditor`).** A frontend never hand-edits these
  lists: it asks (`AddCategory`, `AssignTemplate`, `RemoveTemplate`, `AddOverlay`,
  `DuplicateSlide`, `AddBlock`, ...) and the engine validates, generates ids, applies
  the cascades and refuses anything that would leave the show inconsistent. Rules:
  removing a category uncategorises its slides and removes its scoped overlays;
  removing a slide removes its scoped overlays; a template still assigned to a
  category is refused unless forced (then those categories lose it); duplicates get
  fresh ids. Every operation is atomic — a refused edit changes nothing.
- Not to be confused with **library** categories (`ShowLibrary`): those organise
  whole shows in folders.

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
| `PresentationTypes.hpp` | `Slide`, `ContentBlock`, `Category`, `SlideTemplate`, `Overlay`, `Presentation`, `PlaybackMode`, `CueType`, state enums |
| `PresentationTemplates.hpp/.cpp` | `SlideResolver` (category -> template -> bound blocks, per-slide overrides, overlays, per-output visibility) and `TemplateFile` (a template as a `.vgr` Template document) |
| `ShowEditor.hpp/.cpp` | the engine owns every create/update/delete/duplicate/move on a show: categories, templates, overlays, slides and content blocks — validated, id-generating, cascading, atomic (`PresentationDocument::Edit` commits only on success) |
| `ShowLibrary.hpp/.cpp` | folder-backed show library: library categories are sub-folders, shows are `.vgr` files; index, search, move/rename, damaged-file reporting |
| `PresentationStateMachine.hpp/.cpp` | validated state transitions |
| `PresentationTimeline.hpp/.cpp` | timeline items (sequential/parallel/delayed/loop) |
| `PresentationCues.hpp/.cpp` | `IPresentationCue` + built-in cue types |
| `PresentationNavigator.hpp/.cpp` | navigation + history |
| `PresentationValidator.hpp/.cpp` | validation warnings |
| `PresentationCompiler.hpp/.cpp` | compile → runtime object + cache |
| `PresentationSerializer.hpp/.cpp` | `IPresentationSerializer` + JSON schema v1 (slides, content blocks, per-kind meta) |
| `PresentationDocument.hpp/.cpp` | `IDocumentHandler` "presentation": open/save a show as a native `.vgr` (verified, crash-safe write; registered with the DocumentManager at Initialize) |
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
