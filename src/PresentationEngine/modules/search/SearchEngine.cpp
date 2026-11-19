#include "modules/search/SearchEngine.hpp"

#include "core/logging/Logger.hpp"
#include "modules/search/TextMatching.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <format>
#include <sstream>
#include <unordered_map>

namespace bps::search {
namespace {
using textmatch::ContainsWord;
using textmatch::ContainsPhrase;
using textmatch::EditDistanceBounded;

constexpr int kCacheRankedCap = 500;

constexpr size_t kPartialCap = 2500;

std::string Lower(std::string_view s);

// Default ranking (docs/specs/20 §Ranking): exact > prefix > partial > metadata
// + recency decay + rankBoost.
class DefaultRanking final : public IRankingStrategy {
public:
    bool IsDefault() const override { return true; }

    double Score(const SearchDocument& doc, const SearchQuery& query) const override {
        double score = 0.0;
        const std::string needle = Lower(query.text);
        auto contains = [&](const std::string& hay) {
            return Lower(hay).find(needle) != std::string::npos;
        };
        if (!needle.empty()) {
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

void ParseQuery(std::string_view raw, SearchQuery& out) {
    std::string cur;
    for (char c : raw) {
        const unsigned char u = static_cast<unsigned char>(c);
        if (std::isalnum(u) || u >= 0x80) {
            cur.push_back(static_cast<char>(std::tolower(u)));
        } else if (!cur.empty()) {
            out.exactTerms.push_back(cur);
            cur.clear();
        }
    }
    if (!cur.empty()) out.exactTerms.push_back(cur);
}

bool CandidateWorthy(std::string_view term) { return term.size() >= 2; }

std::pair<std::string, std::string> SplitJam(const std::string& term,
                                             const IndexStorage& storage) {
    if (term.size() < 6) return {};   // "inon" can't hide two real words
    std::pair<std::string, std::string> best;
    size_t bestDf = 0;
    for (size_t cut = 3; cut + 3 <= term.size(); ++cut) {
        const std::string a = term.substr(0, cut);
        const std::string b = term.substr(cut);
        // DfOf (size only) — LookupTf here copied BOTH halves' full postings
        // per split point; a long jam word meant dozens of multi-MB copies.
        const size_t dfA = storage.DfOf(a);
        if (dfA == 0) continue;
        const size_t dfB = storage.DfOf(b);
        if (dfB == 0) continue;
        const size_t m = std::min(dfA, dfB);
        if (m > bestDf) { bestDf = m; best = {a, b}; }
    }
    return best;
}

struct ResolvedTerm { std::string word; double weight = 1.0; };
constexpr size_t kRareTermMaxDf = 100;
ResolvedTerm ResolveTermInVocabulary(const std::string& term,
                                     const IndexStorage& storage) {
    ResolvedTerm out;
    const size_t ownDf = storage.DfOf(term);
    if (ownDf > kRareTermMaxDf) return out;   // an established word: as typed
    const size_t maxDist = term.size() <= 4 ? 1 : 2;
    const size_t minCorrDf = ownDf > 0 ? std::max<size_t>(20, ownDf * 20) : 0;
    if (ownDf > 0) {
        std::string best;
        size_t bestDist = maxDist + 1, bestDf = 0;
        storage.ForEachTermWithDf([&](const std::string& w, size_t wdf) {
            if (w.size() < 3 || w[0] != term[0]) return true;
            if (w.size() + 1 < term.size() || w.size() > term.size() + 1) return true;
            if (wdf < minCorrDf) return true;
            const size_t d = textmatch::IsAdjacentTransposition(term, w)
                                 ? 1 : EditDistanceBounded(term, w, bestDist);
            if (d <= maxDist && (d < bestDist || (d == bestDist && wdf > bestDf))) {
                best = w; bestDist = d; bestDf = wdf;
            }
            return true;
        });
        if (!best.empty()) { out.word = best; out.weight = 0.7; return out; }
    }
    // 1) Completion: the most frequent vocabulary word this term prefixes
    //    ("friend" -> "friends"). Frequency picks between friends/friend's/...
    size_t bestDf = 0;
    for (const auto& w : storage.TermsWithPrefix(term, 8)) {
        const size_t df = storage.DfOf(w);
        if (df > bestDf) { bestDf = df; out.word = w; }
    }
    if (bestDf > 0) return out;   // a stem, not a typo: full weight
    std::string best;
    size_t bestDist = maxDist + 1, bestDf2 = 0;
    storage.ForEachTermWithDf([&](const std::string& w, size_t wdf) {
        if (w.size() < 3 || w[0] != term[0]) return true;
        if (w.size() + maxDist < term.size() || w.size() > term.size() + maxDist) return true;
        if (wdf < minCorrDf) return true;
        if (ownDf > 0 && (w.size() + 1 < term.size() || w.size() > term.size() + 1)) return true;
        const size_t d = textmatch::IsAdjacentTransposition(term, w)
                             ? 1 : EditDistanceBounded(term, w, bestDist);
        if (d <= maxDist && (d < bestDist || (d == bestDist && wdf > bestDf2))) {
            best = w; bestDist = d; bestDf2 = wdf;
        }
        return true;
    });
    if (!best.empty()) { out.word = best; out.weight = 0.7; }
    return out;
}

size_t DensestWindowPos(const std::string& content, const std::vector<std::string>& words,
                        size_t window = 160) {
    struct Hit { size_t pos; unsigned term; };
    std::vector<Hit> hits;
    for (unsigned k = 0; k < words.size() && k < 64; ++k) {
        const std::string& t = words[k];
        if (t.empty()) continue;
        size_t guard = 0;
        for (size_t p = content.find(t); p != std::string::npos && guard++ < 200;
             p = content.find(t, p + t.size()))
            hits.push_back({p, k});
    }
    if (hits.empty()) return std::string::npos;
    std::sort(hits.begin(), hits.end(),
              [](const Hit& a, const Hit& b) { return a.pos < b.pos; });
    size_t counts[64] = {};
    size_t lo = 0, distinct = 0;
    size_t bestLo = 0, bestDistinct = 0, bestCount = 0;
    for (size_t hi = 0; hi < hits.size(); ++hi) {
        if (counts[hits[hi].term]++ == 0) ++distinct;
        while (hits[hi].pos - hits[lo].pos > window) {
            if (--counts[hits[lo].term] == 0) --distinct;
            ++lo;
        }
        const size_t count = hi - lo + 1;
        if (distinct > bestDistinct || (distinct == bestDistinct && count > bestCount)) {
            bestDistinct = distinct; bestCount = count; bestLo = lo;
        }
    }
    return hits[bestLo].pos;
}

bool NearestCoveringSpan(const std::string& content, const std::vector<std::string>& words,
                         size_t& startPos, size_t& span) {
    struct Hit { size_t pos; unsigned term; };
    std::vector<Hit> hits;
    for (unsigned k = 0; k < words.size() && k < 64; ++k) {
        const std::string& t = words[k];
        if (t.empty()) return false;
        size_t guard = 0;
        size_t p = content.find(t);
        if (p == std::string::npos) return false;   // a word missing: no proximity at all
        for (; p != std::string::npos && guard++ < 32; p = content.find(t, p + t.size()))
            hits.push_back({p, k});
    }
    if (words.size() == 1) {
        startPos = hits.front().pos;
        span = 0;
        return true;
    }
    std::sort(hits.begin(), hits.end(),
              [](const Hit& a, const Hit& b) { return a.pos < b.pos; });
    size_t counts[64] = {};
    size_t lo = 0, distinct = 0;
    const size_t need = words.size() > 64 ? 64 : words.size();
    bool found = false;
    for (size_t hi = 0; hi < hits.size(); ++hi) {
        if (counts[hits[hi].term]++ == 0) ++distinct;
        while (distinct == need) {
            const size_t s = hits[hi].pos - hits[lo].pos;
            if (!found || s < span) { span = s; startPos = hits[lo].pos; found = true; }
            if (--counts[hits[lo].term] == 0) --distinct;
            ++lo;
        }
    }
    return found;
}

std::string Lower(std::string_view s) {
    std::string out;
    out.reserve(s.size());
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
        if (cacheSuspend_ == 0)
            cache_.clear();
    }
    (void)EventBus::Instance().Publish(
        events::SearchIndexUpdated{storage_.DocumentCount(), storage_.TermCount()});
    return terms;
}

// Batch scope: while suspended, per-document cache wipes are skipped; the
// OUTERMOST resume clears once. RAII-safe via explicit Begin/End pairs.
void SearchEngine::SetVocabulary(std::string_view source, std::vector<std::pair<std::string, uint32_t>> terms) {
    storage_.SetVocabulary(source, std::move(terms));
}
void SearchEngine::ClearVocabulary(std::string_view source) {
    storage_.ClearVocabulary(source);
}

void SearchEngine::SuspendCacheInvalidation() {
    std::lock_guard<std::mutex> lock(mutex_);
    ++cacheSuspend_;
}
void SearchEngine::ResumeCacheInvalidation() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (cacheSuspend_ > 0 && --cacheSuspend_ == 0)
        cache_.clear();
}

Result<void> SearchEngine::RemoveDocument(std::string_view docId) {
    auto r = storage_.Remove(docId);
    if (r.ok())
        (void)EventBus::Instance().Publish(events::SearchIndexUpdated{
            storage_.DocumentCount(), storage_.TermCount()});
    return r;
}

Result<size_t> SearchEngine::Rebuild(const std::vector<SearchDocument>& docs) {
    // ONE cache clear for the whole rebuild, not one per document.
    SuspendCacheInvalidation();
    size_t terms = 0;
    for (const auto& d : docs) {
        auto t = IndexDocument(d);
        if (t.ok()) terms += t.value();
    }
    ResumeCacheInvalidation();
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
Result<std::vector<SearchEngine::ResolvedQueryTerm>> SearchEngine::ResolveTerms(
    const std::vector<std::string>& terms) const {
    std::vector<ResolvedQueryTerm> out;
    out.reserve(terms.size());
    for (const std::string& t : terms) {
        if (t.empty()) continue;
        ResolvedTerm r = ResolveTermInVocabulary(Lower(t), storage_);
        out.push_back({r.word.empty() ? Lower(t) : r.word, r.weight, {}});
    }
    return out;
}

//   1. spell-check + completion   ("thn"->"then" w=0.7, "friend"->"friends" w=1.0)
//   2. jam-split, last resort     ("holyspirit"->"holy spirit" w=1.0 — a space
ResolvedTerm ResolveOne(const std::string& raw, const IndexStorage& storage,
                        bool typingTail = false) {
    const std::string typed = Lower(raw);
    ResolvedTerm r = ResolveTermInVocabulary(typed, storage);
    if (r.word.empty()) r.word = typed;
    if (r.weight == 1.0 && r.word.size() >= 6 && storage.DfOf(r.word) == 0) {
        if (auto [a, b] = SplitJam(r.word, storage); !a.empty()) r.word = a + ' ' + b;
    }
    if (typingTail && r.weight == 1.0 && r.word.size() > typed.size()
        && r.word.compare(0, typed.size(), typed) == 0)
        r.word = typed;
    return r;
}

std::vector<std::string> SearchEngine::PrefixCandidates(std::string_view prefix,
                                                        size_t limit) const {
    std::vector<std::string> words;
    if (prefix.empty()) return words;
    for (const auto& w : storage_.TermsWithPrefix(std::string(prefix), limit * 4)) {
        if (w.size() < prefix.size() || w.compare(0, prefix.size(), prefix) != 0) continue;
        if (w == prefix) continue;   // the typed word itself is not a candidate
        words.push_back(w);
    }
    std::stable_sort(words.begin(), words.end(), [&](const std::string& a, const std::string& b) {
        return storage_.DfOf(a) > storage_.DfOf(b);
    });
    if (words.size() > limit) words.resize(limit);
    return words;
}

Result<std::vector<SearchEngine::ResolvedQueryTerm>> SearchEngine::ResolveTermsWithSplits(
    const std::vector<std::string>& terms, bool typingTail) const {
    std::vector<ResolvedQueryTerm> out;
    out.reserve(terms.size());
    // Only the LAST given word can still be under the caret (see the header).
    size_t last = 0;
    for (size_t i = 0; i < terms.size(); ++i)
        if (!terms[i].empty()) last = i;
    for (size_t i = 0; i < terms.size(); ++i) {
        const std::string& t = terms[i];
        if (t.empty()) continue;
        const bool tail = typingTail && i == last;
        const ResolvedTerm r = ResolveOne(t, storage_, tail);
        ResolvedQueryTerm entry{r.word, r.weight, {}};
        // A half-typed word whose TYPO reading was not applied is a prefix: hand
        // the caller the ladder of what it can become, most likely first.
        if (tail && r.weight == 1.0 && !r.word.empty() && r.word == Lower(t))
            entry.candidates = PrefixCandidates(r.word);
        out.push_back(std::move(entry));
    }
    return out;
}

Result<std::vector<SearchResult>> SearchEngine::RawSearch(const SearchQuery& query,
                                                          const SearchFilter& filter) const {
    std::vector<SearchResult> results;
    struct TermClass { uint64_t whole = 0, partial = 0; };
    std::map<std::string, double, std::less<>> candidates;
    std::unordered_map<std::string, TermClass> masks;
    std::vector<std::string> terms;
    std::vector<double> termWeight;
    size_t tailTerm = static_cast<size_t>(-1);
    bool tailIsPartial = false;
    {
        size_t tailIndex = 0;
        for (size_t i = 0; i < query.exactTerms.size(); ++i)
            if (!query.exactTerms[i].empty()) tailIndex = i;
        for (size_t i = 0; i < query.exactTerms.size(); ++i) {
            const auto& raw = query.exactTerms[i];
            if (raw.empty()) continue;
            const ResolvedTerm r = CandidateWorthy(raw) ? ResolveOne(raw, storage_, i == tailIndex)
                                                        : ResolvedTerm{Lower(raw), 1.0};
            if (i == tailIndex)
                tailIsPartial = storage_.DfOf(Lower(raw)) == 0;
            std::istringstream in(r.word);
            std::string part;
            while (in >> part) {
                if (std::find(terms.begin(), terms.end(), part) != terms.end()) continue;
                if (i == tailIndex) tailTerm = terms.size();   // the half-typed word's term
                terms.push_back(part);
                termWeight.push_back(r.weight);
            }
        }
    }
    double avgdl = 1.0;
    size_t N = 0;   // (corpus; stats per term in termStats below)
    {
        const auto corpus = storage_.Corpus();
        N = corpus.documents;
        if (corpus.documents > 0 && corpus.totalTokens > 0)
            avgdl = static_cast<double>(corpus.totalTokens) / static_cast<double>(corpus.documents);
    }
    struct TermStats { size_t df; std::unordered_map<std::string, size_t> tf; };
    std::unordered_map<std::string, TermStats> termStats;   // term -> (df, per-doc tf)
    std::vector<std::vector<std::string>> prefixAlts(terms.size());
    bool anyCandidateTerm = false;
    size_t corpusDocs = N;
    constexpr double kStopWordDfShare = 0.6;
    constexpr size_t kStopWordDfFloor = 4096;
    {
        for (const auto& term : terms) {
            if (!CandidateWorthy(term)) continue;
            const size_t df = storage_.DfOf(term);
            if (df == 0) continue;
            if (df > kStopWordDfFloor && corpusDocs > 0
                && static_cast<double>(df) > kStopWordDfShare * static_cast<double>(corpusDocs))
                continue;   // a stop word: contributes no candidates of its own
            anyCandidateTerm = true;
            const auto tfs = storage_.LookupTf(term);   // (bounded df: a bounded copy)
            TermStats& ts = termStats[term];
            ts.df = tfs.size();
            for (const auto& [docId, tf] : tfs) {
                candidates[docId] += 0.0;   // register the candidate
                ts.tf[docId] = tf;
            }
        }
    }
    {
        uint64_t ti = 0;
        for (const auto& term : terms) {
            const uint64_t bit = ti < 64 ? (1ull << ti) : 0;
            ++ti;
            if (!CandidateWorthy(term)) continue;
            for (const auto& docId : storage_.LookupDocIds(term)) {
                if (bit) masks[docId].whole |= bit;
            }
            if (!bit) continue;
            const auto siblings = storage_.TermsWithPrefix(term, 40);
            std::vector<std::pair<size_t, const std::string*>> byDf;
            byDf.reserve(siblings.size());
            for (const auto& pt : siblings) {
                if (pt == term) continue;
                byDf.emplace_back(storage_.DfOf(pt), &pt);   // size only — no postings copy per sibling
            }
            const size_t take = std::min<size_t>(byDf.size(), 12);
            std::partial_sort(byDf.begin(), byDf.begin() + static_cast<long>(take), byDf.end(),
                              [](const auto& a, const auto& b) { return a.first > b.first; });
            if (ti - 1 < prefixAlts.size())
                for (size_t e = 0; e < take; ++e) prefixAlts[ti - 1].push_back(*byDf[e].second);
            for (size_t e = 0; e < take; ++e) {
                for (const auto& docId : storage_.LookupDocIds(*byDf[e].second)) {
                    if (candidates.size() >= kPartialCap && candidates.find(docId) == candidates.end())
                        continue;   // cap reached: only classify docs already in
                    candidates[docId] += 0.0;
                    masks[docId].partial |= bit;
                }
            }
        }
    }
    if (!anyCandidateTerm) {
        const bool anyWorthy = std::any_of(terms.begin(), terms.end(),
                                           [](const std::string& t) { return CandidateWorthy(t); });
        if (!anyWorthy) {
            size_t budget = 0;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                budget = scanBudget_;
            }
            for (const auto& id : storage_.DocumentIds()) {
                if (candidates.size() >= budget) break;
                candidates[id] += 0.0;
            }
        }
    }

    const auto landedAll = [&](const std::string& id) {
        const auto mIt = masks.find(id);
        for (size_t k = 0; k < terms.size() && k < 64; ++k) {
            if (!CandidateWorthy(terms[k])) continue;
            if (mIt == masks.end()) return false;
            if ((mIt->second.whole | mIt->second.partial) & (1ull << k)) continue;
            return false;
        }
        return true;
    };

    std::vector<std::string> ids;
    ids.reserve(candidates.size());
    for (const auto& [id, _] : candidates) ids.push_back(id);
    std::vector<DocRecord> docs;
    {
        const size_t kFetchCap = 48;   // rows that can ever display (maxResults) + headroom
        if (candidates.size() <= kFetchCap) {
            docs = storage_.DocumentsById(ids);
        } else {
            const auto metas = storage_.DocMetas(ids);
            double best = 0.0;
            std::vector<std::pair<double, const std::string*>> prescored;
            prescored.reserve(ids.size());
            for (const auto& id : ids) {
                if (!landedAll(id)) continue;
                if (!filter.type.empty()) {
                    const auto mIt = metas.find(id);
                    if (mIt != metas.end() && mIt->second.type != filter.type) continue;
                }
                if (!filter.idPrefix.empty() && id.compare(0, filter.idPrefix.size(), filter.idPrefix) != 0)
                    continue;
                const TermClass& mc = masks[id];
                double p = 0.0;
                for (size_t k = 0; k < terms.size() && k < 64; ++k) {
                    const auto ts = termStats.find(terms[k]);
                    if (ts == termStats.end()) continue;
                    const size_t tf = [&] {
                        const auto it = ts->second.tf.find(id);
                        return it != ts->second.tf.end() ? it->second : size_t(0);
                    }();
                    if (tf == 0 && !(mc.partial & (1ull << k))) continue;
                    const double w = k < termWeight.size() ? termWeight[k] : 1.0;
                    if (mc.whole & (1ull << k)) p += w * 2.0;
                    if (mc.partial & (1ull << k)) p += w * 0.8;
                    p += w * 0.001;   // registered by this term at all
                }
                // Length tiebreak: shorter docs (a paragraph-like sermon) first.
                const auto mIt2 = metas.find(id);
                if (mIt2 != metas.end() && mIt2->second.tokens > 0)
                    p /= std::sqrt(static_cast<double>(mIt2->second.tokens));
                if (p <= 0.0) continue;   // masks empty: below every display path
                prescored.emplace_back(p, &id);
                if (p > best) best = p;
            }
            std::partial_sort(prescored.begin(),
                              prescored.begin() + std::min(kFetchCap, prescored.size()),
                              prescored.end(),
                              [](const auto& a, const auto& b) { return a.first > b.first; });
            std::vector<std::string> top;
            top.reserve(std::min(kFetchCap, prescored.size()));
            for (size_t i = 0; i < prescored.size() && i < kFetchCap; ++i)
                top.push_back(*prescored[i].second);
            docs = storage_.DocumentsById(top);
        }
    }

    const std::string needle = Lower(query.text);
    std::string phrase;
    for (const auto& t : terms) {
        if (!phrase.empty()) phrase += ' ';
        phrase += t;
    }
    const bool phraseWorth = phrase.find(' ') != std::string::npos;
    const std::vector<std::string> phraseTerms = terms;   // (user order; the phrase helper reads it)
    size_t countable = 0;
    for (const auto& term : terms)
        if (CandidateWorthy(term)) ++countable;
    const bool fastRank = !ranking_ || ranking_->IsDefault();

    const std::vector<std::string> tailLadder =
        (tailIsPartial && tailTerm < terms.size()) ? PrefixCandidates(terms[tailTerm])
                                                  : std::vector<std::string>{};

    std::unordered_map<std::string, size_t> snippetPositions;
    auto clip = [](const std::string& src, size_t pos) {
        const std::string seps = " \t\n.,;:!?\"()-";
        std::string snip = src.substr(pos, 120);
        if (!snip.empty() && seps.find(snip.back()) == std::string::npos &&
            pos + snip.length() < src.size() &&
            seps.find(src[pos + snip.length()]) == std::string::npos) {
            const size_t lastSep = snip.find_last_of(seps);
            if (lastSep != std::string::npos && lastSep > 80) snip.resize(lastSep);
        }
        // Trim a leading partial word the same way.
        if (pos > 0) {
            const size_t firstSep = snip.find_first_of(seps);
            if (firstSep != std::string::npos && firstSep < 30)
                snip = snip.substr(firstSep + 1);
        }
        return snip;
    };

    for (const auto& rec : docs) {
        const SearchDocument& doc = rec.doc;
        if (!landedAll(doc.id)) continue;   // every typed word must be here (see above)
        size_t snippetPos = std::string::npos;
        double& score = candidates[doc.id];
        if (fastRank) {
            constexpr double kK1 = 1.2, kB = 0.75;
            double s = 0.0;
            size_t wholeWords = 0;
            double tailTier = 0.0;
            const TermClass tc = masks.count(doc.id) ? masks.at(doc.id) : TermClass{};
            for (size_t k = 0; k < terms.size() && k < 64; ++k) {
                const std::string& term = terms[k];
                if (!CandidateWorthy(term)) {
                    // Below the index floor ("a", "I"): cheap substring credit so
                    // degenerate queries still rank by something real.
                    if (rec.lowerContent.find(term) != std::string::npos) s += 1.0;
                    if (rec.lowerTitle.find(term) != std::string::npos) s += 1.0;
                    continue;
                }
                if (tc.whole & (1ull << k)) ++wholeWords;
                const auto ts = termStats.find(term);
                if (ts == termStats.end()) continue;
                const auto tfIt = ts->second.tf.find(doc.id);
                if (tfIt == ts->second.tf.end()) {
                    continue;
                }
                const double idf = std::log(1.0 + (static_cast<double>(N) - static_cast<double>(ts->second.df) + 0.5)
                                                       / (static_cast<double>(ts->second.df) + 0.5));
                const double tfd = static_cast<double>(tfIt->second);
                s += termWeight[k] * idf * (tfd * (kK1 + 1.0))
                   / (tfd + kK1 * (1.0 - kB + kB * static_cast<double>(rec.tokenCount) / avgdl));
            }
            for (size_t k = 0; k < terms.size() && k < 64; ++k) {
                const std::string& term = terms[k];
                if (!CandidateWorthy(term)) continue;
                if ((tc.whole & (1ull << k)) && ContainsWord(rec.lowerTitle, term)) {
                    const auto ts = termStats.find(term);
                    const double idf = ts != termStats.end()
                        ? std::log(1.0 + (static_cast<double>(N) - static_cast<double>(ts->second.df) + 0.5)
                                           / (static_cast<double>(ts->second.df) + 0.5))
                        : 1.0;
                    s += termWeight[k] * 2.0 * std::max(1.0, idf);
                }
            }
            // Every word present as a whole word: the query really is here,
            // above any doc that merely contains some words somewhere.
            if (countable > 0 && wholeWords == countable) s += 6.0;
            if (!tailLadder.empty()) {
                if (ContainsWord(rec.lowerContent, terms[tailTerm]))
                    tailTier = 100.0;
                else
                    for (size_t rank = 0; rank < tailLadder.size(); ++rank)
                        if (ContainsWord(rec.lowerContent, tailLadder[rank])) {
                            tailTier = 100.0 / (1.0 + static_cast<double>(rank));
                            break;
                        }
                s += tailTier;
            }
            size_t pos = std::string::npos;
            if (phraseWorth && ContainsPhrase(rec.lowerContent, phraseTerms, &pos)) {
                s += 20.0 + phrase.size() * 0.02;
                snippetPos = pos > 40 ? pos - 40 : 0;
                snippetPositions[doc.id] = snippetPos;   // cheap anchor: reused below
            } else if (phraseWorth) {
                std::vector<std::string> worthy;
                for (const auto& term : phraseTerms)
                    if (CandidateWorthy(term)) worthy.push_back(term);
                size_t clusterStart = 0, clusterSpan = 0;
                const size_t window = 40 * 6;   // ~40 words at ~6 chars each
                if (NearestCoveringSpan(rec.lowerContent, worthy, clusterStart, clusterSpan)
                    && clusterSpan <= window) {
                    const double spread = static_cast<double>(clusterSpan) / static_cast<double>(window);
                    s += 8.0 * (1.0 - spread);   // tight cluster ~ +8, window-edge ~ 0
                    snippetPos = clusterStart > 40 ? clusterStart - 40 : 0;
                    snippetPositions[doc.id] = snippetPos;
                }
            }
            if (doc.modifiedMs > 0) {
                auto nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                                 std::chrono::system_clock::now().time_since_epoch())
                                 .count();
                double ageDays = (nowMs - doc.modifiedMs) / 86400000.0;
                if (ageDays < 30.0) s += 2.0 * (1.0 - ageDays / 30.0);
            }
            s += static_cast<double>(doc.rankBoost) * 0.1;
            score += s;
        } else if (ranking_) {
            score += ranking_->Score(doc, query);
        } else {
            score += 1.0;
        }

        // Filters.
        if (!filter.type.empty() && doc.type != filter.type) continue;
        if (!filter.tag.empty()) {
            bool has = std::find(doc.tags.begin(), doc.tags.end(), filter.tag) != doc.tags.end();
            if (!has) continue;
        }
        if (!filter.language.empty() && doc.language != filter.language) continue;
        if (!filter.author.empty() && doc.author != filter.author) continue;
        if (!filter.idPrefix.empty() && doc.id.compare(0, filter.idPrefix.size(), filter.idPrefix) != 0)
            continue;
        if (filter.modifiedAfterMs > 0 && doc.modifiedMs < filter.modifiedAfterMs) continue;
        if (filter.modifiedBeforeMs > 0 && doc.modifiedMs > filter.modifiedBeforeMs) continue;

        SearchResult r;
        r.documentId = doc.id;
        r.type = doc.type;
        r.title = doc.title;
        r.modifiedMs = doc.modifiedMs;
        r.relatedIds = doc.relatedIds;
        r.score = score;
        auto clip = [](const std::string& src, size_t pos) {
            const std::string seps = " \t\n.,;:!?'\"()-";
            std::string snip = src.substr(pos, 120);
            if (!snip.empty() && seps.find(snip.back()) == std::string::npos &&
                pos + snip.length() < src.size() &&
                seps.find(src[pos + snip.length()]) == std::string::npos) {
                const size_t lastSep = snip.find_last_of(seps);
                if (lastSep != std::string::npos && lastSep > 80) snip.resize(lastSep);
            }
            // Trim a leading partial word the same way.
            if (pos > 0) {
                const size_t firstSep = snip.find_first_of(seps);
                if (firstSep != std::string::npos && firstSep < 30)
                    snip = snip.substr(firstSep + 1);
            }
            return snip;
        };
        const auto cheap = snippetPositions.find(doc.id);
        r.snippet = clip(doc.content, cheap != snippetPositions.end() ? cheap->second : 0);
        results.push_back(std::move(r));
    }

