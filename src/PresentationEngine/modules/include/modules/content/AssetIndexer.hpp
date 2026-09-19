#pragma once

// AssetIndexer (docs/specs/13 §Asset Indexer): builds a search index over
// names, tags, category, description, and file names. Supports exact, partial
// (prefix + substring), and fuzzy (edit-distance) matching plus structured
// filters (type, tag, category, extension, favorite, date, author).

#include "modules/content/AssetMetadata.hpp"

#include <map>
#include <mutex>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

namespace bps::content {

struct SearchQuery {
    std::string text;             // free-text term (exact/partial/fuzzy)
    AssetType type = AssetType::Unknown;   // Unknown = any
    std::string tag;
    std::string category;
    std::string extension;        // e.g. "png"
    std::string author;
    bool favoritesOnly = false;
    int64_t modifiedSinceMs = 0;
    int maxResults = 50;
    bool fuzzy = false;           // enable fuzzy matching on the text term
};

struct SearchResult {
    AssetMetadata meta;
    double score = 0.0;
};

class AssetIndexer {
public:
    AssetIndexer() = default;

    void Index(const AssetMetadata& meta);
    void Remove(const Uuid& uuid);
    void Clear();

    // Full-text search with filters. Results sorted by score desc.
    std::vector<SearchResult> Search(const SearchQuery& q) const;

    size_t IndexedCount() const;
    size_t TokenCount() const;

private:
    void Tokenize(const std::string& text, std::vector<std::string>& out) const;
    static int EditDistance(std::string_view a, std::string_view b);

    mutable std::mutex mutex_;
    std::unordered_map<Uuid, AssetMetadata> docs_;
    std::map<std::string, std::set<std::string>> inverted_;   // token -> uuid strings
};

} // namespace bps::content
