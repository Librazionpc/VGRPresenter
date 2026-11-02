#include "TestHarness.hpp"

#include "modules/search/TextMatching.hpp"

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

void TestSearchIndexing() {
    auto& eng = s::SearchEngine::Instance();
    CHECK(eng.Initialize().ok());
    CHECK(eng.Start().ok());
    s::SearchDocument song;
    song.id = "song-1";
    song.type = "song";
    song.title = "Amazing Grace";
    song.content = "Amazing grace how sweet the sound that saved a wretch like me";
    song.tags = {"hymn", "grace"};
    song.author = "John Newton";
    song.modifiedMs = 1000;
    auto terms = eng.IndexDocument(song);
    CHECK(terms.ok() && terms.value() > 0);
    CHECK(eng.DocumentCount() == 1);
    // Incremental re-index of the same doc replaces (no duplicate).
    s::SearchDocument song2 = song;
    song2.content = "Amazing grace how sweet the sound";
    CHECK(eng.IndexDocument(song2).ok());
    CHECK(eng.DocumentCount() == 1);
    // Search correctness.
    auto results = eng.Search("grace");
    CHECK(results.ok() && !results.value().empty());
    CHECK(results.value()[0].documentId == "song-1");
    auto miss = eng.Search("nonexistent-term-xyz");
    CHECK(miss.ok() && miss.value().empty());
    // Metadata filters.
    s::SearchFilter filter;
    filter.type = "song";
    auto byType = eng.Search("grace", filter);
    CHECK(byType.ok() && byType.value().size() == 1);
    filter.type = "image";
    auto wrongType = eng.Search("grace", filter);
    CHECK(wrongType.ok() && wrongType.value().empty());
    // Suggestions.
    auto sugg = eng.Suggest("ama");
    CHECK(!sugg.empty());
    // Session.
    auto sid = eng.BeginSession();
    CHECK(sid.ok());
    s::SearchEngine::SessionState st;
    st.query = "grace";
    CHECK(eng.UpdateSession(sid.value(), st).ok());
    auto back = eng.GetSession(sid.value());
    CHECK(back.ok() && back.value().query == "grace");
    // Snapshot round-trip.
    auto snap = eng.ExportIndex();
    CHECK(snap.ok() && !snap.value().empty());
    CHECK(eng.Reset().ok());   // clear cache/history
    auto restored = eng.ImportIndex(snap.value());
    CHECK(restored.ok() && restored.value() > 0);
    CHECK(eng.DocumentCount() == 1);
    // Cache hit tracking.
    auto hitsBefore = eng.CacheHits();
    (void)eng.Search("grace");
    (void)eng.Search("grace");
    CHECK(eng.CacheHits() >= hitsBefore + 1);
    CHECK(eng.ValidateIndex().ok());
    CHECK(eng.Stop().ok());
    CHECK(eng.Shutdown().ok());
}
void TestSearchRanking() {
    auto& eng = s::SearchEngine::Instance();
    CHECK(eng.Initialize().ok());
    s::SearchDocument title;
    title.id = "t";
    title.type = "song";
    title.title = "Grace";          // title match ranks higher
    title.content = "something else";
    title.modifiedMs = 1000;
    s::SearchDocument body;
    body.id = "b";
    body.type = "song";
    body.title = "Other";
    body.content = "grace in the body text only";
    body.modifiedMs = 1000;
    CHECK(eng.IndexDocument(title).ok());
    CHECK(eng.IndexDocument(body).ok());
    auto results = eng.Search("grace");
    CHECK(results.ok() && results.value().size() == 2);
    CHECK(results.value()[0].documentId == "t");   // title-ranked first
    CHECK(eng.Shutdown().ok());
}

