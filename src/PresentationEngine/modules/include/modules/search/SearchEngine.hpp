#pragma once

// SearchEngine (docs/specs/20): the Phase 9 facade. Owns the index storage, the
// adapter registry, the query engine, the ranking strategy, the cache, sessions,
// history and suggestions. Every module asks it "find me the information" —
// nobody builds their own search.

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

    // --- Indexing (full + incremental, background-capable) ---
    // Indexes (or re-indexes) one document. Content is extracted through the
    // registered adapter (content-aware), falling back to the document body.
    Result<size_t> IndexDocument(const SearchDocument& doc);
    Result<void> RemoveDocument(std::string_view docId);
    // Rebuilds the whole index from a set of documents (called on a pool
    // worker in production; synchronous here so tests are deterministic).
    Result<size_t> Rebuild(const std::vector<SearchDocument>& docs);
    // Batch scope for bulk indexing (a boot-time library index): while suspended,
    // per-document cache wipes are skipped and the OUTERMOST resume clears once —
    // a 1200-sermon index invalidates the query cache once, not 1200 times.
    // Nestable (a depth counter); Rebuild uses this internally.
    void SuspendCacheInvalidation();
    void ResumeCacheInvalidation();
    size_t DocumentCount() const;
    size_t TermCount() const;

    // --- Query --------------------------------------------------------------
    // Parses the raw query into structured terms (exact/prefix/fuzzy/phrase/
    // metadata) and executes it with ranking + filters.
    Result<std::vector<SearchResult>> Search(std::string_view query,
                                             const SearchFilter& filter = {},
                                             size_t limit = 50);

    // --- Suggestions ----------------------------------------------------------
    std::vector<SearchSuggestion> Suggest(std::string_view prefix, size_t limit = 10) const;

    // --- Ranking strategy (replaceable) ----------------------------------------
    Result<void> SetRankingStrategy(std::shared_ptr<IRankingStrategy> strategy);
    Result<std::vector<SearchResult>> RawSearch(const SearchQuery& query,
                                                const SearchFilter& filter = {}) const;

    // --- Fuzzy resolution -------------------------------------------------------
    // Vocabulary resolution of query words: typos corrected ("thn" -> "then",
    // weight 0.7 so exact spellings outrank corrections) and incomplete words
    // completed ("friend" -> "friends", weight 1.0 — a stem, not an error).
    // Words already in the vocabulary come back as typed. One term in, one out,
    // IN ORDER — exposed so callers that run their own text scan (The Table's
    // paragraph search) look for the SAME corrected words the index ranking
    // uses, instead of literally hunting the user's misspelling.
    struct ResolvedQueryTerm {
        std::string term;      // the word to search for (the input when unresolved)
        double weight = 1.0;   // its BM25 contribution weight
        // THE CANDIDATE LADDER of a half-typed word (only filled for the tail a
        // caller marked as still being typed): the words the typed prefix can
        // become, MOST CONFIDENT FIRST — confidence being how often the corpus
        // uses the word, the one honest signal the index has for "what the user
        // most likely meant" ("rec" -> receive, received, record, ...). Empty
        // for every other term. Callers that run their own text scan walk it in
        // order, so their results start with the likeliest reading and then, as
        // every earlier candidate runs out, reach the merely possible ones
        // (user call: "start from the ones it confident about then come as far
        // as possible options exist").
        std::vector<std::string> candidates;
    };
    Result<std::vector<ResolvedQueryTerm>> ResolveTerms(
        const std::vector<std::string>& terms) const;

    // ResolveTerms PLUS jam-splitting: a long word the vocabulary does not know
    // may be two words typed without the space ("holyspirit" -> "holy spirit",
    // "holyghost" -> "holy ghost") — the pair of real words that reassembles it
    // wins. A split arrives as ONE entry containing a space ("holy spirit") so
    // the 1:1 term mapping holds; callers split on the space if they need the
    // words separately.
    //
    // `typingTail` = the user is STILL TYPING the last word (every live search
    // box: the sermon pane's, the Scripture pane's, the app-wide search). Then a
    // half-typed word is never rewritten into one guessed word — "compelling
    // that you rec" must not become "... received" and so lose the paragraph
    // that says "receive" (user report, with the pane screenshot). The typed
    // prefix is kept and MATCHES EVERY WORD THAT STARTS WITH IT, while the words
    // before it still have to be there; a fully-typed MISSPELLING ("recieve") is
    // still corrected, because that is not a prefix of anything the user meant.
    // Leave it false for a hint/autocomplete answer ("rec -> receive" reads
    // better as the completion than as the raw prefix).
    Result<std::vector<ResolvedQueryTerm>> ResolveTermsWithSplits(
        const std::vector<std::string>& terms, bool typingTail = false) const;

    // The candidate ladder itself (see ResolvedQueryTerm::candidates): every
    // vocabulary word that STARTS WITH `prefix`, most used first, capped at
    // `limit`. Public so EVERY search surface can rank by the same order — the
    // sermon pane, the Scripture pane, the app-wide dialog and the index path
    // all reach the same list through this one call.
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
