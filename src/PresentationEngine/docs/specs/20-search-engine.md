# 20. Search & Indexing Engine (Phase 9)

## Objective

Build a high-performance, extensible search platform capable of indexing and
searching **every piece of content** managed by the engine. The Search &
Indexing Engine is the central knowledge layer: it does not know what a Bible,
song, image, or presentation is — it only knows **documents, metadata,
relationships, and indexes**.

Bible Engine needs search. Song Engine needs search. Media Library needs search.
AI RAG needs search. Asset Manager needs search. Templates need search. Plugins
need search. Build it once — everyone uses it.

## Architecture

```text
Content Sources → Document Adapters → Indexer → Index Storage
                                                       ↓
            Query Engine ← Ranking Engine ← Search Engine ← Results
```

## Core components

```text
SearchEngine            facade: query + suggest + sessions + history + profiles
IndexManager            document registry + incremental/full index lifecycle
Indexer                 tokenize + build/update the inverted index (background)
DocumentAdapterRegistry adapter registry (IIndexAdapter per content type)
DocumentParser          text extraction + tokenization
QueryEngine             exact/prefix/partial/fuzzy/wildcard/phrase/metadata/combined
RankingEngine           score + sort (exact > prefix > partial > metadata > recency)
SearchCache             LRU query cache (sized by the Adaptive Runtime)
SearchSessionManager    query state: filters, sort, paging, restorable
SearchHistoryManager    recent searches (suggestions + personalization)
SearchSuggestionEngine  auto-complete from index terms + history + popular
SearchProfileManager    named search profiles (filters + rank weights)
IndexMaintenanceService scheduled/background rebuilds
SearchValidator         index health checks
SearchSerializer        index snapshot/restore (JSON)
```

## Interfaces

```cpp
IIndexAdapter       // turns a content object into a searchable Document
IIndexStorage       // term -> postings store (in-memory + disk snapshot)
IRankingStrategy    // score(results) — replaceable
IQueryProcessor     // query -> QueryPlan (extensible processors)
ISearchFilter       // filter documents by metadata
ISearchSession      // query state, restorable
ISearchProvider     // future: vector / embedding / semantic / hybrid
```

Everything is interface-based; plugins register new adapters, indexers, ranking
strategies, query processors, and filters without modifying the Search Engine.

## Document model

Every searchable item becomes a generic **Document**:

```cpp
struct SearchDocument {
    std::string id;                  // uuid
    std::string type;                // "song", "bible", "image", "presentation"...
    std::string title;
    std::string content;             // extracted full content (content-aware indexing)
    std::map<std::string, std::string> metadata;   // extensible
    std::vector<std::string> tags;
    std::vector<std::string> keywords;
    std::string author;
    std::string language;
    std::string source;              // file path / asset id / plugin
    std::string version;
    int64_t createdMs = 0;
    int64_t modifiedMs = 0;
    int64_t rankBoost = 0;           // user favourites / history weight
    std::vector<std::string> relatedIds;   // relationships (presentation→song→bg)
};
```

## Content-aware indexing

The engine indexes **contents**, not filenames. A `.pptx` named
`Presentation_Final_v3.pptx` containing "Amazing Grace", "John 3:16" and
"Offering Prayer" is found by searching any of those strings. Document adapters
extract text from every supported format (PDF, PPTX, presentations, songs,
Bible translations, images via OCR providers, audio/video via speech providers).
Whether OCR or speech-to-text runs depends only on installed providers.

## Indexing

- Full indexing, incremental indexing, background indexing, scheduled indexing,
  manual rebuild, automatic updates.
- Editing one slide re-indexes **only that document** — never the whole library.
- The UI/render/presentation never block: indexing runs on the ThreadPool.

## Search modes

Exact · Prefix · Partial · Fuzzy (edit distance ≤ 2) · Wildcard (`*` `?`) ·
Phrase (`"..."`) · Metadata (`type:song tag:worship`) · Combined.

## Filters

Content type, date range, tags, language, author, collection/folder, custom
metadata keys.

## Ranking

Score = exact match (3.0) > prefix (2.0) > partial (1.0) > metadata (0.75) +
recency decay + rankBoost (favourites/history) + frequency. Strategies are
replaceable; AI ranking is a future provider.

## Suggestions

Auto-complete from index terms, recent searches, popular searches, related
content. Published as `search.suggestions_updated`.

## Relationships

Documents carry `relatedIds`; results can navigate relationships
(presentation → song → background image → bible verse).

## Sessions

Current query, filters, sort order, paging, history. Sessions are restorable.

## Cache

LRU cache for frequently searched queries + results + suggestions. The cache
budget comes from the Adaptive Runtime (`GetCacheBytes()`).

## Adaptive Runtime integration

The Search Engine requests memory budgets, thread counts, cache sizes, and
indexing priority — it never hardcodes them.

## EventBus

Consumes: `content.asset_loaded`, `content.asset_deleted`,
`project.opened` (via ConfigHotReload), `ConfigHotReload`.
Publishes: `search.index_updated`, `search.started`, `search.completed`,
`search.index_rebuilt`, `search.suggestions_updated`.

## Notification integration

Index rebuild complete, index corruption detected, search unavailable — the
Notification Service controls visibility.

## AI readiness

The API accommodates future providers: Vector, Embedding, Semantic, Local AI,
Cloud AI, Hybrid — without redesign.

## Performance

Millions of indexed documents, background incremental indexing, near-instant
search (<100 ms), low memory, large libraries.

## Code quality

No UI, no WinUI, no presentation/Bible/song/rendering logic. Only indexing,
querying, ranking, and retrieval.

## Definition of Done

- [x] Generic Document model with extensible metadata + relationships.
- [x] Document adapter registry (interface-driven; plugins add adapters).
- [x] Inverted index with full + incremental + background indexing.
- [x] Exact/prefix/partial/fuzzy/wildcard/phrase/metadata/combined search.
- [x] Replaceable ranking strategy with recency + favourites boost.
- [x] Filters (type, date, tags, language, author, custom metadata).
- [x] Suggestions (auto-complete, recent, popular).
- [x] Restorable search sessions + search history.
- [x] LRU query cache sized by the Adaptive Runtime.
- [x] EventBus integration (publishes index/search/suggest events).
- [x] Index health validation + JSON snapshot/restore.
- [x] Content-aware: contents are indexed, not just filenames.
- [x] Tests: index creation, incremental indexing, search correctness, ranking,
      filters, suggestions, large dataset, concurrent indexing, corruption
      recovery, performance.