void TestSearchFuzzyResolution() {
    using bps::search::textmatch::ContainsWord;
    using bps::search::textmatch::ContainsPhrase;
    using bps::search::textmatch::EditDistanceBounded;
    using bps::search::textmatch::ResolveWordInList;
    using bps::search::textmatch::TokenizeWords;

    // Tokenizer: punctuation-insensitive words, lower-cased.
    const auto toks = TokenizeWords("Then, friends!");
    CHECK((toks == std::vector<std::string>{"then", "friends"}));

    // Whole-word containment.
    CHECK(ContainsWord("we went", "we"));
    CHECK(!ContainsWord("we went", "e w"));
    CHECK(!ContainsWord("sweet", "we"));

    // Phrase: user order, any punctuation between, LAST word may end mid-word
    // (the "friend"->"friends" case the user called out).
    CHECK(ContainsPhrase("then, friends, this is", {"then", "friends"}));
    CHECK(ContainsPhrase("then, friends, this is", {"then", "friend"}));
    CHECK(!ContainsPhrase("then, friends, this is", {"friends", "then"}));   // order kept
    CHECK(!ContainsPhrase("then, friends, this is", {"hen", "friends"}));    // mid-word start

    // Bounded edit distance (beyond budget returns maxDist+1).
    CHECK(EditDistanceBounded("thn", "then", 2) == 1);        // insert 'e'
    CHECK(EditDistanceBounded("frend", "friend", 2) == 1);
    CHECK(EditDistanceBounded("frind", "friend", 1) == 1);    // insert 'e': the dropped-letter shape
    CHECK(EditDistanceBounded("frind", "friend", 0) == 1);    // over budget

    const std::vector<std::string> vocab{"then", "friends", "friend", "the", "find", "ghost"};
    const auto asTyped = ResolveWordInList("friends", vocab);   // (braces protect the comma)
    CHECK((asTyped.weight == 1.0 && asTyped.word.empty()));
    const auto completed = ResolveWordInList("frien", vocab);   // incomplete word
    CHECK((completed.word == "friend" || completed.word == "friends"));
    // "friendz" is 1 edit from BOTH friend and friends (equal distance, equal
    // list frequency): the tie goes to the first in the list — deterministic.
    CHECK(ResolveWordInList("friendz", vocab).word == "friends");
    CHECK(ResolveWordInList("thn", vocab).word == "then");          // insertion > substitution
    CHECK(ResolveWordInList("frind", vocab).word == "friend");
    CHECK(ResolveWordInList("zzz", vocab).word.empty());

    using bps::search::textmatch::IsAdjacentTransposition;
    CHECK(IsAdjacentTransposition("recieve", "receive"));
    CHECK(IsAdjacentTransposition("thier", "their"));
    CHECK(!IsAdjacentTransposition("recieve", "relieve"));   // one substitution, not a swap
    CHECK(!IsAdjacentTransposition("receive", "receive"));
    const std::vector<std::string> swapVocab{"receive", "relieve", "relieved"};
    CHECK(ResolveWordInList("recieve", swapVocab).word == "receive");
    CHECK(ResolveWordInList("thier", {"their", "thief", "there"}).word == "their");

    // End to end through the engine: a typo query finds the doc, exact spelling
    // still wins the ranking, and "holyspirit" splits into holy+spirit.
    auto& eng = s::SearchEngine::Instance();
    CHECK(eng.Initialize().ok());
    s::SearchDocument d;
    d.id = "fz";
    d.type = "song";
    d.title = "Pentecost";
    d.content = "the baptism of the holy spirit fell on them";
    d.modifiedMs = 1000;
    CHECK(eng.IndexDocument(d).ok());
    auto fuzzy = eng.Search("holly spirirt");
    CHECK(fuzzy.ok() && !fuzzy.value().empty());
    CHECK(fuzzy.value()[0].documentId == "fz");
    auto exact = eng.Search("holy spirit");
    CHECK(exact.ok() && exact.value()[0].documentId == "fz");
    auto split = eng.Search("holyspirit");
    CHECK(split.ok() && !split.value().empty());
    CHECK(split.value()[0].documentId == "fz");
    auto resolutions = eng.ResolveTermsWithSplits({"holyspirit"});
    CHECK(resolutions.ok() && resolutions.value().size() == 1);
    CHECK(resolutions.value()[0].term == "holy spirit");
    CHECK(eng.Shutdown().ok());
}

// ===========================================================================
// Phase 10 — Media Engine (docs/specs/21)
// ===========================================================================

