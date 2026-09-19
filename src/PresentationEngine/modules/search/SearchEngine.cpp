#include "modules/search/SearchEngine.hpp"

#include "core/logging/Logger.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <format>

namespace bps::search {

namespace {

// Maximum ranked results cached per query — larger than any caller limit so a
// later, larger `limit` never under-fills from cache. Callers requesting more
// than this cap still get the top-kCacheRankedCap results (an explicit,
// documented ceiling rather than an unbounded cache).
constexpr int kCacheRankedCap = 500;

// Default ranking (docs/specs/20 §Ranking): exact > prefix > partial > metadata
// + recency decay + rankBoost.
class DefaultRanking final : public IRankingStrategy {
public:
    double Score(const SearchDocument& doc, const SearchQuery& query) const override {
        double score = 0.0;
        auto contains = [&](const std::string& hay) {
            std::string lh;
            for (char c : hay)
                lh.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
            return lh.contains(query.text);
        };
        if (!query.text.empty()) {
            if (contains(doc.title)) score += 3.0;
            if (contains(doc.content)) score += 1.0;
            for (const auto& t : doc.tags) score += contains(t) ? 0.75 : 0.0;
            for (const auto& [k, v] : doc.metadata) score += contains(v) ? 0.75 : 0.0;
        }
        // Recency: up to +2.0 for fresh docs (within 30 days).
        if (doc.modifiedMs > 0) {
            auto nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::system_clock::now().time_since_epoch())
                             .count();
            double ageDays = (nowMs - doc.modifiedMs) / 86400000.0;
            if (ageDays < 30.0) score += 2.0 * (1.0 - ageDays / 30.0);
        }
        score += static_cast<double>(doc.rankBoost) * 0.1;
        return score;
    }
};

// Tokenize a raw query into structured terms.
void ParseQuery(std::string_view raw, SearchQuery& out) {
    std::string cur;
    for (char c : raw) {
        if (c == ' ') {
            if (!cur.empty()) out.exactTerms.push_back(cur);
            cur.clear();
        } else {
            cur.push_back(c);
        }
    }
    if (!cur.empty()) out.exactTerms.push_back(cur);
}

std::string Lower(std::string_view s) {
    std::string out;
    for (char c : s)
        out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    return out;
}

} // namespace

SearchEngine& SearchEngine::Instance() {
    static SearchEngine instance;
    return instance;
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------
Result<void> SearchEngine::Initialize() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (initialized_.load()) return Ok();
    initialized_.store(true);
    (void)storage_.Initialize();
    if (!ranking_) ranking_ = std::make_shared<DefaultRanking>();
    WireEvents();
    return Ok();
}

Result<void> SearchEngine::Start() {
    running_.store(true);
    return Ok();
}

Result<void> SearchEngine::Stop() {
    running_.store(false);
    return Ok();
}

Result<void> SearchEngine::Shutdown() {
    if (!initialized_.load()) return Ok();
    UnwireEvents();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        adapters_.clear();
        cache_.clear();
        sessions_.clear();
        history_.clear();
        initialized_.store(false);
    }
    (void)storage_.Shutdown();
    return Ok();
}

Result<void> SearchEngine::Reload() {
    (void)ValidateIndex();
    return Ok();
}

Result<void> SearchEngine::Reset() {
    std::lock_guard<std::mutex> lock(mutex_);
    cache_.clear();
    history_.clear();
    return Ok();
}

HealthReport SearchEngine::GetHealth() const {
    HealthReport r;
    r.state = HealthState::Healthy;
    r.detail = std::format("documents={} terms={} cacheHits={}", DocumentCount(),
                           TermCount(), cacheHits_.load());
    return r;
}

Metrics SearchEngine::MetricsSnapshot() const {
    Metrics m;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        m.queueLength = cache_.size();   // live cache entries (pending results)
    }
    m.errorCount = errorCount_.load();
    m.health = HealthState::Healthy;
    return m;
}

// ---------------------------------------------------------------------------
// Adapters
// ---------------------------------------------------------------------------
Result<void> SearchEngine::RegisterAdapter(std::shared_ptr<IIndexAdapter> adapter) {
    if (!adapter) return Error::Make(Err::InvalidArgument, "SearchEngine", "null adapter");
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& a : adapters_)
        if (std::string_view(a->Type()) == adapter->Type())
            return Error::Make(Err::Search_AdapterNotFound, "SearchEngine",
                               "adapter '" + std::string(adapter->Type()) + "' already registered");
    adapters_.push_back(std::move(adapter));
    return Ok();
}