    std::stable_sort(results.begin(), results.end(),
                     [](const auto& a, const auto& b) { return a.score > b.score; });
    if (query.maxResults > 0 && results.size() > static_cast<size_t>(query.maxResults))
        results.resize(static_cast<size_t>(query.maxResults));

    for (auto& r : results) {
        if (snippetPositions.count(r.documentId))
            continue;   // cheap anchor already set: phrase or proximity
        const DocRecord* rec = nullptr;
        for (const auto& d : docs)
            if (d.doc.id == r.documentId) { rec = &d; break; }
        if (!rec) continue;
        // Words to cluster on: the resolved terms plus the prefix siblings
        // ("friend" matches "friends") — the same set the old in-loop scan used.
        std::vector<std::string> words;
        for (size_t k = 0; k < terms.size() && k < 64; ++k)
            if (CandidateWorthy(terms[k])) words.push_back(terms[k]);
        for (size_t k = 0; k < prefixAlts.size(); ++k)
            for (const auto& pt : prefixAlts[k]) words.push_back(pt);
        if (words.empty()) continue;
        const size_t p = DensestWindowPos(rec->lowerContent, words, 120);
        if (p != std::string::npos) {
            r.snippet = clip(rec->doc.content, p > 40 ? p - 40 : 0);
        } else if (!needle.empty()) {
            const size_t first = rec->lowerContent.find(words.front());
            if (first != std::string::npos)
                r.snippet = clip(rec->doc.content, first > 40 ? first - 40 : 0);
        }
    }
    return results;
}

Result<std::vector<SearchResult>> SearchEngine::Search(std::string_view query,
                                                       const SearchFilter& filter,
                                                       size_t limit) {
    std::string q = std::string(query);
    const auto begin = std::chrono::steady_clock::now();
    (void)EventBus::Instance().Publish(events::SearchStarted{q});
    const std::string filterKey = FilterSignature(filter);
    std::vector<SearchResult> results;
    bool cacheHit = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& entry : cache_) {
            if (entry.query == q && entry.filterKey == filterKey) {
                cacheHits_.fetch_add(1);
                // Copy only what the caller asked for, not the whole cached ranked set (up to 500
                // results with snippets) that is then thrown away under the engine lock.
                const size_t take = std::min(limit, entry.results.size());
                results.assign(entry.results.begin(),
                               entry.results.begin() + static_cast<std::ptrdiff_t>(take));
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
    sig += "|id=" + f.idPrefix;
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