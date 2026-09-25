// Unit tests: Search & Indexing Engine (docs/specs/20).
// Split from tests/unit/main.cpp so a single phase can run alone:
//   ./bps_unit_tests search
#include "TestHarness.hpp"

#include "modules/search/TextMatching.hpp"

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

// The fuzzy layer every search surface shares (TextMatching.hpp + the engine's
// vocabulary resolution): incomplete words complete, typos correct (dropped
// letters beat substitutions), typed-together words split — and exact
// spellings always outrank all of it.
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

    // Resolution: exact stays, incomplete completes, dropped-letter typo beats
    // a same-distance substitution ("thn" -> "then", never "the"), unresolvable
    // words return empty.
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