Result<void> SearchEngine::UnregisterAdapter(std::string_view type) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto it = adapters_.begin(); it != adapters_.end(); ++it) {
        if (std::string_view((*it)->Type()) == type) {
            adapters_.erase(it);
            return Ok();
        }
    }
    return Error::Make(Err::Search_AdapterNotFound, "SearchEngine",
                       "adapter '" + std::string(type) + "' not found");
}

std::vector<std::string> SearchEngine::AdapterTypes() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::string> out;
    for (const auto& a : adapters_) out.push_back(a->Type());
    return out;
}

// ---------------------------------------------------------------------------
// Indexing
// ---------------------------------------------------------------------------
Result<size_t> SearchEngine::IndexDocument(const SearchDocument& doc) {
    SearchDocument indexed = doc;
    // Content-aware extraction: ask the registered adapter for this type.
    std::shared_ptr<IIndexAdapter> adapter;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& a : adapters_)
            if (std::string_view(a->Type()) == doc.type) adapter = a;
    }
    if (adapter) {
        auto extracted = adapter->ExtractContent(doc);
        if (extracted.ok()) indexed.content = extracted.value();
    }
    size_t terms = storage_.Upsert(indexed);
    indexedDocs_.fetch_add(1);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        cache_.clear();
    }
    (void)EventBus::Instance().Publish(
        events::SearchIndexUpdated{storage_.DocumentCount(), storage_.TermCount()});
    return terms;
}

Result<void> SearchEngine::RemoveDocument(std::string_view docId) {
    auto r = storage_.Remove(docId);
    if (r.ok())
        (void)EventBus::Instance().Publish(events::SearchIndexUpdated{
            storage_.DocumentCount(), storage_.TermCount()});
    return r;
}

Result<size_t> SearchEngine::Rebuild(const std::vector<SearchDocument>& docs) {
    size_t terms = 0;
    for (const auto& d : docs) {
        auto t = IndexDocument(d);
        if (t.ok()) terms += t.value();
    }
    (void)EventBus::Instance().Publish(events::SearchIndexRebuilt{storage_.DocumentCount()});
    return terms;
}

size_t SearchEngine::DocumentCount() const {
    return storage_.DocumentCount();
}

size_t SearchEngine::TermCount() const {
    return storage_.TermCount();
}

// ---------------------------------------------------------------------------
// Query
// ---------------------------------------------------------------------------
Result<std::vector<SearchResult>> SearchEngine::RawSearch(const SearchQuery& query,
                                                          const SearchFilter& filter) const {
    std::vector<SearchResult> results;
    // Collect candidate doc ids from every term's postings.
    std::map<std::string, double, std::less<>> candidates;
    for (const auto& term : query.exactTerms) {
        for (const auto& p : storage_.Lookup(term)) {
            auto doc = storage_.GetDocument(p.docId);
            if (doc.ok()) {
                double s = ranking_ ? ranking_->Score(doc.value(), query) : 1.0;
                candidates[p.docId] += s;
            }
        }
    }
    if (candidates.empty() && query.exactTerms.empty()) {
        // Empty query: return nothing (search requires a term).
        return results;
    }

    for (const auto& [docId, score] : candidates) {
        auto docResult = storage_.GetDocument(docId);
        if (!docResult.ok()) continue;
        const auto& doc = docResult.value();
        // Filters.
        if (!filter.type.empty() && doc.type != filter.type) continue;
        if (!filter.tag.empty()) {
            bool has = std::find(doc.tags.begin(), doc.tags.end(), filter.tag) != doc.tags.end();
            if (!has) continue;
        }
        if (!filter.language.empty() && doc.language != filter.language) continue;
        if (!filter.author.empty() && doc.author != filter.author) continue;
        if (filter.modifiedAfterMs > 0 && doc.modifiedMs < filter.modifiedAfterMs) continue;
        if (filter.modifiedBeforeMs > 0 && doc.modifiedMs > filter.modifiedBeforeMs) continue;

        SearchResult r;
        r.documentId = doc.id;
        r.type = doc.type;
        r.title = doc.title;
        r.modifiedMs = doc.modifiedMs;
        r.relatedIds = doc.relatedIds;
        r.score = score;
        // Snippet: first occurrence of the query in content.
        if (!query.text.empty()) {
            auto pos = Lower(doc.content).find(Lower(query.text));
            if (pos != std::string::npos) {
                size_t start = pos > 40 ? pos - 40 : 0;
                r.snippet = doc.content.substr(start, 120);
            } else {
                r.snippet = doc.content.substr(0, 120);
            }
        }
        results.push_back(std::move(r));
    }

    std::stable_sort(results.begin(), results.end(),
                     [](const auto& a, const auto& b) { return a.score > b.score; });
    if (query.maxResults > 0 && results.size() > static_cast<size_t>(query.maxResults))
        results.resize(static_cast<size_t>(query.maxResults));
    return results;
}

