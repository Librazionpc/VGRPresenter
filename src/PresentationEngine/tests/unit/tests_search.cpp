// Unit tests: Search & Indexing Engine (docs/specs/20).
// Split from tests/unit/main.cpp so a single phase can run alone:
//   ./bps_unit_tests search
#include "TestHarness.hpp"

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

// ===========================================================================
// Phase 10 — Media Engine (docs/specs/21)
// ===========================================================================
