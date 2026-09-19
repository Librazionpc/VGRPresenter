# Rendering Engine (Phase 6) — Architecture

> The engine renders **scenes**, never presentations. The renderer knows nothing
> about Bible/Song/Presentation. Any content — church slides, airport flight
> displays, digital menus, dashboards — renders with the same API.

## Layer diagram

```
                Scene Manager (RenderEngine facade)
                        │
        ┌───────────────┼────────────────┐
        ▼               ▼                ▼
     Scenes      Render Pipeline    Render Outputs
        │               │                │
        ▼               ▼                ▼
   SceneGraph      Passes (ordered)   Audience · Stage
   Layers          RenderGraph        Preview · Thumbnail
   Objects                             Stream · Screenshot
   Camera
        │
        ▼
  Renderer (begin/end frame, submit, present)
        │
        ▼
  IGraphicsBackend ──── Null · Software · (DX12 · Vulkan · Metal)
```

## Design rules

1. **Backend independence** — the renderer only talks to `IGraphicsBackend`.
   DirectX12/Vulkan/Metal are drop-in implementations; swapping never touches
   scene, pipeline, or object code.
2. **Pipeline authority** — nothing renders outside the ordered pipeline
   (Background → Video → Image → Text → Overlay → Effects → Debug). The
   `RenderGraph` allows extending with custom passes without modifying the
   renderer (Open/Closed).
3. **Scenes, not documents** — a presentation *becomes* a scene. Modules build
   scenes from their data; the renderer knows only scenes.
4. **Outputs consume frames** — the Render Engine generates frames; each
   `IRenderOutput` decides where they go. One scene → many outputs with zero
   duplicated rendering logic. This is the seam Phase 7 (Display Engine) uses
   for multi-monitor / NDI / OBS.
5. **Adaptive quality** — the engine never decides quality itself; it asks the
   Adaptive Runtime (`GetTextureBudget`, `GetThumbnailResolutionPct`,
   `ShouldUseGpu`, `BackgroundWorkAllowed`).
6. **No UI, no business logic** — zero WinUI/Qt/GTK includes, zero HWND/XAML,
   zero Bible/Song/Presentation logic inside `modules/rendering/`.

## File map

| File                          | Contents                                                                         |
| ----------------------------- | -------------------------------------------------------------------------------- |
| `RenderTypes.hpp`           | Color, Rect, Vec2, Transform, BlendMode, RenderStats                             |
| `IGraphicsBackend.hpp`      | The graphics seam (textures, commands, present)                                  |
| `GraphicsBackends.hpp/.cpp` | NullGraphicsBackend + SoftwareGraphicsBackend                                    |
| `Scene.hpp/.cpp`            | Scene, SceneNode (SceneGraph), Component, Layer, Camera                          |
| `RenderObject.hpp/.cpp`     | Object base + Text/Image/Video/Shape/Background/Gradient/Overlay/Countdown/Clock |
| `TextEngine.hpp/.cpp`       | FontManager, TextStyle, TextLayout, glyph atlas                                  |
| `Animation.hpp/.cpp`        | Keyframes, tracks, easing, Animator                                              |
| `Transitions.hpp/.cpp`      | TransitionType + TransitionEngine (fade/slide/push/zoom/reveal/wipe/crossfade)   |
| `Effects.hpp/.cpp`          | EffectType + EffectStack (pixel + region effects)                                |
| `Pipeline.hpp/.cpp`         | RenderPass, RenderPipeline, RenderGraph (extensible nodes)                       |
| `GpuResources.hpp/.cpp`     | GPUResourceManager, RenderCache, ResourceUploader, FrameGraph                    |
| `RenderOutputs.hpp/.cpp`    | IRenderOutput + concrete outputs                                                 |
| `RenderEngine.hpp/.cpp`     | Facade: scenes, frame loop, diagnostics, events                                  |

## Software backend

`SoftwareGraphicsBackend` is a complete CPU rasterizer (RGBA framebuffer):

- Clear / fill-rect / blit-texture / draw-glyph with alpha blending.
- Shape rasterization (rect, circle, polygon via scanline fill).
- Pixel effects: brightness/contrast/saturation/opacity, box blur, glow,
  shadow, crop/mask regions.
- Frame capture to RGBA buffers for outputs (audience/preview/thumbnail/
  screenshot) — the acceptance tests verify actual pixel output.

## Event flow

- `ContentAssetLoaded` → decode + upload texture (async via ResourceUploader).
- `ContentAssetDeleted` → evict from GPU resources + render cache.
- `engine.resource.pressure_changed` → evict unused resources, shrink caches.
- `ConfigHotReload` → re-read quality/backend config.
- `render.frame_rendered` published each present; `render.frame_dropped` when a
  frame exceeds the budget; `render.gpu_out_of_memory` on allocation failure.

## Kernel integration

Boots at step **17** (after AdaptiveRuntime — it consults it), shuts down first
among the Phase 3–5 module managers. Registered into the ServiceManager.
