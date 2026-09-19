#include "modules/content/AssetIndexer.hpp"

#include <algorithm>
#include <cmath>
#include <cctype>

namespace bps::content {

namespace {

std::string Lower(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s)
        out += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

} // namespace

void AssetIndexer::Tokenize(const std::string& text, std::vector<std::string>& out) const {
    std::string cur;
    for (char c : text) {
        if (std::isalnum(static_cast<unsigned char>(c))) {
            cur += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        } else if (!cur.empty()) {
            out.push_back(cur);
            cur.clear();
        }
    }
    if (!cur.empty()) out.push_back(cur);
}

void AssetIndexer::Index(const AssetMetadata& meta) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::string id = meta.uuid.ToString();
    // Remove stale tokens for this doc.
    auto oldIt = docs_.find(meta.uuid);
    if (oldIt != docs_.end()) {
        std::vector<std::string> oldTokens;
        Tokenize(oldIt->second.name + " " + oldIt->second.description, oldTokens);
        for (const auto& t : oldTokens) {
            auto& s = inverted_[t];
            s.erase(id);
            if (s.empty()) inverted_.erase(t);
        }
        for (const auto& t : oldIt->second.tags) {
            auto& s = inverted_[Lower(t)];
            s.erase(id);
            if (s.empty()) inverted_.erase(Lower(t));
        }
    }
    docs_[meta.uuid] = meta;

    std::vector<std::string> tokens;
    Tokenize(meta.name + " " + meta.description + " " + meta.path, tokens);
    for (const auto& t : tokens) inverted_[t].insert(id);
    for (const auto& t : meta.tags) inverted_[Lower(t)].insert(id);
    if (!meta.category.empty()) inverted_[Lower(meta.category)].insert(id);
    if (!meta.author.empty()) inverted_[Lower(meta.author)].insert(id);
}

void AssetIndexer::Remove(const Uuid& uuid) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = docs_.find(uuid);
    if (it == docs_.end()) return;
    std::string id = uuid.ToString();
    std::vector<std::string> tokens;
    Tokenize(it->second.name + " " + it->second.description + " " + it->second.path, tokens);
    for (const auto& t : tokens) {
        auto& s = inverted_[t];
        s.erase(id);
        if (s.empty()) inverted_.erase(t);
    }
    for (const auto& t : it->second.tags) {
        auto& s = inverted_[Lower(t)];
        s.erase(id);
        if (s.empty()) inverted_.erase(Lower(t));
    }
    docs_.erase(it);
}

size_t AssetIndexer::IndexedCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return docs_.size();
}

size_t AssetIndexer::TokenCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    size_t n = 0;
    for (const auto& [t, s] : inverted_) {
        (void)t;
        n += s.size();
    }
    return n;
}

int AssetIndexer::EditDistance(std::string_view a, std::string_view b) {
    if (a.size() > b.size()) std::swap(a, b);
    if (a.empty()) return static_cast<int>(b.size());
    std::vector<int> prev(b.size() + 1), cur(b.size() + 1);
    for (size_t j = 0; j <= b.size(); ++j) prev[j] = static_cast<int>(j);
    for (size_t i = 1; i <= a.size(); ++i) {
        cur[0] = static_cast<int>(i);
        for (size_t j = 1; j <= b.size(); ++j) {
            int cost = a[i - 1] == b[j - 1] ? 0 : 1;
            cur[j] = std::min({prev[j] + 1, cur[j - 1] + 1, prev[j - 1] + cost});
        }
        std::swap(prev, cur);
    }
    return prev[b.size()];
}

std::vector<SearchResult> AssetIndexer::Search(const SearchQuery& q) const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::string term = Lower(q.text);

    std::set<std::string> candidates;
    bool hasTerm = !term.empty();
    if (hasTerm) {
        // Exact token match.
        auto it = inverted_.find(term);
        if (it != inverted_.end()) candidates = it->second;
        // Partial: prefix + substring across the index.
        if (!q.fuzzy) {
            for (const auto& [token, ids] : inverted_) {
                if (token.size() < term.size()) continue;
                if (token.starts_with(term) || token.contains(term)) {
                    for (const auto& id : ids) candidates.insert(id);
                }
            }
        }
    } else {
        for (const auto& [token, ids] : inverted_) {
            (void)token;
            for (const auto& id : ids) candidates.insert(id);
        }
    }

    std::vector<SearchResult> out;
    for (const auto& id : candidates) {
        Uuid u = Uuid::FromString(id);
        auto docIt = docs_.find(u);
        if (docIt == docs_.end()) continue;
        const AssetMetadata& m = docIt->second;

        // Structured filters.
        if (q.type != AssetType::Unknown && m.type != q.type) continue;
        if (!q.tag.empty() &&
            std::find(m.tags.begin(), m.tags.end(), q.tag) == m.tags.end())
            continue;
        if (!q.category.empty() && m.category != q.category) continue;
        if (!q.extension.empty()) {
            auto dot = m.path.find_last_of('.');
            std::string ext = dot == std::string::npos ? "" : Lower(m.path.substr(dot + 1));
            if (ext != Lower(q.extension)) continue;
        }
        if (!q.author.empty() && m.author != q.author) continue;
        if (q.favoritesOnly && !m.favorite) continue;
        if (q.modifiedSinceMs > 0 && m.modifiedMs < q.modifiedSinceMs) continue;

        double score = 0.0;
        if (hasTerm) {
            std::string nameLower = Lower(m.name);
            if (nameLower == term) score += 100.0;
            else if (nameLower.starts_with(term)) score += 60.0;
            else if (nameLower.contains(term)) score += 30.0;
            if (m.description.contains(q.text)) score += 5.0;
            for (const auto& t : m.tags)
                if (Lower(t).contains(term)) score += 10.0;
            if (q.fuzzy) {
                for (const auto& token : {Lower(m.name)}) {
                    int dist = EditDistance(term, token.size() > 64 ? token.substr(0, 64)
                                                                    : token);
                    if (dist <= 2) score += 40.0 - dist * 10.0;
                }
            }
        } else {
            score = 1.0;
        }
        out.push_back({m, score});
    }

    std::sort(out.begin(), out.end(),
              [](const SearchResult& a, const SearchResult& b) { return a.score > b.score; });
    if (static_cast<size_t>(q.maxResults) < out.size()) out.resize(static_cast<size_t>(q.maxResults));
    return out;
}

void AssetIndexer::Clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    docs_.clear();
    inverted_.clear();
}

} // namespace bps::content
