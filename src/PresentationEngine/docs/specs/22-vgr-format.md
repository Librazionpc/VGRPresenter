# 22. Native Platform File Format — `.vgr`

## Objective

`.vgr` is the official project format of the platform: the authoritative
representation of projects created within the application. External formats
(PowerPoint, PDF, OpenSong, ChordPro) are import/export formats only — imported
content is always converted into the optimized `.vgr` representation.

Everything created by the platform is stored as a `.vgr` document with an
internal type:

```text
Church Service Show → Sunday Service.vgr
Presentation        → Easter Conference.vgr
Template            → Modern Worship.vgr
Theme               → Dark Theme.vgr
Workspace           → Production Workspace.vgr
Playlist            → Sunday Morning Playlist.vgr
Project Package     → Conference 2027.vgr
```

The extension stays the same; the internal **document type** determines what the
object represents (Show, Presentation, Template, Theme, Workspace, Playlist,
Project Package, Asset Collection, Configuration Profile — future types too).

## Every .vgr file includes

```text
File signature (magic number)      File format version
Engine version                     Document type
Unique UUID                        Creation date
Last modified date                 Author (optional)
Compression info                   Encryption info (future)
Integrity hash                     Dependency manifest
Embedded assets (when required)    External asset references (when required)
Search metadata                    Custom metadata
AI metadata (future)
```

The structure is extensible: new fields can be added without breaking older
files. A `.vgr` created today opens in future versions: the engine detects the
document version, applies any required migration, validates the migrated
document, and opens it transparently — original data preserved whenever possible.

## Container layout

```text
┌─────────────────────────────────────────┐
│ MAGIC  (8 bytes "BPSVGR01")             │
│ Header length (u32 LE)                  │
│ JSON header (version, type, uuid,       │
│   dates, author, compression, hash,     │
│   dependency manifest, metadata)        │
│ Section count (u32)                     │
│ Sections (name, kind, offset, length,   │
│   crc32) — e.g. "assets", "document"    │
│ Section payloads (compressed or stored) │
└─────────────────────────────────────────┘
```

The JSON header makes the format self-describing and forward-compatible;
sectioned payloads support streaming and incremental loading. Compression uses
the Phase 3 AssetCompressor (stored/RLE today, pluggable). Integrity is checked
with CRC32 per section + a whole-file hash.

## Design philosophy

Fast to load and save · optimized for streaming/incremental loading · highly
compressible · versioned · extensible · cross-platform · recoverable after
unexpected shutdowns (header precedes payloads; corrupt sections are isolated) ·
secure against corruption · friendly to future collaboration and cloud sync.

## Definition of Done

- [x] Magic number + format version + engine version.
- [x] Document type + UUID + creation/modified dates + author.
- [x] Compression info + integrity hash (CRC32).
- [x] Dependency manifest + external asset references.
- [x] Embedded assets section.
- [x] Search metadata + custom metadata.
- [x] Version detection + migration hook + validation on open.
- [x] Sectioned container: streaming/incremental friendly.
- [x] Round-trip: serialize → deserialize → identical document.
- [x] Corruption detection (bad magic / bad CRC fails cleanly).
- [x] Tests: round-trip, migration, corruption, large payloads, dependencies.
