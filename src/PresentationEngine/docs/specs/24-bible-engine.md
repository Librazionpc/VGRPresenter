# 24. Bible Engine (Phase 12)

| Spec version | 1.0.0 |
|---|---|
| Spec status | Approved |
| Implementation status | Complete |
| Source files | `modules/bible/`, `modules/include/modules/bible/` |
| Namespace | `bps::bible` |

## Purpose

The Bible Engine is the platform's **first-class Scripture knowledge system**. It
discovers, imports, validates, indexes, organizes, queries, references, formats,
compares, and presents Scripture — while remaining completely independent of the
user interface. The engine never knows whether verses are being displayed in a
presentation, searched by AI, or exported to another format. It provides
Scripture as structured, queryable, versioned data.

The Bible Engine knows **nothing** about WinUI, Windows, Linux, presentations,
slides, templates, outputs, or rendering. It understands only Bible versions,
books, chapters, verses, paragraphs, headings, sections, footnotes, cross
references, and translation metadata.

## Responsibilities

1. **Import** Bible data from any supported source through pluggable providers
   (internal VGR Bible, USFM, OSIS, Zefania XML, JSON, plain text, future online
   providers) and convert it into the canonical internal model.
2. **Validate** every imported Bible (integrity checks, metadata extraction,
   reference validation) before it enters the active library — corrupt or
   incomplete files never load.
3. **Index** actual Scripture content through the platform Search Engine so
   users can search verse text, references, and metadata.
4. **Resolve references** such as `John 3:16`, `John 3`, `Psalm 23`,
   `Genesis 1:1-10`, `Romans 8`, `Matthew 5:3-12`, `1 Corinthians 13`, whole
   books, and multi-passage ranges, normalizing and validating them.
5. **Query** verses, chapters, books, and passages instantly.
6. **Format** Scripture in paragraph mode, verse-per-line, poetry/prose,
   headings, footnotes, red-letter, and translation-specific layouts — the
   formatting rules belong to the engine, not the UI.
7. **Compare** any number of installed translations in parallel with verse
   alignment handled by the engine.
8. **Manage user data** (notes, highlights, bookmarks, tags, collections)
   separately from Scripture text so source updates never overwrite user data.
9. **Expose a clean API** so WinUI, web, mobile, CLI, and AI consume the same
   Scripture services without duplicating logic.

## Public API

```cpp
namespace bps::bible {

class BibleEngine final : public IService {
public:
    static BibleEngine& Instance();

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
    Result<void> RegisterProvider(std::shared_ptr<IBibleProvider> provider);
    Result<void> UnregisterProvider(std::string_view name);
    std::vector<std::string> ProviderNames() const;

    // Import pipeline: detect -> validate -> convert -> store -> index.
    Result<std::string> Import(std::string_view source, std::string_view format,
                               const ImportOptions& options = {});
    Result<void> RemoveBible(std::string_view bibleId);
    std::vector<std::string> BibleIds() const;
    size_t BibleCount() const;

    // Lookup
    Result<BibleVersion> GetBible(std::string_view bibleId) const;
    Result<BibleBook> GetBook(std::string_view bibleId, std::string_view bookId) const;
    Result<std::vector<BibleVerse>> GetPassage(std::string_view bibleId,
                                               const PassageRef& ref) const;
    Result<BibleVerse> GetVerse(std::string_view bibleId, std::string_view bookId,
                                int chapter, int verse) const;
    Result<size_t> VerseCount(std::string_view bibleId) const;

    // Reference resolution (normalize + validate + expand)
    Result<PassageRef> ResolveReference(std::string_view text) const;
    Result<std::vector<PassageRef>> ResolveReferences(std::string_view text) const;

    // Search (actual verse content via Search Engine)
    Result<std::vector<BibleSearchHit>> Search(std::string_view query,
                                               std::string_view bibleId = "") const;

    // Parallel Bible (align the same reference across versions)
    Result<std::vector<ParallelVerse>> Compare(std::string_view refText,
                                               const std::vector<std::string>& bibleIds) const;

    // Formatting (rules live here, not in the UI)
    Result<std::string> Format(std::string_view bibleId, const PassageRef& ref,
                               const FormatOptions& options = {}) const;

    // User data (stored separately from Scripture)
    Result<void> AddNote(std::string_view bibleId, const PassageRef& ref,
                         const std::string& text);
    Result<std::vector<UserNote>> Notes(std::string_view bibleId, const PassageRef& ref) const;
    Result<void> SetHighlight(std::string_view bibleId, const PassageRef& ref, bool on);
    Result<std::vector<PassageRef>> Highlights(std::string_view bibleId) const;
    Result<void> AddCollection(std::string_view name);
    Result<void> AddToCollection(std::string_view collection, const PassageRef& ref);
    Result<std::vector<PassageRef>> Collection(std::string_view collection) const;

    // Cross references (queryable relationships)
    Result<std::vector<CrossReference>> CrossReferences(std::string_view bibleId,
                                                        const PassageRef& ref) const;

    // Events
    void WireEvents();
    void UnwireEvents();
};

} // namespace bps::bible
```

