#pragma once

// IndexStorage (docs/specs/20 §Indexing). An in-memory inverted index:
// term → postings (docId, term frequency, position). Supports full +
// incremental updates and a JSON snapshot for persistence. The engine never
// knows content types — only terms and documents.

#include "core/common/Common.hpp"
#include "modules/search/SearchTypes.hpp"

#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace bps::search {

struct Posting {
    std::string docId;
    size_t frequency = 0;
    size_t positions = 0;   // number of match positions (for phrase scoring)
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

    // All terms with a given prefix (suggestions / prefix search).
    std::vector<std::string> TermsWithPrefix(std::string_view prefix, size_t limit = 50) const;

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
    mutable std::mutex mutex_;
    std::map<std::string, std::map<std::string, Posting, std::less<>>, std::less<>> index_;
    std::map<std::string, SearchDocument, std::less<>> documents_;
    bool initialized_ = false;
};

} // namespace bps::search
