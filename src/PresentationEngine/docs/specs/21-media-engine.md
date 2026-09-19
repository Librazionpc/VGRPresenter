# 21. Media Engine (Phase 10)

## Objective

Build the single system responsible for discovering, importing, organizing,
processing, caching, decoding, playing, streaming, and managing **every type of
media** used by the application. No other subsystem manages media directly. The
Media Engine does not know what a Song, Bible, Presentation, Slide, or Theme is —
it only understands **Media Assets**.

## Supported types

Images (PNG, JPEG, WEBP, GIF, BMP, TIFF; AVIF/HEIF future), Videos (MP4, MOV,
MKV, AVI, WEBM, MPEG), Audio (MP3, WAV, FLAC, OGG, AAC, M4A), Animated images,
SVG/vector graphics, Icons, Fonts, Backgrounds. New formats/codecs are added
through providers — never engine modifications.

## Media pipeline

```text
Import → Validate → Extract Metadata → Generate Thumbnail → Generate Preview
      → Index (Phase 9) → Cache → Ready
```

No shortcuts — every asset passes through the same pipeline. All processing runs
in the background; the live presentation never freezes because media is being
processed.

## Metadata extraction

Every media file exposes: name, size, type, resolution, duration, codec,
bitrate, frame rate, audio channels, orientation, color space, created/modified
dates, tags, author, copyright. The metadata model is extensible (a map of
key→value plus typed fields).

## Thumbnails & previews

Image thumbnails, video thumbnails, audio artwork, waveforms, SVG previews.
Generated in the background, cached, regenerated only when required. Preview
generation never blocks the UI.

## Playback

Play/pause/seek/loop/playback-speed/frame-step for images, videos, audio, and
animated backgrounds. The playback engine is independent of presentations.
Hardware acceleration (GPU decode/encode/scale) is used when available and falls
back to software automatically. Recovery: a failed asset retries, falls back,
reports, and never crashes the presentation.

## Adaptive Runtime integration

Cache sizes, decode threads, buffer sizes, and hardware acceleration come from
the Adaptive Runtime — never hardcoded.

## Caching

Image/video/audio/thumbnail/metadata/preview caches with automatic release of
unused assets. Cache budgets from the Adaptive Runtime.

## Search integration

Every imported asset becomes searchable — by metadata, embedded text, tags, and
captions (not just filenames) via the Phase 9 Search Engine.

## EventBus

Consumes: `content.asset_loaded`, `content.asset_deleted`, `ConfigHotReload`,
`engine.resource.pressure_changed`.
Publishes: `media.imported`, `media.ready`, `media.removed`, `media.updated`,
`media.thumbnail_generated`, `media.playback_started`, `media.playback_stopped`,
`media.decode_failed`.

## Notification integration

Unsupported formats, corrupt media, missing codecs, import completed,
thumbnail generated — the Notification Service controls visibility.

## Plugin support

Plugins add new formats, codecs, thumbnail generators, preview generators,
metadata extractors, playback engines, hardware decoders, and streaming
protocols without modifying the Media Engine.

## Threading

Independent workers for image decoding, video decoding, audio decoding,
metadata extraction, thumbnail generation, preview generation, and cache
maintenance. No single task blocks another.

## Code quality

No WinUI, no presentation/Bible/song/rendering logic. Only media management and
playback.

## Definition of Done

- [x] Media Asset model with extensible metadata.
- [x] Media type/format registry (image/video/audio/animated/svg/font/background).
- [x] Metadata extraction (size, resolution, duration, codec, dates, tags).
- [x] Thumbnail generation (image/svg/audio-artwork/waveform) in the background.
- [x] Preview generation without blocking.
- [x] Playback engine: play/pause/seek/loop/speed/step, hardware→software fallback.
- [x] Multi-level caching with pressure-aware release.
- [x] Background processing pipeline (import→validate→extract→thumbnail→preview→cache).
- [x] Provider registry (IMediaProvider) for formats/codecs/decoders.
- [x] Search integration (auto-index via Phase 9).
- [x] EventBus + Notification integration.
- [x] Failure recovery (corrupt/missing codec never crashes playback).
- [x] Tests: import, metadata, thumbnails, playback states, caching, recovery,
      large libraries, concurrent processing.
