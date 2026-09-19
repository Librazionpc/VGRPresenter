# 18. Display & Output Engine (Phase 7)

## Objective

The engine should no longer think in terms of *Monitor 1, Monitor 2, Projector* — it
thinks in terms of **Outputs**. A monitor is just one type of output. Tomorrow an
output could be OBS, NDI, an LED wall, a web browser, a mobile app, a virtual
display, a recording, a screenshot, AI vision, or a remote stage display. The engine
doesn't care.

The Display Engine routes **one rendered frame** to **any number of outputs** through
a provider-based architecture, with display detection (hot-plug), removal recovery,
profiles, layouts, scaling, synchronization, and full EventBus integration. It never
contains UI code, WinUI, HWND, XAML, or dialogs.

## Architecture

```text
RenderEngine
    │  (one frame)
    ▼
DisplayEngine  ── Output Router ──► Outputs (Audience, Stage, Preview, Recording,
    │                                    Screenshot, Stream, Virtual, Remote, …)
    │
    ▼
IDisplayProvider ─► Linux / Windows / Virtual / Null / OBS / NDI / Remote / Web (future)
    │
    ▼
PAL (IMonitor)  ─► physical devices (hot-plug)
```

## Major Components

### 1. Provider-based display model

`DisplayManager` owns a registry of `IDisplayProvider`s — exactly like Importers,
Notifications, and Plugins:

```cpp
class IDisplayProvider {
public:
    virtual const char* Name() const noexcept = 0;
    virtual const char* Version() const noexcept = 0;
    // All physical/virtual displays this provider currently offers.
    virtual std::vector<DisplayDevice> Enumerate() const = 0;
    // Probe the provider (hot-plug refresh). Returns devices that changed.
    virtual Result<std::vector<DisplayDevice>> Probe() = 0;
    virtual DisplayProviderCapabilities Capabilities() const noexcept = 0;
};
```

Every provider advertises:

- Name, version
- Display count, max outputs, supports hot-plug, supports virtual outputs
- Supported scaling modes, resolutions, refresh rates

### 2. DisplayDevice

A display is a *device* with capabilities — never a window:

```cpp
struct DisplayDevice {
    std::string id;            // stable id ("eDP-1", "\\.\DISPLAY1", "virtual-aud")
    std::string name;          // friendly name
    int x = 0, y = 0;          // virtual-desktop origin
    int width = 0, height = 0; // native resolution
    int refreshRateHz = 0;
    int dpi = 96;
    int orientation = 0;       // degrees
    bool primary = false;
    bool connected = true;
    bool hdrSupported = false;
    std::string provider;      // owning provider name
};
```

### 3. Outputs (not monitors)

Each output is a logical destination that consumes frames:

```cpp
enum class OutputKind : int { Audience, Stage, Preview, Thumbnail, Stream, Recording,
                              Screenshot, Virtual, Remote, Custom };

struct Output {
    std::string id;                 // stable id ("audience", "stage-2")
    OutputKind kind;
    std::string displayId;          // bound display device ("") = virtual/headless
    std::string name;
    OutputTransform transform;      // scaling + layout
    bool enabled = true;
    bool autoRestore = true;        // re-attach when the display returns
    OutputState state;              // Off / Starting / Running / Failed / Lost
};
```

The Output Router (`OutputRouter`) is the only path frames travel:

```text
Renderer → one frame → OutputRouter → Outputs
```

One frame is shared; outputs scale/crop it locally — adding a monitor never doubles
rendering cost.

### 4. Scaling

`ScalingMode`: Native, Fit, Fill, Stretch, Letterbox, Crop, PixelPerfect. The router
computes a source→dest rectangle per output transform (fit/letterbox/crop math lives
here, unit-tested).

### 5. Resolutions & refresh rates

- Native (device), 720p, 1080p, 1440p, 4K, 8K — mixed freely.
- Refresh 60/75/120/144 Hz — mixed without problems (each output paces itself).

### 6. Color

- Color profile per output (`ColorProfile`): sRGB / Rec.709 / Gamma / SafeColor /
  Custom with a gamma curve and brightness.
- HDR flagged as supported on capable devices (future path).

### 7. Synchronization

