# 13 — Content & Asset Management System (CAMS)

Phase 3 of the engine. CAMS is the centralized system responsible for
discovering, importing, organizing, storing, indexing, caching, loading,
exporting, and monitoring every piece of content used by the engine.

**Purpose**: After this phase, no module accesses files directly. Everything
goes through CAMS.

**Status**: Implemented — `modules/content/` (namespace `bps::content`).
See `docs/architecture/CAMS.md` for the architecture; `docs/architecture/Conformance.md`
§16 (Phase 3 DoD) for the acceptance checklist.

## Purpose

- Make every piece of content an *asset* with a UUID and metadata.
- Provide a single public API (`ContentManager`) for find/load/save/import/
  export/delete/move/rename/duplicate/search.
- Decouple file access behind a Virtual File System (Disk / ZIP / Memory).
- Keep extensible import/export behind interfaces (`IImporter` / `IExporter`)
  so new formats never require modifying CAMS (Open/Closed principle).
- Integrate with the Core: Logger, ConfigurationManager, EventBus,
  ResourceManager (memory pressure), DatabaseManager (metadata persistence),
  and the PAL (all file I/O through `platform::IFilesystem`).

## Responsibilities

- Own the content library: metadata, runtime assets, caches, indexes, watchers.
- Publish lifecycle events (`content.*`) and consume Core events
  (`Shutdown`, `ConfigHotReload`, `ResourcePressureHigh`).
- Never bypass the Core or the PAL; never touch the OS directly.

## Public API (ContentManager facade)

- `ImportFile(hostPath, ImportOptions)` → imports a host file into the VFS
  library through the registered importer for its extension.
- `CreateText(asset name/type/text)` → creates an in-library text asset.
- `Find`, `GetMetadata`, `List` — metadata lookups.
- `Load(uuid)`, `LoadAsync(uuid, progress, cancel)` — cached loads through the
  AssetLoader; `Unload(uuid)`, `Pin(uuid, pinned)`.
- `Save(uuid, bytes)`, `Delete(uuid)`, `Move(uuid, newPath)`,
  `Rename(uuid, newName)`, `Duplicate(uuid)`.
- `Export(uuid, format, destPath)` / `ExportPackage(uuids, destPath)`.
- `Search(query)` — AssetIndexer-backed search.
- `RegisterImporter` / `RegisterExporter` — extension points.
- `Mount` (VFS roots), `Watch(relPath, onChange)`, `ScanRoots()`.
- `Evict()` / `ShrinkCache()` — ResourceManager pressure integration.
- `GetHealth()` / `MetricsSnapshot()` — engine-wide diagnostics contract.

## Internal Components

| Component | File | Role |
|---|---|---|
| ContentManager | `ContentManager.hpp/.cpp` | Facade, lifecycle, integrations |
| AssetDatabase | `AssetDatabase.hpp/.cpp` | Metadata-only store + indexes |
| AssetRegistry | `AssetRegistry.hpp/.cpp` | UUID → runtime asset, aliases, dup detection |
| AssetCache | `AssetCache.hpp/.cpp` | LRU memory cache (pin/evict/shrink) |
| AssetLoader | `AssetLoader.hpp/.cpp` | Sync/async loads, progress, cancellation |
| ImportManager | `ImportManager.hpp/.cpp` | Importer registry + pipeline |
| ExportManager | `ExportManager.hpp/.cpp` | Exporter registry |
| AssetWatcher | `AssetWatcher.hpp/.cpp` | FS change detection → events + DB sync |
| AssetIndexer | `AssetIndexer.hpp/.cpp` | Inverted index + search |
| AssetValidator | `AssetValidator.hpp/.cpp` | Missing/corrupt/dup/invalid checks |
| AssetSerializer | `AssetSerializer.hpp/.cpp` | Metadata JSON round-trip + migration |
| AssetCompressor | `AssetCompressor.hpp/.cpp` | Stored/RLE compressors (zlib future) |
| ThumbnailManager | `ThumbnailManager.hpp/.cpp` | Placeholder + image dims thumbnails |
| Vfs (Disk/Memory/Zip) | `Vfs.hpp/.cpp` | Virtual File System |

## State Machine

Asset state: `Unknown → Discovered → Indexed → Imported → Validated → Cached →
Loaded → Referenced → Modified → Saved → Exported → Archived → Deleted`
(repr. `AssetState` in `AssetTypes.hpp`).

## Threading Model

- `AssetDatabase`, `AssetRegistry`, `AssetCache`, `AssetIndexer` are mutex-guarded.
- `AssetLoader::LoadAsync` dispatches to the Core ThreadPool (06).
- `AssetWatcher` runs a polling heartbeat via the Core TaskScheduler (07).

## Events Published

`content.asset_added`, `content.asset_removed`, `content.asset_changed`,
`content.asset_loaded`, `content.asset_unloaded`, `content.asset_imported`,
`content.asset_exported`, `content.asset_deleted`, `content.asset_indexed`,
`content.cache_updated`, `content.thumbnail_generated`,
`content.validation_failed`.

## Events Consumed

`engine.kernel.shutdown_started`, `engine.config.hot_reload`,
`engine.resource.pressure_high`.

## Dependencies

Logger (02), ConfigurationManager (03), EventBus (05), ThreadPool (06),
TaskScheduler (07), ResourceManager (10), DatabaseManager (core/database),
PAL IFilesystem (Phase 2).

## Failure Modes

- Unsupported format → `Content_UnsupportedFormat`; importer lookup failure.
- Import pipeline failure → `Content_ImportFailed` with cause chain.
- Validation failure → `Content_ValidationFailed` + `ContentValidationFailed` event.
- VFS path outside roots → `Content_VfsPathNotMounted`.
- Cancellation → `Content_Cancelled`.
- Cache allocation over budget → evict oldest unpinned entries (LRU).

## Performance Goals

- Metadata operations O(1)/O(log n) via UUID map + inverted index.
- Metadata loads deferred: loading a library of 100k+ assets stays responsive.
- Async loads keep the UI thread free; large loads are streamed via cache.

## Future Extensions

- Real raster/audio/video decoding in AssetLoader (codecs).
- zlib/deflate + zstd compression; Protobuf/FlatBuffers serialization.
- Cloud VFS mounts, plugin-provided VFS locations.
- AI embeddings in the AssetIndexer.
- Thumbnail generation from real decoders (background pipeline exists).
