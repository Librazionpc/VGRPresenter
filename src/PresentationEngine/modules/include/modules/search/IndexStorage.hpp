#pragma once

// IndexStorage (docs/specs/20 §Indexing). An in-memory inverted index:
// term → postings (docId, term frequency, position). Supports full +
// incremental updates and a JSON snapshot for persistence. The engine never
// knows content types — only terms and documents.
//
// PERFORMANCE MODEL (2026-09-24 overhaul — the query-path audit):
//  - Reads take a shared_lock, writes an exclusive one: concurrent queries no
//    longer serialize behind each other or behind background indexing.
//  - Each document keeps PRECOMPUTED lowercase title/content (DocRecord) at
//    index time. Ranking and snippet building used to Lower() the full text of
//    every candidate document per query — megabytes of fresh strings per
//    keystroke; now they scan the ready-made bytes with zero allocation.
//  - LookupDocIds returns just the ids of a term's postings (no Posting
//    vector copies), and DocumentsById copies the docs once per QUERY (not
//    once per candidate per term). GetDocument-by-value remains for the
//    single-doc callers (services, snapshot, tests).

#include "core/common/Common.hpp"
#include "modules/search/SearchTypes.hpp"

#include <functional>
#include <map>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace bps::search {

struct Posting {
    std::string docId;
    size_t frequency = 0;
    size_t positions = 0;   // number of match positions (for phrase scoring)
};

// One indexed document + its query-time precomputation. Stored by value in an
// unordered_map (pointer-stable values — a rehash never moves a record).
struct DocRecord {
    SearchDocument doc;
    std::string lowerTitle;     // ready for ranking/snippets — no per-query Lower()
    std::string lowerContent;
    size_t tokenCount = 0;      // |D| for BM25 length normalization (all fields)
};
// (DocRecord carries TWO full text copies — doc.content + lowerContent — so
// fetching one per candidate per keystroke was the Quick-search lag: a common
// word registered thousands of candidates => hundreds of MB of string copies.
// Queries now pre-score from the postings and fetch records for the top few
// dozens only; DocMetas feeds that pre-score without touching the texts.)

class IndexStorage {
public:
    Result<void> Initialize();
    Result<void> Shutdown();

    // Adds/updates one document: tokenizes title+content+tags+metadata and
    // rebuilds its postings. Returns the number of terms indexed.
    size_t Upsert(const SearchDocument& doc);

    // Removes a document and all its postings.
    Result<void> Remove(std::string_view docId);

    // Lookup postings for a term (lowercased unless case-sensitive).
    std::vector<Posting> Lookup(std::string_view term) const;

    // FAST query path: just the doc ids a term posted to — no Posting copies.
    std::vector<std::string> LookupDocIds(std::string_view term) const;

    // BM25 query path: a term's (docId, term-frequency) pairs — the tf side of
    // tf·(k1+1)/(tf + k1·lengthNorm). Doc frequency comes from the pair count.
    struct TermTf { std::string docId; size_t tf = 0; };
    std::vector<TermTf> LookupTf(std::string_view term) const;

    // Corpus stats for BM25: N and the average document length (avgdl).
    struct CorpusStats { size_t documents = 0; size_t totalTokens = 0; };
    CorpusStats Corpus() const;

    // The candidate documents in ONE pass (one copy per doc per query, not per
    // posting per term), missing ids silently dropped.
    std::vector<DocRecord> DocumentsById(const std::vector<std::string>& ids) const;

    // Token count + type per doc WITHOUT copying any text (the lag fix:
    // pre-scoring ranks the candidates from the index alone; only the top few
    // dozen get full DocRecords with their two text copies each). The type is
    // half the payload: with a type filter set, the pre-score must drop other
    // types EARLY — a common word's top-48 global candidates are all short
    // Bible verses (the length tiebreak favors them), which would leave zero
    // sermon rows after the filter empties them.
    struct DocMeta { size_t tokens; std::string type; };
    std::unordered_map<std::string, DocMeta> DocMetas(
        const std::vector<std::string>& ids) const;

    // All terms with a given prefix (suggestions / prefix search).
    std::vector<std::string> TermsWithPrefix(std::string_view prefix, size_t limit = 50) const;

    // Visits every vocabulary term with its doc frequency under ONE shared lock
    // (fuzzy-correction scans: the callback can cheap-filter by length, then run
    // edit distance; df comes free from the postings size — no re-lookup, which
    // would deadlock on the non-recursive shared_mutex).
    void ForEachTermWithDf(const std::function<bool(const std::string&, size_t)>& visit) const;

    // Document store access.
    Result<SearchDocument> GetDocument(std::string_view docId) const;
    std::vector<std::string> DocumentIds() const;
    size_t DocumentCount() const;
    size_t TermCount() const;

    // Corruption-tolerant snapshot (JSON) — used by the SearchSerializer.
    std::string Snapshot() const;
    Result<size_t> Restore(std::string_view json);

    // Stats.
    size_t TotalPostings() const;

private:
    // MUST hold mutex_ (shared for reads, unique for writes).
    const DocRecord* FindRecordLocked(std::string_view docId) const;

    mutable std::shared_mutex mutex_;
    std::map<std::string, std::map<std::string, Posting, std::less<>>, std::less<>> index_;
    std::unordered_map<std::string, DocRecord> documents_;
    size_t totalTokens_ = 0;   // Σ tokenCount over live documents (avgdl denominator)
    bool initialized_ = false;
};

} // namespace bps::search