// ===========================================================================
// THE TYPING TAIL — the rule every live search box shares
// ===========================================================================
namespace {
void IndexTypingTailCorpus(s::SearchEngine& eng) {
    const std::pair<const char*, const char*> docs[] = {
        // THE answer: every typed word, and the likeliest reading of "rec".
        {"tail-0", "and compelling and compulsory that you receive the holy ghost now"},
        // More "receive" documents (corpus frequency = confidence) ...
        {"tail-1", "receive the spirit after they believed and were baptized"},
        {"tail-2", "receive the promise of the father and be filled with it"},
        {"tail-3", "blessed are they which do hunger for they shall receive"},
        // ... and two other readings of the same prefix, less used.
        {"tail-4", "they received the spirit after they believed and were baptized"},
        {"tail-5", "compelling that you record every word for the people"},
    };
    for (const auto& [id, text] : docs) {
        s::SearchDocument d;
        d.id = id;
        d.type = "table";
        d.title = std::string("Sermon ") + id;
        d.content = text;
        d.modifiedMs = 1000;
        CHECK(eng.IndexDocument(d).ok());
    }
}

bool TailListHas(const std::vector<std::string>& list, const std::string& word) {
    return std::find(list.begin(), list.end(), word) != list.end();
}
} // namespace

// The resolution keeps a half-typed word (and hands back its ladder); a word
// that is not a prefix of anything in the vocabulary is still corrected.
void TestTypingTailResolution() {
    auto& eng = s::SearchEngine::Instance();
    CHECK(eng.Initialize().ok());
    CHECK(eng.Start().ok());
    IndexTypingTailCorpus(eng);

    auto tail = eng.ResolveTermsWithSplits({"compelling", "rec"}, /*typingTail=*/true);
    CHECK(tail.ok() && tail.value().size() == 2);
    CHECK(tail.value()[0].term == "compelling");
    CHECK(tail.value()[1].term == "rec");                 // the prefix, not a guess
    CHECK(tail.value()[1].candidates.size() > 1);          // several readings offered
    CHECK(!TailListHas(tail.value()[1].candidates, "rec"));   // never the typed word itself
    // The ladder handed back is the shared one — no hidden second order.
    CHECK(tail.value()[1].candidates == eng.PrefixCandidates("rec"));

    // A hint answer (typingTail = false) still completes, so the search boxes can
    // say "rec -> receive" when the user asks what it thinks they meant.
    auto hinted = eng.ResolveTermsWithSplits({"compelling", "rec"});
    CHECK(hinted.ok() && hinted.value().size() == 2);
    CHECK(hinted.value()[1].term != "rec");
    CHECK(hinted.value()[1].term.rfind("rec", 0) == 0);
    CHECK(hinted.value()[1].candidates.empty());

    // A fully typed misspelling is still corrected, tail or not — "recieve" is not
    // a prefix of anything the user meant.
    auto typo = eng.ResolveTermsWithSplits({"recieve"}, /*typingTail=*/true);
    CHECK(typo.ok() && typo.value().size() == 1);
    CHECK(typo.value()[0].term == "receive");
    CHECK(typo.value()[0].weight < 1.0);

    CHECK(eng.Shutdown().ok());
}

// The candidate ladder: most confident (most used) first, and the half-typed
// tail finds the document whichever reading it carries.
void TestTypingTailSearch() {
    auto& eng = s::SearchEngine::Instance();
    CHECK(eng.Initialize().ok());
    CHECK(eng.Start().ok());
    IndexTypingTailCorpus(eng);

    CHECK(eng.PrefixCandidates("").empty());                  // nothing to expand
    const auto ladder = eng.PrefixCandidates("rec");
    CHECK(ladder.size() >= 3);
    CHECK(!TailListHas(ladder, "rec"));                       // the typed word is not a candidate
    CHECK(ladder.front() == "receive");                       // the corpus says this most
    CHECK(TailListHas(ladder, "received"));
    CHECK(TailListHas(ladder, "record"));
    // ONLY a candidate list: the most used reading first, the others after it.
    CHECK(ladder.front() != "record");

    // The half-typed tail finds the "receive" paragraph, and the reading the
    // ladder is most confident about leads the merely possible ones ("record").
    auto hits = eng.Search("compelling that you rec");
    CHECK(hits.ok() && !hits.value().empty());
    CHECK(hits.value().front().documentId == "tail-0");

    auto none = eng.Search("compelling ghost rec");
    for (const auto& r : none.value())
        CHECK(r.documentId != "tail-1");   // "received", but no compelling/ghost

    CHECK(eng.Shutdown().ok());
}