## Internal Components

```text
Bible Engine
  ├── Bible Manager (facade, owns everything below)
  ├── Bible Registry        — installed versions by id
  ├── Bible Providers       — IBibleProvider per source format
  ├── Bible Parser          — format -> canonical model
  ├── Bible Storage         — canonical model + user data stores
  ├── Reference Resolver    — "John 3:16" -> PassageRef
  ├── Query Engine          — instant verse/chapter/book lookup
  ├── Formatter             — paragraph/verse-per-line/headings/footnotes
  └── Results               — structured query results
```

Every component has a single responsibility. Providers convert external formats
into the **canonical internal model**; the rest of the engine never sees USFM,
OSIS, XML, or JSON.

## Canonical Model

```cpp
struct TranslationMetadata {
    std::string id;            // stable id ("KJV", "vgr", ...)
    std::string name;          // display name
    std::string language;
    std::string copyright;
    std::string license;
    std::string source;        // where it came from
    std::string version;       // translation version
    std::string abbreviation;
    bool readOnly = false;
};

struct BibleBook {
    std::string id;            // stable ("GEN", "PSA", "MRK", "1CO", ...)
    std::string name;          // "Genesis", "1 Corinthians"
    std::string testament;     // "old" | "new"
    std::vector<std::string> aliases;   // "Gen", "Gn", "Ge", ...
    int order = 0;
};

struct BibleVerse {
    std::string bookId;
    int chapter = 0;
    int verse = 0;
    std::string text;
    std::string heading;       // section heading ("" if none)
    bool redLetter = false;
    std::vector<Footnote> footnotes;
    std::vector<CrossReference> crossRefs;
};

struct BibleChapter {
    std::string bookId;
    int number = 0;
    std::vector<BibleVerse> verses;
    std::string title;         // chapter heading
};

struct BibleVersion {
    TranslationMetadata metadata;
    std::vector<BibleBook> books;          // ordered
    std::map<std::string, BibleChapter, std::less<>> chapters;   // "GEN.1" -> chapter
    std::vector<BibleVerse> verses;        // flattened, ordered (search + lookup)
};
```

## Reference Resolution

The reference parser understands:

```text
John 3:16            John 3             Psalm 23
Genesis 1:1-10       Romans 8           Matthew 5:3-12
1 Corinthians 13     Entire books       Multiple passages
John 3:16,18         John 3:16-18       Gen 1:1-2,5-7
```

Book names are resolved through `BibleBook.aliases` (canonical abbreviations
included), normalized case-insensitively, and validated against the loaded
Bible. References are expanded into explicit `PassageRef { bookId, chapter,
verseStart, verseEnd }` values with automatic cross-chapter ranges.

## Search Integration

The Bible Engine uses the **platform Search Engine** (docs/specs/20) through an
`IIndexAdapter` for type `bible`. Each verse is indexed as its own searchable
document (`bible:<bibleId>:<book>:<chapter>:<verse>` → content = verse text,
metadata = book/chapter/verse/reference) plus one document per book for
headings. Search inspects the actual verse content — never only references or
book names — so `"Love is patient"` finds the verse, and `"John 3:16"` resolves
through the reference resolver first.

## Parallel Bible Support

