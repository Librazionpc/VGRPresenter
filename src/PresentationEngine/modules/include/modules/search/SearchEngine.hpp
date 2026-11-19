#pragma once

#include "core/common/Common.hpp"
#include "core/events/EventBus.hpp"
#include "core/events/Events.hpp"
#include "interfaces/IService.hpp"
#include "modules/search/ISearchProvider.hpp"
#include "modules/search/IndexStorage.hpp"
#include "modules/search/SearchTypes.hpp"

#include <atomic>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace bps::search {
class SearchEngine final : public IService {
public:
    static SearchEngine& Instance();

    // --- Lifecycle (engine-wide contract) ---
    Result<void> Initialize() override;
    Result<void> Start() override;
    Result<void> Stop() override;
    Result<void> Shutdown() override;
    Result<void> Reload() override;
    Result<void> Reset() override;
    HealthReport GetHealth() const override;
    Metrics MetricsSnapshot() const override;
    const char* ServiceName() const noexcept override { return "SearchEngine"; }

    // --- Adapter registry (IIndexAdapter per content type) ---
    Result<void> RegisterAdapter(std::shared_ptr<IIndexAdapter> adapter);
    Result<void> UnregisterAdapter(std::string_view type);
    std::vector<std::string> AdapterTypes() const;

    Result<size_t> IndexDocument(const SearchDocument& doc);
    Result<void> RemoveDocument(std::string_view docId);
    // Rebuilds the whole index from a set of documents (called on a pool
    // worker in production; synchronous here so tests are deterministic).
    Result<size_t> Rebuild(const std::vector<SearchDocument>& docs);
    // A word list for a source whose documents are NOT indexed (the sermon library): typo correction, word splitting
    // and completion see its words and their document counts, at a few MB instead of hundreds. See IndexStorage.
    void SetVocabulary(std::string_view source, std::vector<std::pair<std::string, uint32_t>> terms);
    void ClearVocabulary(std::string_view source);
    void SuspendCacheInvalidation();
    void ResumeCacheInvalidation();
    size_t DocumentCount() const;
    size_t TermCount() const;

    Result<std::vector<SearchResult>> Search(std::string_view query,
                                             const SearchFilter& filter = {},
                                             size_t limit = 50);

    // --- Suggestions ----------------------------------------------------------
    std::vector<SearchSuggestion> Suggest(std::string_view prefix, size_t limit = 10) const;

    // --- Ranking strategy (replaceable) ----------------------------------------
    Result<void> SetRankingStrategy(std::shared_ptr<IRankingStrategy> strategy);
    Result<std::vector<SearchResult>> RawSearch(const SearchQuery& query,
                                                const SearchFilter& filter = {}) const;

    struct ResolvedQueryTerm {
        std::string term;      // the word to search for (the input when unresolved)
        double weight = 1.0;   // its BM25 contribution weight
        std::vector<std::string> candidates;
    };
    Result<std::vector<ResolvedQueryTerm>> ResolveTerms(
        const std::vector<std::string>& terms) const;

    Result<std::vector<ResolvedQueryTerm>> ResolveTermsWithSplits(
        const std::vector<std::string>& terms, bool typingTail = false) const;

    std::vector<std::string> PrefixCandidates(std::string_view prefix, size_t limit = 12) const;

    // --- Sessions (restorable query state) ---------------------------------------
    struct SessionState {
        std::string query;
        SearchFilter filter;
        size_t offset = 0;
        size_t limit = 50;
    };
    Result<uint64_t> BeginSession();
    Result<void> UpdateSession(uint64_t id, const SessionState& state);
    Result<SessionState> GetSession(uint64_t id) const;

    // --- History -----------------------------------------------------------------
    std::vector<std::string> RecentQueries(size_t limit = 10) const;
    void ClearHistory();

    // --- Cache --------------------------------------------------------------------
    size_t CacheHits() const { return cacheHits_.load(); }
    void ClearCache();
    size_t CacheSize() const;

    // --- Serialization / validation -------------------------------------------------
    Result<std::string> ExportIndex() const;             // JSON snapshot
    Result<size_t> ImportIndex(std::string_view json);
    Result<void> ValidateIndex();                        // health check

    // --- Events --------------------------------------------------------------------
    void WireEvents();
    void UnwireEvents();
    void OnAssetLoaded(const events::ContentAssetLoaded& e);
    void OnAssetDeleted(const events::ContentAssetDeleted& e);
    void OnConfigReload(const events::ConfigHotReload& e);

private:
    // Canonical signature of a filter for the query cache.
    std::string FilterSignature(const SearchFilter& f) const;
    SearchEngine() = default;

    mutable std::mutex mutex_;
    IndexStorage storage_;
    std::vector<std::shared_ptr<IIndexAdapter>> adapters_;
    std::shared_ptr<IRankingStrategy> ranking_;
    // Query cache: normalized query + filter signature -> results (LRU).
    struct CacheEntry {
        std::string query;
        std::string filterKey;
        std::vector<SearchResult> results;
    };
    std::deque<CacheEntry> cache_;
    size_t cacheMax_ = 128;
    size_t scanBudget_ = 4000;   // cap on the no-index fallback full scan (short/1-char queries)
    int cacheSuspend_ = 0;   // >0 while inside a SuspendCacheInvalidation batch scope
    std::map<uint64_t, SessionState, std::less<>> sessions_;
    std::deque<std::string> history_;
    size_t historyMax_ = 50;
    uint64_t nextSessionId_ = 1;
    std::vector<Subscription> subscriptions_;
    std::atomic<bool> initialized_{false};
    std::atomic<bool> running_{false};
    std::atomic<uint64_t> errorCount_{0};
    std::atomic<uint64_t> searchCount_{0};
    std::atomic<uint64_t> cacheHits_{0};
    std::atomic<uint64_t> indexedDocs_{0};
};
} // namespace bps::search