All outputs stay synchronized: a frame is stamped with the engine frame counter and
the router delivers the *same* frame (same counter, same content) to every enabled
output in one distribution pass. Transitions, timers, videos, animations and slides
never drift because there is exactly one frame source.

### 8. Display detection (hot-plug)

The engine periodically (scheduler tick) probes providers and drains the PAL
monitor changes. On connect:

```text
Engine Running → Monitor Connected → Detected → Capabilities Read →
Assign Output → Ready
```

No restart. On disconnect:

```text
Display Lost → Keep Presentation Running → Remember Assignment → Wait →
Reconnect → Automatically Restore
```

No crashes. Resolution/refresh changes are detected and the output transform
recomputed automatically.

### 9. Display profiles & output profiles

```cpp
struct DisplayProfile {
    std::string id, name;
    std::vector<Output> outputs;        // full output assignments
    std::vector<LayoutItem> layout;     // visual layout (positions)
};
```

- *Display profiles*: "Church Main Hall", "Conference Hall" — one click configures
  every display.
- *Output profiles*: "Sunday Morning" (Audience + Stage + Preview), "Youth Service"
  (Audience + Stage + Recording + Stream).
- Profiles serialize to JSON (via the Configuration/Database systems) and restore
  automatically on boot.

### 10. Error recovery

- Output display disappears → output state `Lost`, frame routing continues to other
  outputs, the presentation never stops.
- Display returns → auto-restore the remembered assignment (`autoRestore`).
- Resolution changes → recompute transform, keep running.

### 11. Adaptive Runtime integration

The Display Engine never asks *how much VRAM?* — it asks the Adaptive Runtime for
`GetOutputBufferCount()` / `GetRenderCacheBytes()` and honors the recommended buffer
count for its frame queue.

### 12. EventBus integration

Consumes: `platform.monitor_connected/disconnected`, `engine.config.hot_reload`,
`engine.resource.pressure_changed`.

Publishes:

- `display.ready` — engine fully ready with assigned outputs
- `display.failed` — an output/display failed to come up
- `display.changed` — assignment/layout changed
- `display.restored` — a lost display reconnected and restored
- `output.started` / `output.stopped` — per-output lifecycle

### 13. Threading

- Display thread (hot-plug polling, scheduler tick)
- Output thread (frame distribution)
- Each output may pace itself at its own refresh rate
- All engine singletons are internally locked; the router copies the output list
  under lock and distributes outside it

### 14. Plugin support

Future `HDMIMatrixProvider`, `DeckLinkProvider`, `NDIProvider`, `OBSProvider` drop
into `plugins/`, register, and work — no engine changes.

## Performance expectations

- One frame → N outputs; no per-output rendering.
- Adding an output costs only scaling/compositing, never a second render.
- Hot-plug probe is cheap (device enumeration, not rendering).

## Testing expectations

- Single, dual, triple display setups (virtual providers)
- Hot-plug connect/disconnect
- Display removal + auto-restore
- Profile switching (display + output profiles)
- Mixed resolutions and refresh rates
- Synchronization (same frame counter to all outputs)
- Recovery, stress (many outputs), performance (no extra renders)

## Definition of Done

- [ ] DisplayManager is provider-based (`IDisplayProvider` registry).
- [ ] Outputs are independent of monitors; unlimited outputs.
- [ ] One frame routes to all enabled outputs via the OutputRouter.
- [ ] Hot-plug: connect → detect → assign → ready; disconnect → continue → restore.
- [ ] Display and output profiles save/restore.
- [ ] Scaling modes (Native/Fit/Fill/Stretch/Letterbox/Crop/PixelPerfect) work.
- [ ] Synchronization keeps every output on the same frame.
- [ ] Adaptive Runtime answers buffer-count questions.
- [ ] EventBus integration complete (consume monitor events, publish display/output
      events).
- [ ] No UI code anywhere; clean build, unit tested, ASan clean.

## Acceptance test

Start engine with no displays → engine runs. Connect monitor → detected → assign
Audience. Connect projector → assign Stage. Connect third → assign Preview. Start
presentation → everything displays. Disconnect projector → presentation continues.
Reconnect → auto-restores. Save display profile → restart → everything restores
automatically.
