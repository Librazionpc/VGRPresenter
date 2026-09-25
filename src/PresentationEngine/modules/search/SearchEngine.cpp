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

// Maximum ranked results cached per query — larger than any caller limit so a
// later, larger `limit` never under-fills from cache. Callers requesting more
// than this cap still get the top-kCacheRankedCap results (an explicit,
// documented ceiling rather than an unbounded cache).
constexpr int kCacheRankedCap = 500;

// Cap on the bounded prefix expansion (partial-match candidates per term) —
// keeps a 2-char stem ("we" -> "weak, wealth, went...") from ballooning the
// candidate set per keystroke.
constexpr size_t kPartialCap = 2500;

std::string Lower(std::string_view s);

// Default ranking (docs/specs/20 §Ranking): exact > prefix > partial > metadata
// + recency decay + rankBoost.
class DefaultRanking final : public IRankingStrategy {
public:
    // RawSearch takes a zero-allocation fast path for the DEFAULT strategy (the
    // ranking is recomputed inline from IndexStorage's precomputed lowercase
    // views); a custom strategy set via SetRankingStrategy keeps the classic
    // Score(doc, query) call — slower, but fully pluggable.
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

// Tokenize a raw query into structured terms: lowercase alphanumeric runs.
// (Was: space-split only — "if we," indexed the term "we," which is in no
// index, silently dropping the word. Lowercasing here lets ranking do
// case-insensitive work without rebuilding a lowered needle per candidate.)
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

// Terms worth indexing/candidate work: the old 1-char floor let every "a"/"I"
// query union the whole index. These do not DISAPPEAR — ParseQuery keeps every
// token, and RawSearch scores the raw text against them; they just do not
// contribute candidates on their own.
bool CandidateWorthy(std::string_view term) { return term.size() >= 2; }

// Jam-splitting: a user's query word may be TWO words typed without the
// space ("holyspirit", "holyghost"). Candidate split points are pairs of
// vocabulary words that reassemble the term; the pair covering the corpus
// best (min df of the two, so both halves are real words) wins. Frequency
// alone would split "theology" into "the"+"ology"-adjacent noise — the min-df
// gate plus a both-halves->=3-char rule keeps splits honest. Returns empty
// when the word does not split (the common case).
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

// Vocabulary resolution of one query term (typo correction + incomplete-word
// completion), against the live index. Returns the word to LOOK UP ("" keeps
// the term as typed) and the weight its BM25 contribution carries (1.0 for a
// completion — a stem, not an error; 0.7 for an edit-distance correction, so
// exact spellings always outrank them).
//   "friend" (no postings) -> "friends"   (completion, weight 1.0)
//   "thn"                  -> "then"      (1 edit, weight 0.7)
//   "frend"                -> "friend"    (1 edit, weight 0.7)
// The edit-distance scan keeps the term's first letter (the overwhelmingly
// common typo shape) and a ±maxDist length band, which holds the scan of a
// ~100k-term vocabulary to the low milliseconds.
struct ResolvedTerm { std::string word; double weight = 1.0; };
ResolvedTerm ResolveTermInVocabulary(const std::string& term,
                                     const IndexStorage& storage) {
    ResolvedTerm out;
    if (storage.DfOf(term) > 0) return out;   // spelled right: as typed (size only — no postings copy)
    // 1) Completion: the most frequent vocabulary word this term prefixes
    //    ("friend" -> "friends"). Frequency picks between friends/friend's/...
    size_t bestDf = 0;
    for (const auto& w : storage.TermsWithPrefix(term, 8)) {
        const size_t df = storage.DfOf(w);
        if (df > bestDf) { bestDf = df; out.word = w; }
    }
    if (bestDf > 0) return out;   // a stem, not a typo: full weight
    // 2) Typo: closest vocabulary word by bounded edit distance.
    const size_t maxDist = term.size() <= 4 ? 1 : 2;
    std::string best;
    size_t bestDist = maxDist + 1, bestDf2 = 0;
    storage.ForEachTermWithDf([&](const std::string& w, size_t wdf) {
        if (w.size() < 3 || w[0] != term[0]) return true;
        if (w.size() + maxDist < term.size() || w.size() > term.size() + maxDist) return true;
        const size_t d = EditDistanceBounded(term, w, bestDist);
        if (d <= maxDist && (d < bestDist || (d == bestDist && wdf > bestDf2))) {
            best = w; bestDist = d; bestDf2 = wdf;
        }
        return true;
    });
    if (!best.empty()) { out.word = best; out.weight = 0.7; }
    return out;
}

// The position in `content` where the query words cluster most densely: the
// ~snippet-width window holding the most DISTINCT words (occurrence count
// breaks ties). Anchoring here keeps every highlighted word inside the visible
// two lines — a paragraph can open with five "then"s and hide the actual
// "Then, friends," match 600 chars down, and the FIRST occurrence anchors the
// wrong place (the glow lands above the cap; the phrase only shows on hover).
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

// The smallest span (chars) holding ALL of `words` at least once, and where it
// starts. Unlike a first-occurrence pair this sees a TIGHT cluster sitting deep
// in the text even when an early lone occurrence of one word would have
// poisoned a first-find comparison — the paragraph that says "…might? Then,
// friends, isn't…" 1,200 chars past its first "then" IS a proximity hit.
// Bounded finds per word (a common word's occurrences stop at 32) keep the
// scan flat; returns false when any word never appears.
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
    // Cache invalidation: per document OUTSIDE a batch was 1200 wipes during a
    // boot-time index (the cache was dead weight exactly when search ran). Inside
    // SuspendCache/batch scope the wipe is deferred; the outermost resume clears
    // once.
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
// Public fuzzy resolution (see SearchEngine.hpp): one term in, one out, in
// order — the same resolution RawSearch applies internally, exposed for
// callers that run their own text scans.
Result<std::vector<SearchEngine::ResolvedQueryTerm>> SearchEngine::ResolveTerms(
    const std::vector<std::string>& terms) const {
    std::vector<ResolvedQueryTerm> out;
    out.reserve(terms.size());
    for (const std::string& t : terms) {
        if (t.empty()) continue;
        ResolvedTerm r = ResolveTermInVocabulary(Lower(t), storage_);
        out.push_back({r.word.empty() ? Lower(t) : r.word, r.weight});
    }
    return out;
}

// One query word, FULLY resolved — the single definition of the rules, shared
// by the public ResolveTermsWithSplits and RawSearch's inline path so they can
// never disagree:
//   1. spell-check + completion   ("thn"->"then" w=0.7, "friend"->"friends" w=1.0)
//   2. jam-split, last resort     ("holyspirit"->"holy spirit" w=1.0 — a space
//      inside the word; only when spell-check found nothing and the vocabulary
//      does not know the word, so "sandwhich"->"sandwich" keeps beating the
//      tempting but wrong "sand"+"which")
ResolvedTerm ResolveOne(const std::string& raw, const IndexStorage& storage) {
    ResolvedTerm r = ResolveTermInVocabulary(Lower(raw), storage);
    if (r.word.empty()) r.word = Lower(raw);
    if (r.weight == 1.0 && r.word.size() >= 6 && storage.DfOf(r.word) == 0) {
        if (auto [a, b] = SplitJam(r.word, storage); !a.empty()) r.word = a + ' ' + b;
    }
    return r;
}

// Two-word jams ("holyspirit") split at their best vocabulary seam — the pair
// of real words that reassembles the term. Nothing else about resolution
// changes: callers get one output term per input, in order, and a split
// arrives as "holy spirit" (a space inside one entry) to keep the 1:1 shape.
Result<std::vector<SearchEngine::ResolvedQueryTerm>> SearchEngine::ResolveTermsWithSplits(
    const std::vector<std::string>& terms) const {
    std::vector<ResolvedQueryTerm> out;
    out.reserve(terms.size());
    for (const std::string& t : terms) {
        if (t.empty()) continue;
        const ResolvedTerm r = ResolveOne(t, storage_);
        out.push_back({r.word, r.weight});
    }
    return out;
}

Result<std::vector<SearchResult>> SearchEngine::RawSearch(const SearchQuery& query,
                                                          const SearchFilter& filter) const {
    std::vector<SearchResult> results;
    // Candidate registration from the INVERTED INDEX (ids only — no copies).
    // A doc under a term's postings contains that term as a WHOLE TOKEN by
    // construction (the indexer tokenizes on non-alphanumerics), so whole-word
    // matching costs nothing at query time. A bounded prefix expansion adds the
    // PARTIAL class ("we" also matching "went") — scored lower, capped so a
    // short stem cannot balloon the set. Terms under 2 chars contribute no
    // candidates of their own (see CandidateWorthy); a query of ONLY those — or
    // no indexable terms at all — falls back to a bounded full scan so short
    // queries still answer honestly instead of returning nothing.
    struct TermClass { uint64_t whole = 0, partial = 0; };
    std::map<std::string, double, std::less<>> candidates;
    std::unordered_map<std::string, TermClass> masks;
    // The query's terms, deduplicated IN THE USER'S ORDER (the verbatim-phrase
    // test must honor what was typed), then resolved against the vocabulary:
    // typos corrected ("thn" -> "then"), incomplete words completed ("friend"
    // -> "friends") — each correction carrying a <1.0 weight so exact
    // spellings always outrank fuzzy ones (see ResolveTermInVocabulary).
    std::vector<std::string> terms;
    std::vector<double> termWeight;
    {
        // One helper owns resolution (spell-check -> completion -> jam-split,
        // see ResolveOne); this loop only dedupes on the RESOLVED word ("frend
        // friend" collapses to one "friend") and splits spaced entries (a jam
        // split arrives as "holy spirit") into real terms for the bitmasks.
        for (const auto& raw : query.exactTerms) {
            if (raw.empty()) continue;
            const ResolvedTerm r = CandidateWorthy(raw) ? ResolveOne(raw, storage_)
                                                        : ResolvedTerm{Lower(raw), 1.0};
            std::istringstream in(r.word);
            std::string part;
            while (in >> part) {
                if (std::find(terms.begin(), terms.end(), part) != terms.end()) continue;
                terms.push_back(part);
                termWeight.push_back(r.weight);
            }
        }
    }
    // BM25 corpus stats + per-term postings, fetched ONCE per query (not per
    // candidate). idf(t) = ln(1 + (N - df + 0.5)/(df + 0.5)) — non-negative.
    // Per-document scoring (in the docs loop below) applies the FULL BM25 tf
    // component including length normalization: dl/avgdl with b = 0.75 — a
    // 100-paragraph sermon must not outscore a 3-paragraph one just by saying
    // the word more often.
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
    // Per query term: its top prefix siblings by df ("friend" -> ["friends",
    // "friendship"...]) — the scoring pass uses them to give prefix-only docs
    // partial word credit (see the 40% branch below).
    std::vector<std::vector<std::string>> prefixAlts(terms.size());
    bool anyCandidateTerm = false;
    // A near-universal word ("and", "could" — df approaching the corpus size)
    // used to register EVERY document as a candidate: one map node per doc plus
    // a per-term tf map of the same size, per keystroke. A stop word carries
    // almost no ranking information anyway, so a term whose postings exceed a
    // fraction of the corpus contributes candidates NO-OP (its own docs are
    // effectively "all docs"); the query still ranks on its other terms, and
    // an ONLY-stop-word query falls through to the bounded full scan below —
    // honest, and capped.
    size_t corpusDocs = N;
    // BOTH gates must hold: a >60% share keeps common words from being
    // registered, but only ABOVE an absolute df floor (4k postings) — with a
    // one-document test corpus "grace" IS 100% of the corpus and must stay a
    // normal term; the allocation storm only exists at hundred-thousand scale.
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
            // Partial class: the term's vocabulary siblings ranked by DOCUMENT
            // FREQUENCY (not alphabetical — "friend" must expand to the big
            // "friends", not to "friendlier"), capped so a 2-char stem cannot
            // balloon the candidate set per keystroke.
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
        // A term below the index floor ("a", "I") has no postings by design, so
        // a query of ONLY those falls back to a bounded full scan to answer
        // honestly. A long term that simply MISSES ("nonexistent-xyz") resolves
        // to nothing anywhere — that is a genuine no-match, not a scan request:
        // return empty, the pre-fuzzy behavior (the miss test pins it).
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

    // THE LAG FIX: candidates are pre-scored from the INDEX ALONE (postings tf
    // + term weights, no text touched), and only the top slice gets full
    // DocRecords — each carries TWO full text copies (content + precomputed
    // lowercase), so copying one per candidate per keystroke moved hundreds of
    // MB of strings for common words. The pre-score's only job is to pick which
    // few dozen candidates deserve that copy; the authoritative BM25 + bonuses
    // run below on just those.
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
                // Type pre-filter BEFORE the truncation: the pre-score picks the
                // top-48 candidates to fetch, and the display filter (below) runs
                // only on those. Without the early drop, a common query word's
                // global top-48 is all short Bible verses — the length tiebreak
                // favors them — so filter.type="table" emptied the result while
                // dozens of matching sermons sat just outside the fetched slice.
                // ("then friend": 48/48 bible → 0 table rows; "revelation" only
                // survived because the word is rare.) Rank WITHIN the kept type.
                if (!filter.type.empty()) {
                    const auto mIt = metas.find(id);
                    if (mIt != metas.end() && mIt->second.type != filter.type) continue;
                }
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

    // The needle and the RESOLVED query as one phrase ("thn frend" -> "then
    // friend"), lowered once per QUERY. Whole-vs-partial comes from the INDEX
    // masks above — no per-candidate content scans.
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

    // Snippet anchors found CHEAPLY during scoring (verbatim phrase / proximity,
    // one bounded find each). The expensive densest-cluster scan moved BELOW the
    // sort — computed for the SHOWN rows only. It used to run for every
    // candidate inside the loop: a common word registers thousands of
    // candidates, each paying a bounded-but-real multi-find pass per keystroke,
    // while only maxResults rows ever display — that was the autofill lag.
    std::unordered_map<std::string, size_t> snippetPositions;
    auto clip = [](const std::string& src, size_t pos) {
        const std::string seps = " \t\n.,;:!?\"()-";
        std::string snip = src.substr(pos, 120);
        // Never end mid-word: a clipped tail reads as garbage, and a query
        // word straddling the cut can't be highlighted whole. Back up to the
        // last separator when the cut lands inside a word (keeping >= 80).
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
        size_t snippetPos = std::string::npos;
        double& score = candidates[doc.id];
        if (fastRank) {
            // BM25 (Okapi, the article's ranking — principled IDF weighting:
            // rare words count far more than "then"/"friends"; tf saturates;
            // long documents are length-normalized) + structure bonuses on
            // top: title match, all-words-present, verbatim phrase.
            constexpr double kK1 = 1.2, kB = 0.75;
            const double dlNorm = kB * (static_cast<double>(rec.tokenCount) / avgdl - 1.0);
            double s = 0.0;
            size_t wholeWords = 0;
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
                    // Whole-token miss but the word may exist as a PREFIX sibling
                    // ("friend" inside "friends" — para 7 says "Then, friends,").
                    // Zero credit here is why such docs fell off the candidate
                    // list before the paragraph scan ever saw them. Prefix hits
                    // score at 40% (real evidence, below a true whole word).
                    if (tc.partial & (1ull << k)) {
                        for (const auto& pt : prefixAlts[k]) {
                            const auto pts = termStats.find(pt);
                            if (pts == termStats.end()) continue;
                            const auto ptf = pts->second.tf.find(doc.id);
                            if (ptf == pts->second.tf.end()) continue;
                            const double pidf = std::log(1.0 +
                                (static_cast<double>(N) - static_cast<double>(pts->second.df) + 0.5)
                                / (static_cast<double>(pts->second.df) + 0.5));
                            const double ptfd = static_cast<double>(ptf->second);
                            s += 0.4 * termWeight[k] * pidf * (ptfd * (kK1 + 1.0))
                               / (ptfd + kK1 * (1.0 - kB + kB * static_cast<double>(rec.tokenCount) / avgdl));
                            break;   // the highest-df sibling carries the credit
                        }
                    }
                    continue;
                }
                const double idf = std::log(1.0 + (static_cast<double>(N) - static_cast<double>(ts->second.df) + 0.5)
                                                       / (static_cast<double>(ts->second.df) + 0.5));
                const double tfd = static_cast<double>(tfIt->second);
                s += termWeight[k] * idf * (tfd * (kK1 + 1.0))
                   / (tfd + kK1 * (1.0 - kB + kB * static_cast<double>(rec.tokenCount) / avgdl));
            }
            // Title hits: a doc whose TITLE carries the whole word is the better
            // answer ("...Then Jesus Came" for then+jesus) — flat bonus, IDF-
            // scaled so rare words boost the title more.
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
            // Verbatim phrase (a pasted paragraph): beats everything except
            // a longer phrase — longer pastes are more specific. Single-word
            // queries skip it (the word bonus already covers it). The shared
            // helper: punctuation-tolerant, the LAST word may end mid-word
            // ("then friend" finds "Then, friends," — the case a literal
            // substring find silently missed).
            size_t pos = std::string::npos;
            if (phraseWorth && ContainsPhrase(rec.lowerContent, phraseTerms, &pos)) {
                s += 20.0 + phrase.size() * 0.02;
                snippetPos = pos > 40 ? pos - 40 : 0;
                snippetPositions[doc.id] = snippetPos;   // cheap anchor: reused below
            } else if (phraseWorth) {
                // No verbatim phrase: PROXIMITY — words near each other mean the
                // passage is about the query even when connectors separate them
                // ("then" ... "friends" within a window outranks the same words
                // chapters apart). Window = 40 words; closer clusters score more.
                // The nearest COVERING window of all query words — not the first
                // occurrence of each (an early lone "then" used to poison the
                // measurement and starve a paragraph whose real "Then, friends,"
                // cluster sat 1,200 chars deeper).
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
        if (filter.modifiedAfterMs > 0 && doc.modifiedMs < filter.modifiedAfterMs) continue;
        if (filter.modifiedBeforeMs > 0 && doc.modifiedMs > filter.modifiedBeforeMs) continue;

        SearchResult r;
        r.documentId = doc.id;
        r.type = doc.type;
        r.title = doc.title;
        r.modifiedMs = doc.modifiedMs;
        r.relatedIds = doc.relatedIds;
        r.score = score;
        // Snippet from the PRECOMPUTED lowercase content, anchored on the phrase
        // or the first whole-word hit (was: a Lower() per result row, always the
        // head of the text).
        auto clip = [](const std::string& src, size_t pos) {
            const std::string seps = " \t\n.,;:!?'\"()-";
            std::string snip = src.substr(pos, 120);
            // Never end mid-word: a clipped tail reads as garbage, and a query
            // word straddling the cut can't be highlighted whole. Back up to the
            // last separator when the cut lands inside a word (keeping >= 80).
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
        // Snippet from the PRECOMPUTED lowercase content, anchored on the CHEAP
        // anchor found during scoring (verbatim phrase / proximity) — or the
        // head when none. The densest-cluster anchoring for the remaining rows
        // happens below, AFTER the sort, for the shown rows only.
        const auto cheap = snippetPositions.find(doc.id);
        r.snippet = clip(doc.content, cheap != snippetPositions.end() ? cheap->second : 0);
        results.push_back(std::move(r));
    }

    std::stable_sort(results.begin(), results.end(),
                     [](const auto& a, const auto& b) { return a.score > b.score; });
    if (query.maxResults > 0 && results.size() > static_cast<size_t>(query.maxResults))
        results.resize(static_cast<size_t>(query.maxResults));

    // THE GLOW RULE, deferred: shown rows without a cheap anchor get their
    // snippet re-anchored at the DENSEST cluster of query words. The cluster
    // window (= the visible snippet width, 120) bounds what can be counted, so
    // a counted word is always INSIDE the visible text — never swallowed past
    // the clip (a wider window than the clip is exactly how words got cut off).
    // This used to run per candidate inside the scoring loop (the autofill
    // lag); per SHOWN row it is a handful of bounded scans per keystroke.
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
