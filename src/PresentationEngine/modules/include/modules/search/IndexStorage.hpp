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
    uint32_t ordinal = 0;       // this document's slot in the postings (see IndexStorage::slots_)
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

    // A WORD LIST WITHOUT DOCUMENTS. A source with a huge text (the sermon library: ~100 MB) only needs its WORDS known
    // here — to correct a typo, split a run-together word, or offer a completion — and each word's document count.
    // Indexing the documents themselves stored the whole text three more times plus a posting per word per document
    // (~370 MB for the sermons). `terms` is sorted by word; setting a source again replaces its earlier list.
    void SetVocabulary(std::string_view source, std::vector<std::pair<std::string, uint32_t>> terms);
    void ClearVocabulary(std::string_view source);
    size_t VocabularyTermCount() const;

private:
    // MUST hold mutex_ (shared for reads, unique for writes).
    const DocRecord* FindRecordLocked(std::string_view docId) const;
    // MUST hold mutex_ (unique): index one document, replacing any earlier copy of it.
    size_t UpsertLocked(const SearchDocument& doc);
    // MUST hold mutex_ (unique): take a document out of the postings of the terms IT posted to
    // (the same tokenizer the index was built with) — not a sweep over every term in the index.
    void ErasePostingsOf(const DocRecord& rec);

    // A posting is 8 bytes: WHICH DOCUMENT is a small number (its slot, DocRecord::ordinal) and a term's list is a
    // vector sorted by it. It used to be a std::map<string_view, {size_t, size_t}> per term — a 64-byte tree node
    // (80 with the allocator's header) for every (word, document) pair: 774k of them for the Bible alone, ~47 MB
    // where this takes ~6. Slots are handed out in increasing order, so indexing a source appends; taking a document
    // out is a binary search and an erase.
    struct PostingEntry {
        uint32_t ordinal = 0;
        uint32_t frequency = 0;
    };
    using PostingList = std::vector<PostingEntry>;
    // slot -> the id string documents_ owns for it (an unordered_map node key: it never moves); null while free.
    std::vector<const std::string*> slots_;
    std::vector<uint32_t> freeSlots_;
    std::string_view IdOfSlot(uint32_t ordinal) const { return *slots_[ordinal]; }

    mutable std::shared_mutex mutex_;
    std::map<std::string, PostingList, std::less<>> index_;
    // Owns the id strings the postings view: a document is erased only AFTER its postings are.
    std::unordered_map<std::string, DocRecord> documents_;
    size_t totalTokens_ = 0;   // Σ tokenCount over live documents (avgdl denominator)
    // source -> its sorted (word, document count) list; see SetVocabulary.
    std::map<std::string, std::vector<std::pair<std::string, uint32_t>>, std::less<>> vocabulary_;
    bool initialized_ = false;
};
} // namespace bps::search