// Unit tests: Bible Engine (docs/specs/24).
// Split from tests/unit/main.cpp so a single phase can run alone:
//   ./bps_unit_tests bible
#include "TestHarness.hpp"

void TestBibleResolver() {
    // Reference resolution against the canonical table (no Bible loaded).
    auto r1 = bb::ReferenceResolver::Resolve("John 3:16", {});
    CHECK(r1.ok() && r1.value().bookId == "JHN");
    CHECK(r1.ok() && r1.value().chapter == 3);
    CHECK(r1.ok() && r1.value().verseStart == 16 && r1.value().verseEnd == 16);
    auto r2 = bb::ReferenceResolver::Resolve("Psalm 23", {});
    CHECK(r2.ok() && r2.value().bookId == "PSA" && r2.value().chapter == 23);
    CHECK(r2.ok() && r2.value().IsWholeChapter());
    auto r3 = bb::ReferenceResolver::Resolve("1 Corinthians 13", {});
    CHECK(r3.ok() && r3.value().bookId == "1CO" && r3.value().chapter == 13);
    auto r4 = bb::ReferenceResolver::Resolve("Genesis 1:1-10", {});
    CHECK(r4.ok() && r4.value().bookId == "GEN");
    CHECK(r4.ok() && r4.value().verseStart == 1 && r4.value().verseEnd == 10);
    auto r5 = bb::ReferenceResolver::Resolve("Jn 3:16-18", {});
    CHECK(r5.ok() && r5.value().bookId == "JHN" && r5.value().verseEnd == 18);
    auto r6 = bb::ReferenceResolver::Resolve("Ps 23", {});
    CHECK(r6.ok() && r6.value().bookId == "PSA");
    auto r7 = bb::ReferenceResolver::Resolve("Romans 8", {});
    CHECK(r7.ok() && r7.value().bookId == "ROM" && r7.value().chapter == 8);
    auto bad = bb::ReferenceResolver::Resolve("Xyzzy 3:16", {});
    CHECK(!bad.ok());
    CHECK(bad.error().code == Err::Bible_InvalidReference);
    auto empty = bb::ReferenceResolver::Resolve("   ", {});
    CHECK(!empty.ok());
    auto all = bb::ReferenceResolver::ResolveAll("John 3:16, Psalm 23", {});
    CHECK(all.ok() && all.value().size() == 2);
    auto all3 = bb::ReferenceResolver::ResolveAll("John 3:16; Romans 8; Gen 1:1", {});
    CHECK(all3.ok() && all3.value().size() == 3);
    CHECK(bb::PassageRef{}.ToString() == "JHN 3:16" || bb::PassageRef{}.Valid() == false);
    bb::PassageRef ref;
    ref.bookId = "JHN"; ref.chapter = 3; ref.verseStart = 16; ref.verseEnd = 18;
    CHECK(ref.ToString() == "JHN 3:16-18");
}
void TestBibleProviders() {
    auto& eng = bb::BibleEngine::Instance();
    CHECK(eng.Initialize().ok());
    CHECK(eng.Start().ok());
    CHECK(eng.ProviderNames().size() >= 5);

    // Zefania-style XML.
    const char* zefania =
        "<bible abbrev=\"KJV\" name=\"King James\">"
        "<book bnum=\"43\" bsname=\"JHN\" bname=\"John\">"
        "<chapter number=\"3\">"
        "<verse number=\"16\">For God so loved the world that he gave his one and only Son.</verse>"
        "<verse number=\"17\">For God did not send his Son into the world to condemn the world.</verse>"
        "</chapter></book></bible>";
    auto id1 = eng.Import(zefania, "xml", bb::ImportOptions{std::string("KJV"), std::string(), false, false});
    CHECK(id1.ok() && id1.value() == "KJV");
    CHECK(eng.VerseCount("KJV").ok() && eng.VerseCount("KJV").value() == 2);
    auto v = eng.GetVerse("KJV", "JHN", 3, 16);
    CHECK(v.ok() && v.value().text.find("loved the world") != std::string::npos);

    // Zefania with bnum+bname but no bsname: the canonical id must be derived
    // from the book number/name, not left numeric (docs/specs/24 §Providers).
    const char* zefNoShort =
        "<bible abbrev=\"NBS\" name=\"No Bsname\">"
        "<book bnum=\"43\" bname=\"John\">"
        "<chapter number=\"3\">"
        "<verse number=\"16\">For God so loved the world that he gave his one and only Son.</verse>"
        "</chapter></book></bible>";
    auto idN = eng.Import(zefNoShort, "xml",
                          bb::ImportOptions{std::string("NBS"), std::string(), false, false});
    CHECK(idN.ok() && idN.value() == "NBS");
    auto vN = eng.GetVerse("NBS", "JHN", 3, 16);
    CHECK(vN.ok() && vN.value().text.find("loved the world") != std::string::npos);

    // Platform <bible><book num> shape.
    const char* plat =
        "<bible abbrev=\"WMB\" name=\"WMB\">"
        "<book num=\"JHN\" name=\"John\">"
        "<chapter num=\"3\"><verse num=\"16\">For God so loved the world.</verse></chapter>"
        "</book></bible>";
    auto id2 = eng.Import(plat, "xml", bb::ImportOptions{std::string("WMB"), std::string(), false, false});
    CHECK(id2.ok() && id2.value() == "WMB");

    // JSON.
    const char* js =
        "{\"metadata\":{\"id\":\"JSNB\",\"name\":\"JSON Bible\",\"language\":\"en\"},"
        "\"books\":[{\"id\":\"JHN\",\"name\":\"John\",\"testament\":\"new\"}],"
        "\"verses\":[{\"book\":\"JHN\",\"chapter\":3,\"verse\":16,"
        "\"text\":\"For God so loved the world that he gave his one and only Son.\"}]}";
    auto id3 = eng.Import(js, "json", bb::ImportOptions{std::string(), std::string(), false, false});
    CHECK(id3.ok() && id3.value() == "JSNB");

    // JSON as many Bible dumps write it: numeric canonical "book" (43 = John), "module"
    // / "shortname" / "lang_short" instead of id / abbreviation / language.
    const char* numJs =
        "{\"metadata\":{\"name\":\"Numbered\",\"shortname\":\"NUMB\",\"module\":\"numb\",\"lang_short\":\"en\"},"
        "\"verses\":[{\"book_name\":\"John\",\"book\":43,\"chapter\":3,\"verse\":16,"
        "\"text\":\"For God so loved the world.\"},"
        "{\"book_name\":\"Genesis\",\"book\":1,\"chapter\":1,\"verse\":1,\"text\":\"In the beginning.\"}]}";
    auto idNum = eng.Import(numJs, "json", bb::ImportOptions{std::string(), std::string(), false, false});
    CHECK(idNum.ok() && idNum.value() == "numb");
    auto numBible = eng.GetBible("numb");
    CHECK(numBible.ok() && numBible.value().metadata.abbreviation == "NUMB"
          && numBible.value().metadata.language == "en");
    auto numVerse = eng.GetVerse("numb", "JHN", 3, 16);
    CHECK(numVerse.ok() && numVerse.value().text == "For God so loved the world.");
    CHECK(eng.GetVerse("numb", "GEN", 1, 1).ok());

    // USFM.
    const char* usfm =
        "\\id JHN\n\\h John\n\\c 3\n"
        "\\v 16 For God so loved the world that he gave his one and only Son.\n"
        "\\s The Son of God\n\\v 17 For God did not send his Son into the world to condemn the world.\n";
    auto id4 = eng.Import(usfm, "usfm", bb::ImportOptions{std::string("USFMB"), std::string(), false, false});
    CHECK(id4.ok() && id4.value() == "USFMB");
    CHECK(eng.VerseCount("USFMB").ok() && eng.VerseCount("USFMB").value() == 2);
    auto v4 = eng.GetVerse("USFMB", "JHN", 3, 17);
    CHECK(v4.ok() && !v4.value().heading.empty());

    // OSIS.
    const char* osis =
        "<osis><osisText>"
        "<div type=\"book\" osisID=\"Jhn\"><title>John</title>"
        "<chapter osisID=\"Jhn.3\">"
        "<verse osisID=\"Jhn.3.16\">For God so loved the world that he gave his one and only Son.</verse>"
        "</chapter></div>"
        "</osisText></osis>";
    auto id5 = eng.Import(osis, "osis", bb::ImportOptions{std::string("OSISB"), std::string(), false, false});
    CHECK(id5.ok() && id5.value() == "OSISB");
    CHECK(eng.VerseCount("OSISB").ok() && eng.VerseCount("OSISB").value() == 1);

    // Plain text.
    const char* txt =
        "JHN 3:16 For God so loved the world that he gave his one and only Son.\n"
        "PSA 23:1 The LORD is my shepherd; I shall not want.\n";
    auto id6 = eng.Import(txt, "txt", bb::ImportOptions{std::string("TEXTB"), std::string(), false, false});
    CHECK(id6.ok() && id6.value() == "TEXTB");
    CHECK(eng.VerseCount("TEXTB").ok() && eng.VerseCount("TEXTB").value() == 2);

    // Failure modes.
    CHECK(!eng.Import("not xml at all", "xml", {}).ok());
    CHECK(!eng.Import("<bible></bible>", "xml", {}).ok());
    CHECK(!eng.Import("{}", "json", {}).ok());
    CHECK(!eng.Import("anything", "zzz", {}).ok());
    CHECK(eng.Import(zefania, "xml", bb::ImportOptions{std::string("KJV"), std::string(), false, false}).error().code ==
          Err::Bible_AlreadyExists);

    CHECK(eng.Stop().ok());
    CHECK(eng.Shutdown().ok());
}
void TestBibleEngine() {
    auto& eng = bb::BibleEngine::Instance();
    auto& search = s::SearchEngine::Instance();
    CHECK(search.Initialize().ok());
    CHECK(search.Start().ok());
    CHECK(eng.Initialize().ok());
    CHECK(eng.Start().ok());

    // Events: BibleImported fires for the import below.
    int importedEvents = 0;
    Subscription sub = EventBus::Instance().Subscribe<events::BibleImported>(
        [&](const events::BibleImported&) { ++importedEvents; }, 0);

    const char* src =
        "<bible abbrev=\"TEST\" name=\"Test Bible\">"
        "<book num=\"JHN\" name=\"John\">"
        "<chapter num=\"3\">"
        "<verse num=\"16\">For God so loved the world that he gave his one and only Son, that "
        "whoever believes in him shall not perish but have eternal life.</verse>"
        "<verse num=\"17\">For God did not send his Son into the world to condemn the world.</verse>"
        "</chapter></book>"
        "<book num=\"PSA\" name=\"Psalms\">"
        "<chapter num=\"23\"><verse num=\"1\">The LORD is my shepherd; I shall not want.</verse></chapter>"
        "</book></bible>";
    auto id = eng.Import(src, "xml", bb::ImportOptions{std::string("TEST"), std::string(), true, false});
    CHECK(id.ok() && id.value() == "TEST");
    CHECK(importedEvents == 1);
    (void)EventBus::Instance().Unsubscribe(sub);

    // Lookup.
    auto bible = eng.GetBible("TEST");
    CHECK(bible.ok() && bible.value().metadata.abbreviation == "TEST");
    CHECK(eng.VerseCount("TEST").ok() && eng.VerseCount("TEST").value() == 3);
    auto ref = eng.ResolveReference("John 3:16", "TEST");
    CHECK(ref.ok() && ref.value().bookId == "JHN" && ref.value().chapter == 3);
    auto passage = eng.GetPassage("TEST", ref.value());
    CHECK(passage.ok() && passage.value().size() == 1);
    CHECK(passage.ok() && passage.value().front().text.find("eternal life") != std::string::npos);
    // Whole chapter + whole book.
    bb::PassageRef chRef; chRef.bookId = "JHN"; chRef.chapter = 3;
    auto chapter = eng.GetPassage("TEST", chRef);
    CHECK(chapter.ok() && chapter.value().size() == 2);
    bb::PassageRef bookRef; bookRef.bookId = "JHN";
    auto wholeBook = eng.GetPassage("TEST", bookRef);
    CHECK(wholeBook.ok() && wholeBook.value().size() == 2);
    // Missing verse -> NotFound.
    bb::PassageRef missRef; missRef.bookId = "JHN"; missRef.chapter = 3;
    missRef.verseStart = missRef.verseEnd = 99;
    CHECK(!eng.GetPassage("TEST", missRef).ok());
    CHECK(eng.GetBible("NOPE").error().code == Err::Bible_NotFound);

    // Search the actual verse content.
    auto hits = eng.Search("eternal life", "TEST");
    CHECK(hits.ok() && !hits.value().empty());
    CHECK(hits.ok() && hits.value().front().verse == 16);
    auto refHits = eng.Search("John 3:16", "TEST");
    CHECK(refHits.ok() && !refHits.value().empty());
    CHECK(refHits.ok() && refHits.value().front().reference == "JHN 3:16");
    auto none = eng.Search("xyzzy-no-such-word", "TEST");
    CHECK(none.ok());

    // Parallel Bible comparison.
    const char* esv =
        "<bible abbrev=\"ESV\" name=\"ESV\">"
        "<book num=\"JHN\" name=\"John\">"
        "<chapter num=\"3\">"
        "<verse num=\"16\">For God so loved the world, that he gave his only Son.</verse>"
        "</chapter></book></bible>";
    CHECK(eng.Import(esv, "xml", bb::ImportOptions{std::string("ESV"), std::string(), true, false}).ok());    auto cmp = eng.Compare("John 3:16", {"TEST", "ESV"});
    CHECK(cmp.ok() && cmp.value().size() == 2);
    CHECK(cmp.ok() && cmp.value()[0].present && cmp.value()[1].present);
    bool hasEsv = false;
    for (const auto& pv : cmp.ok() ? cmp.value() : std::vector<bb::ParallelVerse>{})
        if (pv.bibleId == "ESV" && pv.text.find("only Son") != std::string::npos) hasEsv = true;
    CHECK(hasEsv);

    // Regression: importing a second Bible must never wipe the first Bible's
    // index documents (incremental upsert, never a global Rebuild).
    auto stillTest = eng.Search("eternal life", "TEST");
    CHECK(stillTest.ok() && !stillTest.value().empty());

    // Formatting.
    auto para = eng.Format("TEST", ref.value(), bb::FormatOptions{});
    CHECK(para.ok() && !para.value().empty());
    bb::FormatOptions vpl;
    vpl.mode = bb::FormatOptions::Mode::VersePerLine;
    vpl.includeNumbers = true;
    auto lines = eng.Format("TEST", chRef, vpl);
    CHECK(lines.ok());
    CHECK(lines.ok() && lines.value().find("16.") != std::string::npos);

    // User data (separate from Scripture).
    CHECK(eng.AddNote("TEST", ref.value(), "memorize this one").ok());
    auto notes = eng.Notes("TEST", ref.value());
    CHECK(notes.ok() && notes.value().size() == 1);
    CHECK(notes.ok() && notes.value().front().text == "memorize this one");
    CHECK(eng.SetHighlight("TEST", ref.value(), true).ok());
    auto highlights = eng.Highlights("TEST");
    CHECK(highlights.ok() && highlights.value().size() == 1);
    CHECK(eng.SetHighlight("TEST", ref.value(), false).ok());
    CHECK(eng.Highlights("TEST").value().empty());
    CHECK(eng.AddCollection("Sermon Passages").ok());
    CHECK(eng.AddToCollection("Sermon Passages", ref.value()).ok());
    auto col = eng.Collection("Sermon Passages");
    CHECK(col.ok() && col.value().size() == 1);
    CHECK(eng.CollectionNames().size() == 1);

    // Remove: the translation's documents leave the global search index too.
    CHECK(eng.RemoveBible("ESV").ok());
    auto esvGone = eng.Search("only Son", "ESV");
    CHECK(esvGone.ok() && esvGone.value().empty());
    CHECK(eng.BibleCount() == 1);
    CHECK(!eng.RemoveBible("ESV").ok());
    CHECK(eng.RemoveBible("TEST").ok());

    CHECK(eng.Stop().ok());
    CHECK(eng.Shutdown().ok());
    CHECK(search.Stop().ok());
    CHECK(search.Shutdown().ok());
}

// ===========================================================================
// Phase 13 — Song & Lyrics Engine (docs/specs/25)
// ===========================================================================