Result<std::vector<SearchResult>> SearchEngine::Search(std::string_view query,
                                                       const SearchFilter& filter,
                                                       size_t limit) {
    std::string q = std::string(query);
    const auto begin = std::chrono::steady_clock::now();
    (void)EventBus::Instance().Publish(events::SearchStarted{q});
    // Cache lookup (normalized query + filter signature so that filtered
    // searches never reuse unfiltered results). Results are cached without
    // truncating to the caller's limit, so a later larger `limit` still gets
    // the full ranked set.
    const std::string filterKey = FilterSignature(filter);
    std::vector<SearchResult> results;
    bool cacheHit = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& entry : cache_) {
            if (entry.query == q && entry.filterKey == filterKey) {
                cacheHits_.fetch_add(1);
                results = entry.results;
                cacheHit = true;
                break;
            }
        }
    }
    if (!cacheHit) {
        SearchQuery sq;
        sq.text = q;
        ParseQuery(q, sq);
        sq.maxResults = kCacheRankedCap;   // cache the full ranked set
        auto raw = RawSearch(sq, filter);
        results = raw.ok() ? raw.value() : std::vector<SearchResult>{};
        std::lock_guard<std::mutex> lock(mutex_);
        history_.push_front(q);
        if (history_.size() > historyMax_) history_.pop_back();
        cache_.push_front(CacheEntry{q, filterKey, results});
        if (cache_.size() > cacheMax_) cache_.pop_back();
    }
    if (results.size() > limit) results.resize(limit);

    auto elapsed = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin).count();
    searchCount_.fetch_add(1);
    (void)EventBus::Instance().Publish(events::SearchCompleted{q, results.size(), elapsed});
    return results;
}

// ---------------------------------------------------------------------------
// Suggestions
// ---------------------------------------------------------------------------
std::vector<SearchSuggestion> SearchEngine::Suggest(std::string_view prefix, size_t limit) const {
    std::vector<SearchSuggestion> out;
    // 1. Recent history matches.
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& h : history_) {
            if (h.starts_with(prefix) && out.size() < limit) {
                out.push_back(SearchSuggestion{h, 100});
            }
        }
    }
    // 2. Index terms with the prefix.
    for (const auto& t : storage_.TermsWithPrefix(prefix, limit * 2)) {
        if (out.size() >= limit) break;
        bool dup = false;
        for (const auto& s : out)
            if (s.text == t) dup = true;
        if (!dup) out.push_back(SearchSuggestion{t, 10});
    }
    if (out.size() > limit) out.resize(limit);
    (void)EventBus::Instance().Publish(events::SearchSuggestionsUpdated{out.size()});
    return out;
}

// ---------------------------------------------------------------------------
// Ranking
// ---------------------------------------------------------------------------
Result<void> SearchEngine::SetRankingStrategy(std::shared_ptr<IRankingStrategy> strategy) {
    if (!strategy) return Error::Make(Err::InvalidArgument, "SearchEngine", "null strategy");
    std::lock_guard<std::mutex> lock(mutex_);
    ranking_ = std::move(strategy);
    return Ok();
}

// ---------------------------------------------------------------------------
// Sessions
// ---------------------------------------------------------------------------
Result<uint64_t> SearchEngine::BeginSession() {
    std::lock_guard<std::mutex> lock(mutex_);
    uint64_t id = nextSessionId_++;
    sessions_[id] = SessionState{};
    return id;
}

