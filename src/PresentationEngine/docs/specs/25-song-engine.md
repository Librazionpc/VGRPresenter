# 25. Song & Lyrics Engine (Phase 13)

| Spec version | 1.0.0 |
|---|---|
| Spec status | Approved |
| Implementation status | Complete |
| Source files | `modules/songs/`, `modules/include/modules/songs/` |
| Namespace | `bps::song` |

## Purpose

The Song Engine is a platform-independent subsystem for importing, storing,
searching, editing, organizing, arranging, transposing, and presenting **structured
musical content**. A song is never a block of text: it is metadata, sections,
lyrics, chords, arrangements, notes, and media references — fully queryable and
extensible.

The Song Engine **never renders lyrics**. It returns structured song data. The
Scene Composition Engine (docs/specs/23) decides how a song looks on Audience /
Stage / Stream outputs; the Rendering Engine draws it; the Display Engine routes
it. The Song Engine knows nothing about presentations, layouts, or pixels.

## Responsibilities

1. **Import** songs from any supported source through pluggable providers
   (ChordPro, OpenSong, OpenLP, ProPresenter, EasyWorship, VideoPsalm, plain
   text, JSON, future formats) into the canonical Song model.
2. **Validate** every song on import (required fields, section integrity, chord
   legality) — malformed songs never enter the library.
3. **Store** the canonical model with a **stable id independent of the filename**.
4. **Index** the actual lyrics, chords, titles, authors, tags, and metadata
   through the platform Search Engine.
5. **Parse chords as structured data** (root, quality, extension, bass) so the
   engine understands `G`, `Am`, `D/F#`, `Cmaj7`, `Em7`, `G/B` as chords, not
   arbitrary text.
6. **Transpose** any song to any key — slash chords, minors, extensions,
   accidentals, and enharmonic equivalents included — without modifying the
   original.
7. **Manage arrangements** as independent ordered lists that reference existing
   sections (Original / Sunday / Short / Choir / Acoustic / Custom) without
   duplicating the song.
8. **Manage multiple keys** (original, preferred, current performance key,
   stored alternatives) so users never duplicate songs for a key change.
9. **Detect duplicates** using actual content (title, normalized lyrics,
   author, similarity), not just filenames.
10. **Version** songs — every save creates a recoverable version; a crash never
    corrupts the library.
11. **Store licensing information** (copyright holder/year, license, CCLI,
    restrictions, source) without assuming a single licensing model.
12. **Expose structured data to the future AI Engine** — AI consumes the Song
    API, never database files.

## Public API

```cpp
namespace bps::song {

class SongEngine final : public IService {
public:
    static SongEngine& Instance();

    // Lifecycle (engine-wide contract)
    Result<void> Initialize() override;
    Result<void> Start() override;
    Result<void> Stop() override;
    Result<void> Shutdown() override;
    Result<void> Reload() override;
    Result<void> Reset() override;
    HealthReport GetHealth() const override;
    Metrics MetricsSnapshot() const override;
    const char* ServiceName() const noexcept override;

    // Provider registry (add a format without engine changes)
    Result<void> RegisterProvider(std::shared_ptr<ISongProvider> provider);
    Result<void> UnregisterProvider(std::string_view name);
    std::vector<std::string> ProviderNames() const;

    // Import pipeline: detect -> parse -> validate -> dedupe -> store -> index.
    Result<std::string> Import(std::string_view source, std::string_view format,
                               const ImportOptions& options = {});
    Result<void> RemoveSong(std::string_view songId);
    std::vector<std::string> SongIds() const;
    size_t SongCount() const;

    // CRUD on the canonical model (stable ids, never filenames)
    Result<std::string> CreateSong(const Song& song);
    Result<Song> GetSong(std::string_view songId) const;
    Result<void> UpdateSong(std::string_view songId, const Song& song);
    Result<std::vector<Song>> Search(std::string_view query) const;

    // Transposition (preserves the original song)
    Result<Song> Transposed(std::string_view songId, int semitones) const;
    Result<void> SetPerformanceKey(std::string_view songId, std::string_view key);

    // Arrangements (independent orderings of existing section ids)
    Result<void> AddArrangement(std::string_view songId, const Arrangement& arr);
    Result<void> RemoveArrangement(std::string_view songId, std::string_view arrId);
    Result<std::vector<Arrangement>> Arrangements(std::string_view songId) const;

    // Sections (first-class; reorder without rewriting lyrics)
    Result<void> ReorderSections(std::string_view songId,
                                 const std::vector<std::string>& orderedSectionIds);

    // Collections (favorites, worship sets, custom categories)
    Result<void> CreateCollection(std::string_view name);
    Result<void> AddToCollection(std::string_view collection, std::string_view songId);
    Result<std::vector<std::string>> Collection(std::string_view collection) const;

    // Duplicate detection (content-based)
    Result<std::vector<DuplicateGroup>> FindDuplicates() const;
    Result<std::vector<std::string>> LikelyDuplicates(std::string_view songId) const;

    // Versioning & recovery
    Result<std::vector<SongVersion>> Versions(std::string_view songId) const;
    Result<Song> RestoreVersion(std::string_view songId, std::string_view versionId) const;

    // Events
    void WireEvents();
    void UnwireEvents();
};

} // namespace bps::song
```