`Compare(reference, versions)` aligns the same `PassageRef` across any number of
installed versions. Alignment is by canonical reference (book/chapter/verse);
versions missing a verse report it as absent rather than misaligning.

## Formatting

Formatting rules belong to the Bible Engine:

- **Paragraph mode** — verses joined into flowing paragraphs (verse numbers as
  superscript markers by default).
- **Verse-per-line** — one verse per line with optional number prefix.
- **Headings** — section headings and chapter titles rendered above content.
- **Footnotes / red-letter** — preserved and emitted when requested.
- Translation-specific overrides are metadata-driven, never UI state.

## User Data

Notes, highlights, bookmarks, tags, and collections are stored in separate
stores keyed by canonical reference — **never inside the Scripture text**. If a
translation is updated or replaced, user data keyed by stable book ids survives.

Collections are first-class objects (favorite verses, reading plans, sermon
passages, topic collections, user-defined collections).

## Import & Validation

```text
Import -> Validation -> Integrity Checks -> Metadata Extraction
       -> Reference Validation -> Conversion -> Indexing -> Ready
```

Corrupt or incomplete files are rejected at import time with typed errors and
never enter the active library. A failure during any stage aborts the import
without partial state.

## EventBus

**Consumes:** `engine.config.hot_reload`.
**Publishes:** `bible.loaded`, `bible.indexed`, `bible.imported`,
`bible.updated`, `bible.removed`, `bible.passage_resolved`,
`bible.search_completed`, `bible.validation_completed`, `bible.validation_failed`.

The engine never calls notification or UI code directly; the Notification
Service (docs/specs/14) maps these events to user notifications.

## Plugin Support

Plugins add Bible providers, importers, exporters, reference parsers, search
strategies, formatting engines, language packs, commentary providers, lexicon
providers, and study resources **without modifying the Bible Engine**. Adding a
new format is: implement `IBibleProvider` → register → done.

## Performance

- Instant verse lookup (in-memory canonical storage, flattened verse vector +
  chapter map).
- Instant reference parsing (single-pass tokenizer over alias tables).
- Near-instant full-text search (delegated to the Search Engine).
- Background indexing (import never blocks the live presentation).
- Efficient memory usage; dozens of installed translations comfortably handled.

## State Machine

The engine is stateless with respect to presentation; its own lifecycle follows
the engine-wide contract: `Created → Initialized → Started → Running → Stopped →
Shutdown`. Import runs as a validated pipeline and either completes fully or
leaves no partial state.

## Threading Model

All mutable state is guarded by an internal mutex. Imports may run on a worker
pool thread; published events are dispatched on the EventBus. Lookups are
const-correct and safe to call from any thread after initialization.

## Dependencies

`core/common`, `core/events` (EventBus), `core/logging`, `modules/search`
(Search Engine for indexing), `interfaces/IService`. No dependency on
presentation, rendering, display, or output systems.

## Failure Modes

| Failure | Behavior |
|---|---|
| Unsupported format | `Bible_UnsupportedFormat`, no import |
| Corrupt / incomplete file | `Bible_ValidationFailed`, never enters the library |
| Missing book/chapter/verse | `Bible_NotFound` |
| Invalid reference text | `Bible_InvalidReference` |
| Duplicate Bible id | `Bible_AlreadyExists` |
| Provider parse error | typed error from the provider; import aborts |

## Future Extensions

Semantic Scripture search, natural-language lookup, related-passage discovery,
automatic cross-reference suggestions, sermon preparation, context-aware
recommendations, online Bible providers, commentary/lexicon providers — all
consume the existing API without redesign.

## Testing Requirements

Unit tests (`TestBible*`) must cover: provider registration, import of every
built-in format, validation/integrity rejection, metadata extraction, reference
resolution (single, ranges, cross-chapter, aliases, invalid), instant lookup,
content search, parallel comparison, formatting modes, user data separation,
collections, cross references, events, error paths, and lifecycle. The
**architecture test**: a CLI program imports Bibles, validates them, resolves
`John 3:16`, searches their actual text, compares translations, retrieves cross
references, and manages user-linked metadata — with no presentation or
rendering code involved.
