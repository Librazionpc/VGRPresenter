# CAMS — Content & Asset Management System

Phase 3 architecture. CAMS makes **every piece of content an asset**: a UUID +
metadata record whose bytes live behind a Virtual File System. After this
phase, no module touches files directly — everything goes through the
`ContentManager` facade.

```
Presentation / Songs / Bible / Media
                 │
                 ▼
        ┌────────────────────┐
        │   ContentManager   │   ← the ONLY public API for content
        └─────────┬──────────┘
                  │
     ┌────────────┼─────────────────────────────┐
     ▼            ▼                             ▼
AssetDatabase  AssetManager                ImportManager   ExportManager
(metadata)     │  AssetRegistry            │  IImporter    │  IExporter
               │  AssetCache               ▼              ▼
               │  AssetLoader          importers/      exporters/
               │  ThumbnailManager      *.cpp           *.cpp
               ▼
          AssetIndexer ──── search
          AssetWatcher ──── FS changes → events
          AssetValidator ── corruption/dup checks
          AssetSerializer ─ JSON round-trip
          AssetCompressor ─ stored/RLE
                  │
                  ▼
        ┌────────────────────┐
        │ Virtual File System│   DiskVfs │ MemoryVfs │ ZipVfs
        └─────────┬──────────┘
                  ▼
        PAL IFilesystem (Phase 2)
```

## Layers

1. **Facade** — `ContentManager` (IService). Lifecycle, config, event wiring,
   ResourceManager pressure handling, DatabaseManager persistence.
2. **Library** — `AssetDatabase` (metadata + indexes), `AssetRegistry`
   (UUID→runtime asset, aliases, duplicate detection).
3. **Runtime** — `AssetCache` (LRU, pin/evict/shrink), `AssetLoader`
   (sync/async, progress, cancellation), `ThumbnailManager`.
4. **Pipeline** — `ImportManager` (IImporter registry), `ExportManager`
   (IExporter registry), `AssetValidator`, `AssetSerializer`,
   `AssetCompressor`.
5. **Discovery** — `AssetWatcher` (TaskScheduler heartbeat poll),
   `AssetIndexer` (inverted index + filters).
6. **Transport** — Virtual File System: `DiskVfs` (PAL filesystem),
   `MemoryVfs` (in-memory), `ZipVfs` (stored-entry ZIP read/write).

## Key rules

- **No direct file access outside CAMS.** All I/O goes through the VFS, which
  sits on the PAL `IFilesystem`.
- **Metadata is separate from bytes.** `AssetDatabase` stores metadata only;
  the database row is JSON-persisted via `DatabaseManager` (`content` collection).
- **Open for extension, closed for modification.** New formats = implement
  `IImporter`/`IExporter` and register. No CAMS code changes.
- **Events, not calls.** All lifecycle transitions publish `content.*` events;
  CAMS consumes `Shutdown`, `ConfigHotReload`, `ResourcePressureHigh`.
- **Engine-wide contract.** `ContentManager` implements the full
  IService lifecycle (`Initialize/Start/Stop/Shutdown/Reload/Reset`),
  `GetHealth()`, and `MetricsSnapshot()`.

## Integration with Core

| Core system | How CAMS uses it |
|---|---|
| Logger | Every import/load/save/validation/error is logged with module tag `CAMS` |
| ConfigurationManager | `cams.cache.bytes`, `cams.watch.enabled`, `cams.roots` |
| EventBus | Publishes `content.*`; subscribes to shutdown/hot-reload/pressure |
| ThreadPool | Async loads (`LoadAsync`) |
| TaskScheduler | Watcher heartbeat (1 s poll) |
| ResourceManager | On `pressure_high`: `Evict()` unpinned, `ShrinkCache()` |
| DatabaseManager | `content` collection persists metadata JSON |
| PAL | All file ops via `IPlatform::Filesystem()` |

## Design notes

- `Uuid` is a v4-style random UUID (`Uuid.hpp`).
- `AssetType` covers the full Phase 3 taxonomy (Presentation, Slide, Song,
  Bible, Image, Video, Audio, Font, Theme, Template, Background, Overlay, ...).
- ZIP support is stored-entry (method 0) for both read and write — packages
  produced by `PackageExporter` are standard `.zip` archives readable by any
  unzip tool. Deflate compression is available through `DeflateCompressor`
  (zlib, behind `ICompressor`); zstd remains a documented future extension.
- Thumbnails: placeholder generation + image dimension extraction (PNG/JPEG
  header parse); real raster decode is available through the self-contained
  zlib-based `PngCodec` in the rendering module.
- Performance: 100k+ asset libraries stay responsive because metadata loads
  are cheap and large file loads are deferred until `Load()` is called.