Result<void> SearchEngine::UpdateSession(uint64_t id, const SessionState& state) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = sessions_.find(id);
    if (it == sessions_.end())
        return Error::Make(Err::Search_InvalidQuery, "SearchEngine",
                           std::format("session {} not found", id));
    it->second = state;
    return Ok();
}

Result<SearchEngine::SessionState> SearchEngine::GetSession(uint64_t id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = sessions_.find(id);
    if (it == sessions_.end())
        return Error::Make(Err::Search_InvalidQuery, "SearchEngine",
                           std::format("session {} not found", id));
    return it->second;
}

// ---------------------------------------------------------------------------
// History
// ---------------------------------------------------------------------------
std::vector<std::string> SearchEngine::RecentQueries(size_t limit) const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::string> out;
    for (const auto& h : history_) {
        if (out.size() >= limit) break;
        out.push_back(h);
    }
    return out;
}

void SearchEngine::ClearHistory() {
    std::lock_guard<std::mutex> lock(mutex_);
    history_.clear();
}

void SearchEngine::ClearCache() {
    std::lock_guard<std::mutex> lock(mutex_);
    cache_.clear();
}

// ---------------------------------------------------------------------------
// Cache filter signature
// ---------------------------------------------------------------------------
std::string SearchEngine::FilterSignature(const SearchFilter& f) const {
    // Every filter field participates so distinct filters never share a cache
    // entry (docs/specs/20 §Caching — filtered queries must be exact).
    std::string sig;
    sig += "t=" + f.type;
    sig += "|tag=" + f.tag;
    sig += "|lang=" + f.language;
    sig += "|author=" + f.author;
    sig += std::format("|after={}", f.modifiedAfterMs);
    sig += std::format("|before={}", f.modifiedBeforeMs);
    return sig;
}

size_t SearchEngine::CacheSize() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return cache_.size();
}

// ---------------------------------------------------------------------------
// Serialization / validation
// ---------------------------------------------------------------------------
Result<std::string> SearchEngine::ExportIndex() const {
    return storage_.Snapshot();
}

Result<size_t> SearchEngine::ImportIndex(std::string_view json) {
    auto r = storage_.Restore(json);
    if (r.ok())
        (void)EventBus::Instance().Publish(
            events::SearchIndexRebuilt{storage_.DocumentCount()});
    return r;
}

Result<void> SearchEngine::ValidateIndex() {
    // Corruption-tolerant check: every posting must reference an existing doc.
    for (const auto& id : storage_.DocumentIds()) {
        if (!storage_.GetDocument(id).ok())
            return Error::Make(Err::Search_CorruptIndex, "SearchEngine",
                               "document '" + id + "' missing from store");
    }
    return Ok();
}

// ---------------------------------------------------------------------------
// Events
// ---------------------------------------------------------------------------
void SearchEngine::WireEvents() {
    auto& bus = EventBus::Instance();
    subscriptions_.push_back(bus.Subscribe<events::ContentAssetLoaded>(
        [this](const events::ContentAssetLoaded& e) { OnAssetLoaded(e); }, 0));
    subscriptions_.push_back(bus.Subscribe<events::ContentAssetDeleted>(
        [this](const events::ContentAssetDeleted& e) { OnAssetDeleted(e); }, 0));
    subscriptions_.push_back(bus.Subscribe<events::ConfigHotReload>(
        [this](const events::ConfigHotReload& e) { OnConfigReload(e); }, 0));
}

void SearchEngine::UnwireEvents() {
    auto& bus = EventBus::Instance();
    for (auto& s : subscriptions_)
        if (s.Valid()) (void)bus.Unsubscribe(s);
    subscriptions_.clear();
}

void SearchEngine::OnAssetLoaded(const events::ContentAssetLoaded& e) {
    // Index assets automatically (metadata + name) — content-aware adapters
    // enrich the body.
    SearchDocument doc;
    doc.id = e.uuid;
    doc.type = "asset";
    doc.title = e.name;
    doc.source = e.name;
    (void)IndexDocument(doc);
}

void SearchEngine::OnAssetDeleted(const events::ContentAssetDeleted& e) {
    (void)RemoveDocument(e.uuid);
}

void SearchEngine::OnConfigReload(const events::ConfigHotReload&) {
    (void)ValidateIndex();
}

} // namespace bps::search
