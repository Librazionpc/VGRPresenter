#pragma once

#include "core/common/Common.hpp"
#include "modules/search/SearchTypes.hpp"

#include <string>
#include <vector>

namespace bps::search {
// Turns a content object (any type) into a searchable document. Adapters are
// registered by plugins; the Search Engine never knows concrete content types.
class IIndexAdapter {
public:
    virtual ~IIndexAdapter() = default;
    virtual const char* Type() const noexcept = 0;                 // "song", "image"...
    virtual bool CanIndex(const SearchDocument& doc) const = 0;
    // Extracts searchable content from the raw payload (content-aware indexing).
    virtual Result<std::string> ExtractContent(const SearchDocument& doc) const = 0;
};

// Scores and orders results. Replaceable strategies (exact/prefix/fuzzy boosts,
// recency, favourites, future AI).
class IRankingStrategy {
public:
    virtual ~IRankingStrategy() = default;
    // Scores a candidate document for a query. Higher is better.
    virtual double Score(const SearchDocument& doc, const SearchQuery& query) const = 0;
    virtual bool IsDefault() const { return false; }
};

// Future search providers (vector, embedding, semantic, local/cloud AI).
class ISearchProvider {
public:
    virtual ~ISearchProvider() = default;
    virtual const char* Name() const noexcept = 0;
    virtual Result<std::vector<SearchResult>> Search(const SearchQuery& query) const = 0;
};
} // namespace bps::search