## Internal Components

```text
Song Engine
  ├── Song Manager (facade, owns everything below)
  ├── Song Registry         — songs by stable id
  ├── Song Providers        — ISongProvider per source format
  ├── Song Parser           — format -> canonical Song model
  ├── Chord System          — Chord parsing, validation, transposition
  ├── Arrangement Manager   — independent section orderings
  ├── Duplicate Detector    — content similarity
  ├── Version Store         — immutable song snapshots + recovery
  └── Results               — structured query results
```

## Canonical Song Model

```cpp
struct SongMetadata {
    std::string title;
    std::vector<std::string> authors;
    std::string copyright;
    std::string ccli;             // CCLI song number
    std::string language;
    std::string originalKey;      // "C", "D#m", ...
    std::string preferredKey;
    std::string performanceKey;   // current
    std::string tempo;            // "72 bpm"
    std::string timeSignature;    // "4/4"
    std::vector<std::string> tags;
    std::string notes;
    LicensingInfo licensing;
    std::vector<MediaRef> mediaRefs;   // references to Media Engine assets only
};

struct SongLine {
    std::vector<ChordRef> chords;   // chord positions (structured)
    std::string lyrics;             // line text
};

struct SongSection {
    std::string id;                 // stable ("v1", "chorus", ...)
    std::string name;               // "VERSE 1", "CHORUS", "BRIDGE", custom
    std::vector<SongLine> lines;
};

struct Arrangement {
    std::string id;                 // "sunday", "short", ...
    std::string name;
    std::vector<std::string> sectionIds;   // ordered references to sections
};

struct Song {
    std::string id;                 // stable, never the filename
    SongMetadata metadata;
    std::vector<SongSection> sections;
    std::vector<Arrangement> arrangements;   // first = original order
    int64_t createdMs = 0;
    int64_t modifiedMs = 0;
};
```

## Chord System

A chord is structured data, not text:

```cpp
struct Chord {
    std::string root;        // "C", "C#", "Db", "D", ...
    ChordQuality quality;    // Major, Minor, Diminished, Augmented, Suspended,
                             // DominantSeventh, Seventh, Extended, Custom
    std::string extension;   // "7", "maj7", "9", "sus4", "m7b5", ...
    std::string bass;        // slash bass ("" if none) — "D/F#" has bass "F#"
    std::string custom;      // free-form when the notation is unrecognized
    std::string Display() const;   // canonical text form
};
```

The parser recognizes `G`, `Am`, `D/F#`, `Cmaj7`, `Em7`, `G/B`, `C#m7b5`,
suspended, diminished, augmented, and extended chords; unknown notations are
preserved as `custom` so nothing is ever lost. **Transposition** maps every chord
through a semitone table (with enharmonic resolution), preserves qualities,
extensions, and slash basses, and rewrites the song in the target key — the
original song object is never mutated.

## Sections & Arrangements

