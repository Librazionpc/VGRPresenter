#pragma once

// Search & Indexing Engine (Phase 9, docs/specs/20). The central knowledge
// layer: only knows documents, metadata, relationships, and indexes — never
// Bibles, songs, images, or presentations. Content-type agnostic.

#include "core/common/Common.hpp"

#include <map>
#include <string>
#include <vector>

namespace bps::search {

// --- Generic document (docs/specs/20 §Document model) -------------------------
struct SearchDocument {
    std::string id;
    std::string type;                              // "song", "bible", "image"...
    std::string title;
    std::string content;                           // extracted full content
    std::map<std::string, std::string, std::less<>> metadata;   // extensible
    std::vector<std::string> tags;
    std::vector<std::string> keywords;
    std::string author;
    std::string language;
    std::string source;
    std::string version;
    int64_t createdMs = 0;
    int64_t modifiedMs = 0;
    int64_t rankBoost = 0;
    std::vector<std::string> relatedIds;           // relationships
};

// --- Query (docs/specs/20 §Search modes) ---------------------------------------
struct SearchQuery {
    std::string text;                              // raw query text
    std::vector<std::string> exactTerms;
    std::vector<std::string> prefixTerms;
    std::vector<std::string> fuzzyTerms;
    std::vector<std::string> phraseTerms;          // multi-word phrases
    std::map<std::string, std::string, std::less<>> metadataFilters;  // type:xxx
    int maxResults = 50;
    size_t offset = 0;
    bool caseSensitive = false;
};

// --- Filter (docs/specs/20 §Filters) -------------------------------------------
struct SearchFilter {
    std::string type;                              // "" = all
    std::string tag;                               // "" = all
    std::string language;                          // "" = all
    std::string author;                            // "" = all
    int64_t modifiedAfterMs = 0;
    int64_t modifiedBeforeMs = 0;                  // 0 = unbounded
};

// --- A ranked result ------------------------------------------------------------
struct SearchResult {
    std::string documentId;
    std::string type;
    std::string title;
    std::string snippet;                           // matching content excerpt
    double score = 0.0;
    int64_t modifiedMs = 0;
    std::vector<std::string> relatedIds;
};

// --- Suggestions (docs/specs/20 §Suggestions) ----------------------------------
struct SearchSuggestion {
    std::string text;
    size_t weight = 0;                             // popularity/history weight
};

} // namespace bps::search
