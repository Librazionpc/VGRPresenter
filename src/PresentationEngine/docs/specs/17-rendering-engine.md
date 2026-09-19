# 17. Rendering Engine (Phase 6)

Status: **implemented** · Module: `modules/rendering/` · Namespace: `bps::rendering`

## Purpose

Convert **engine objects into pixels**. The Rendering Engine knows nothing about
presentations, songs, Bibles, or any frontend (WinUI/Qt/GTK). It only understands:

    Scene → Layers → Objects → Pixels

Anything can be rendered: church presentations, airport flight displays, digital
menus, dashboards, signage. The engine is generic by construction — the same
scene API renders any content on any backend.

## Responsibilities

- Own scenes (create / destroy / update / render / serialize).
- Run an ordered render pipeline; nothing bypasses it.
- Expose graphics backends through `IGraphicsBackend` only.
- Manage GPU resources (textures, buffers, shaders, meshes, pipeline states).
- Provide text, image, video, shape, animation, transition and effect systems.
- Produce frames for **Render Outputs** (audience, stage, preview, thumbnail,
  stream, screenshot) without knowing where they go.
- Report diagnostics (FPS, frame time, draw calls, triangles, batches, texture
  count, frame drops, GPU memory).

## Non-Responsibilities

- No frontend code (no HWND, XAML, WinUI, Qt).
- No business logic; no Bible/Song/Presentation knowledge.
- No direct graphics API usage outside `IGraphicsBackend` implementations.

## Architecture

```
Scene Manager ─► Scene ─► Layers ─► Objects
        │
        ▼
Render Pipeline (ordered passes)
        │
        ▼
Renderer (begin/end frame, submit, present)
        │
        ▼
IGraphicsBackend  ◄── Null / Software / (DirectX12 · Vulkan · Metal)
        │
        ▼
Render Outputs (Audience · Stage · Preview · Thumbnail · Stream · Screenshot)
```

## Components

| Component                                 | Responsibility                                                                 | Status |
| ----------------------------------------- | ------------------------------------------------------------------------------ | ------ |
| `RenderEngine`                          | Facade (IService): scenes, frame loop, diagnostics, events                     | ✅     |
| `Scene` / `SceneNode` / `Component` | Scene graph hierarchy (node tree, transforms)                                  | ✅     |
| `Layer`                                 | Independent render layers (background, video, image, text, overlay, debug)     | ✅     |
| `RenderObject`                          | Base class; Text/Image/Video/Shape/Background/Gradient/Overlay/Countdown/Clock | ✅     |
| `Camera`                                | Orthographic zoom/pan/viewport/safe areas                                      | ✅     |
| `TextEngine` + `FontManager`          | Unicode text, alignment, wrapping, spacing, shadow/stroke/glow, glyph atlas    | ✅     |
| `AnimationEngine`                       | Keyframe tracks (position/scale/rotation/opacity/color/custom), easing         | ✅     |
| `TransitionEngine`                      | Fade/Slide/Push/Zoom/Reveal/Wipe/CrossFade                                     | ✅     |
| `EffectStack`                           | Blur/Glow/Shadow/Opacity/Crop/Mask/Brightness/Contrast/Saturation              | ✅     |
| `RenderPipeline` + `RenderPass`       | Ordered passes; nothing bypasses the pipeline                                  | ✅     |
| `RenderGraph`                           | Extensible pass nodes + dependencies (topological order)                       | ✅     |
| `GPUResourceManager`                    | Texture/buffer/shader/mesh registry, reuse, eviction, memory accounting        | ✅     |
| `RenderCache`                           | Glyph/layout/geometry/shader/pipeline-state caches                             | ✅     |
| `ResourceUploader`                      | Async texture/font uploads without blocking rendering                          | ✅     |
| `FrameGraph`                            | Per-frame resource lifecycle + dependency tracking                             | ✅     |
| `RenderOutputs`                         | Audience/Stage/Preview/Thumbnail/Stream/Screenshot frame consumers             | ✅     |
| `Diagnostics`                           | FPS, frame time, GPU memory, draw calls, triangles, batches, drops             | ✅     |

## Render Pipeline (fixed order)

    Frame Begin → Scene Update → Animation Update → Layout Update →
    Resource Upload → Background Pass → Video Pass → Image Pass → Text Pass →
    Overlay Pass → Effects Pass → Debug Pass → GPU Submit → Frame Present

## Backends

`IGraphicsBackend` is the only graphics seam:

- `NullGraphicsBackend` — headless, command accounting (CI/server, thumbnails
  where pixels are not needed).
- `SoftwareGraphicsBackend` — CPU rasterizer producing RGBA frames; used by the
  acceptance tests and on machines without a GPU. Proves the engine produces
  correct pixels end-to-end.
- DirectX12 / Vulkan / Metal / OpenGL — future implementations behind the same
  interface; swapping backends never touches the renderer.

## Render Outputs

The Render Engine only generates frames. Each `IRenderOutput` decides where
frames go. Multiple outputs (audience + stage + preview + thumbnail + stream)
consume the same rendered scene without duplicating rendering logic. This is the
seam the Display Engine (Phase 7) will route to multiple monitors/NDI/OBS.

## EventBus Integration

**Consumes:** `ContentAssetLoaded` (texture upload), `ContentAssetChanged`,
`ContentAssetDeleted` (evict), `DisplayChanged`, `ConfigHotReload`,
`engine.resource.pressure_changed` (evict + quality scale).

**Publishes:** `render.frame_rendered`, `render.texture_loaded`,
`render.shader_compiled`, `render.gpu_out_of_memory`, `render.frame_dropped`,
`render.error`.

## Resource Manager Integration

On memory pressure: evict unused textures/meshes/shaders, shrink render cache,
reduce dynamic quality (thumbnail resolution via the Adaptive Runtime).

## Adaptive Runtime Integration

The engine never decides quality itself. It asks `AdaptiveRuntime`:
`GetTextureBudget()`, `GetRenderCacheBytes()`, `GetRecommendedThreadCount()`,
`GetThumbnailResolutionPct()`, `ShouldUseGpu(task)`, `BackgroundWorkAllowed()`.

## Performance Goals

- 60 FPS on typical presentation hardware; 120+ on capable systems.
- Batch draw calls; minimize state changes; cache glyphs/layouts; reuse GPU
  resources; async texture loading; no frame hitches during media loads.

## Definition of Done

1. A scene renders from engine data (software backend produces correct pixels).
2. Rendering is independent of the frontend.
3. Multiple backends supported through `IGraphicsBackend`.
4. Text, images, videos, shapes render correctly.
5. Layers, animations, transitions, effects work through the pipeline.
6. GPU resources managed without leaks; caches evict under pressure.
7. Rendering is asynchronous where appropriate, maintains smooth frame rates.
8. Integrates with Core, PAL, CAMS, Project System, Resource Manager, EventBus.
9. Architecture is extensible via interfaces and plugins (custom passes,
   effects, shaders, transitions, renderables).
10. The engine can render the same scene to multiple outputs simultaneously.