Sections are first-class objects with stable ids. An arrangement is an ordered
list of section ids referencing the song's sections — creating a Sunday
arrangement never rewrites or duplicates the lyrics. The first arrangement in a
song is the original order.

## Search Integration

Every song is indexed through the platform Search Engine via an `IIndexAdapter`
for type `song`. Search works against the **actual song content**: lyrics, chord
names, titles, authors, tags, copyright, metadata, sections, notes, and
collections. `song_027_final.opn` containing "Amazing grace, how sweet the sound"
is found by searching `how sweet the sound` — the filename is only one possible
field. Intelligent search (fuzzy, phrase, semantic, natural language, AI) plugs
into the existing API later.

## Duplicate Detection

`FindDuplicates` compares normalized lyrics (case/punctuation/whitespace
folded), title similarity, author, and copyright metadata. Importing the same
song twice reports a likely duplicate instead of blindly creating
`Amazing Grace (2)`, `Amazing Grace (3)`, and `Amazing Grace FINAL`.

## Versioning & Recovery

Every save stores an immutable snapshot. `Versions(songId)` lists snapshots and
`RestoreVersion` recovers any previous state. A crash during editing or import
never corrupts the library: writes are copy-on-write and imports are atomic.

## EventBus

**Consumes:** `song.selected`, `engine.config.hot_reload`.
**Publishes:** `song.loaded`, `song.indexed`, `song.imported`, `song.updated`,
`song.deleted`, `song.arrangement_changed`, `song.key_changed`,
`song.validated`, `song.validation_failed`.

No direct coupling to notification or UI systems — the Notification Service
decides how these events are presented.

## Plugin Support

Plugins provide importers, exporters, chord parsers, music metadata providers,
licensing providers, online song databases, AI song providers, translation
providers, and search providers **without modifying the Song Engine**. A future
`NEW_SONG_FORMAT` needs only: implement `ISongProvider` → register → done.

## Offline First

The engine works completely offline — searching, editing, transposing, arranging,
presenting, organizing, and saving require no internet. Online databases are
optional providers.

## Performance

Instant song lookup, instant chord transposition, instant arrangement changes,
background indexing, and large libraries (thousands or millions of indexed
lyrics) with no UI blocking and no presentation interruption.

## State Machine

The engine's lifecycle follows the engine-wide contract
(`Created → Initialized → Started → Running → Stopped → Shutdown`). It holds no
presentation state; a song's "current" state (selected, key, arrangement) is
project state, not engine state.

## Threading Model

All mutable state is guarded by an internal mutex. Imports may run on a worker
pool thread; events are dispatched on the EventBus. Lookups are const-correct
and safe from any thread after initialization.

## Dependencies

`core/common`, `core/events` (EventBus), `core/logging`, `modules/search`
(Search Engine for indexing), `modules/media` (media references only as ids),
`interfaces/IService`. No dependency on presentation, rendering, display, or
output systems.

## Failure Modes

| Failure | Behavior |
|---|---|
| Unsupported format | `Song_UnsupportedFormat`, no import |
| Malformed song | `Song_ValidationFailed`, never enters the library |
| Unknown song id | `Song_NotFound` |
| Duplicate registration | `Song_AlreadyExists` |
| Illegal chord | preserved as `custom`; transposition leaves it untouched |
| Missing section in arrangement | `Song_SectionNotFound` |
| Version missing | `Song_VersionNotFound` |

## Future Extensions

Fuzzy/phrase/semantic/AI song search ("songs about surrender"), AI worship-set
suggestion, similar-song discovery, automatic arrangement suggestions, lyric
translation, and metadata generation — all consume the existing Song API.

## Testing Requirements

Unit tests (`TestSong*`) must cover: provider registration, import of every
built-in format, validation, lyrics parsing, chord parsing, chord transposition
(including slash chords, minors, extensions, accidentals, enharmonics),
arrangement independence, reordering, duplicate detection, content search,
metadata, copyright data, versioning, recovery, collections, events, error
paths, and lifecycle. The **architecture test**: a CLI imports songs, validates,
searches lyrics, changes key, creates an arrangement, saves, reloads, and
exports — with no UI code anywhere in the path.
