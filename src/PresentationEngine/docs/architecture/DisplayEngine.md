# Display & Output Engine (Phase 7) — Architecture

> Spec: docs/specs/18-display-engine.md

## Role

The Display Engine is the **output layer**: it routes one rendered frame to any
number of outputs (monitors are just one output type). It sits above the Render
Engine (Phase 6) and owns display detection, assignment, profiles, layout, scaling,
synchronization and recovery — with zero UI code.

```text
RenderEngine (Phase 6)  →  one frame
        │
        ▼
DisplayEngine
   ├── OutputRouter          routes the frame to every enabled output
   ├── DisplayManager        IDisplayProvider registry (Linux/Virtual/Null/…)
   ├── ProfileManager        display profiles + output profiles (JSON)
   ├── LayoutManager         visual layout of outputs (positions)
   ├── ScalingEngine         Native/Fit/Fill/Stretch/Letterbox/Crop/PixelPerfect
   ├── SyncEngine            same frame counter delivered to all outputs
   └── RecoveryEngine        lost display → remember → auto-restore
        │
        ▼
PAL IMonitor (hot-plug)      + EventBus (monitor_* consumed, display.*/output.* published)
```

## Design rules

- **Provider-based**: `DisplayManager` only knows `IDisplayProvider`. Adding a
  monitor type (OBS/NDI/LED wall) is registering a provider.
- **Outputs ≠ monitors**: an `Output` is a logical destination with an id, kind,
  transform and binding; a monitor is only where the pixels go.
- **One frame**: the router distributes the *same* frame — adding an output never
  adds a render.
- **No UI**: no WinUI, no HWND, no XAML, no dialogs — display logic only.
- **Adaptive-aware**: buffer count and cache budgets come from the Adaptive Runtime.

## File map

| File | Purpose |
|---|---|
| `DisplayTypes.hpp` | `DisplayDevice`, `Output`, `OutputKind`, `ScalingMode`, `ColorProfile`, `LayoutItem`, profiles |
| `IDisplayProvider.hpp` | provider interface + capabilities |
| `DisplayManager.hpp/.cpp` | provider registry + hot-plug probe |
| `OutputRouter.hpp/.cpp` | frame distribution + scaling transforms |
| `DisplayProfiles.hpp/.cpp` | display/output profile save/load (JSON) |
| `DisplayLayout.hpp/.cpp` | layout items + normalization |
| `DisplayEngine.hpp/.cpp` | facade: Initialize/Start/Stop/Reload/Reset, assignments, recovery, events |

## Integration

- **Kernel**: boots at step 18 (after RenderEngine 17), shuts down first in the
  display group.
- **Render Engine**: consumes `RenderEngine` frames via the router.
- **EventBus**: consumes `platform.monitor_connected/disconnected`,
  `engine.config.hot_reload`, `engine.resource.pressure_changed`; publishes
  `display.ready`, `display.failed`, `display.changed`, `display.restored`,
  `output.started`, `output.stopped`.
- **Adaptive Runtime**: `GetOutputBufferCount()` etc.
- **PAL**: `IMonitor` for physical device enumeration + hot-plug.
