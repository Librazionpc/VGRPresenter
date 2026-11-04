#pragma once

#include "core/common/Common.hpp"
#include "modules/search/SearchTypes.hpp"

#include <functional>
#include <map>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <string_view>
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

    size_t DfOf(std::string_view term) const;

    // Corpus stats for BM25: N and the average document length (avgdl).
    struct CorpusStats { size_t documents = 0; size_t totalTokens = 0; };
    CorpusStats Corpus() const;

    // The candidate documents in ONE pass (one copy per doc per query, not per
    // posting per term), missing ids silently dropped.
    std::vector<DocRecord> DocumentsById(const std::vector<std::string>& ids) const;

    struct DocMeta { size_t tokens; std::string type; };
    std::unordered_map<std::string, DocMeta> DocMetas(
        const std::vector<std::string>& ids) const;

    // All terms with a given prefix (suggestions / prefix search).
    std::vector<std::string> TermsWithPrefix(std::string_view prefix, size_t limit = 50) const;

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
    // MUST hold mutex_ (unique): index one document, replacing any earlier copy of it.
    size_t UpsertLocked(const SearchDocument& doc);
    // MUST hold mutex_ (unique): take a document out of the postings of the terms IT posted to
    // (the same tokenizer the index was built with) — not a sweep over every term in the index.
    void ErasePostingsOf(const SearchDocument& doc);

    // A posting's numbers. WHICH DOCUMENT it is for is the key of the list it sits in, and that key is
    // a VIEW of the id string documents_ already owns (an unordered_map node: its key never moves). The
    // index used to hold the id twice more for every posting — as the key and again inside the Posting —
    // as full string copies of file paths, millions of times over: gigabytes of memory, and a window
    // that froze for tens of seconds freeing them when the app quit.
    struct PostingData {
        size_t frequency = 0;
        size_t positions = 0;
    };
    using PostingList = std::map<std::string_view, PostingData>;

    mutable std::shared_mutex mutex_;
    std::map<std::string, PostingList, std::less<>> index_;
    // Owns the id strings the postings view: a document is erased only AFTER its postings are.
    std::unordered_map<std::string, DocRecord> documents_;
    size_t totalTokens_ = 0;   // Σ tokenCount over live documents (avgdl denominator)
    bool initialized_ = false;
};
} // namespace bps::search