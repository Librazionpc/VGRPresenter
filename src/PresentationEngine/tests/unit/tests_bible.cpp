// Unit tests: Bible Engine (docs/specs/24).
// Split from tests/unit/main.cpp so a single phase can run alone:
//   ./bps_unit_tests bible
#include "TestHarness.hpp"

#include "modules/presentation/BookFill.hpp"
#include "platform/PlatformAccessor.hpp"

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
    // Spaces around the separators are typed constantly ("Genesis 1: 4" from the
    // screenshot): they must not break the verse out of the reference.
    auto r8 = bb::ReferenceResolver::Resolve("Genesis 1: 4", {});
    CHECK(r8.ok() && r8.value().chapter == 1 && r8.value().verseStart == 4 && r8.value().verseEnd == 4);
    auto r9 = bb::ReferenceResolver::Resolve("John 3: 16 - 18", {});
    CHECK(r9.ok() && r9.value().verseStart == 16 && r9.value().verseEnd == 18);
    // Trailing colon (the auto-colon leaves "John 3:" in the box mid-typing): whole chapter.
    auto r10 = bb::ReferenceResolver::Resolve("John 3:", {});
    CHECK(r10.ok() && r10.value().chapter == 3 && r10.value().verseStart == 0);
    // FreeShow-parity prefix resolution: a typed PREFIX that uniquely names one book
    // resolves ("gene" opens Genesis) — with or without a chapter/verse tail.
    auto p1 = bb::ReferenceResolver::Resolve("gene", {});
    CHECK(p1.ok() && p1.value().bookId == "GEN");
    auto p2 = bb::ReferenceResolver::Resolve("gene 1:4", {});
    CHECK(p2.ok() && p2.value().bookId == "GEN" && p2.value().chapter == 1 && p2.value().verseStart == 4);
    auto p3 = bb::ReferenceResolver::Resolve("genes", {});
    CHECK(p3.ok() && p3.value().bookId == "GEN");
    auto p4 = bb::ReferenceResolver::Resolve("1 sam", {});
    CHECK(p4.ok() && p4.value().bookId == "1SA");
    auto p5 = bb::ReferenceResolver::Resolve("rev 1", {});
    CHECK(p5.ok() && p5.value().bookId == "REV" && p5.value().chapter == 1);
    // Ambiguous prefixes must NOT guess: "j" starts Joshua/Job/John/..., "1 c"
    // starts 1 Chronicles and 1 Corinthians — both stay unresolved.
    auto amb1 = bb::ReferenceResolver::Resolve("j 3:16", {});
    CHECK(!amb1.ok());
    auto amb2 = bb::ReferenceResolver::Resolve("1 c 13", {});
    CHECK(!amb2.ok());
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

    // Real Zefania shape: <XMLBIBLE> root, uppercase tags throughout, the
    // bnumber/cnumber/vnumber attribute spellings, and the bible name in
    // <information><biblename> — as distributed on bible websites.
    const char* zefCaps =
        "<XMLBIBLE biblename=\"King James Caps\" status=\"v\">"
        "<BIBLEBOOK bnumber=\"43\" bname=\"John\">"
        "<CHAPTER cnumber=\"3\">"
        "<VERS vnumber=\"16\">For God so loved the world that he gave his one and only Son.</VERS>"
        "<VERS vnumber=\"17\">For God did not send his Son into the world to condemn the world.</VERS>"
        "</CHAPTER></BIBLEBOOK></XMLBIBLE>";
    auto idCaps = eng.Import(zefCaps, "xml",
                             bb::ImportOptions{std::string("ZFNC"), std::string(), false, false});
    CHECK(idCaps.ok() && idCaps.value() == "ZFNC");
    CHECK(eng.VerseCount("ZFNC").ok() && eng.VerseCount("ZFNC").value() == 2);
    auto vCaps = eng.GetVerse("ZFNC", "JHN", 3, 16);
    CHECK(vCaps.ok() && vCaps.value().text.find("loved the world") != std::string::npos);
    auto capsBible = eng.GetBible("ZFNC");
    CHECK(capsBible.ok() && capsBible.value().metadata.name == "King James Caps");

    // OSIS content that ships in a ".xml" file: the extension-based lookup
    // must fall back to the OSIS provider when the Zefania XML provider
    // declines the payload (docs/specs/24 §Providers).
    const char* osisXml =
        "<osis><osisText>"
        "<div type=\"book\" osisID=\"Jhn\"><title>John</title>"
        "<chapter osisID=\"Jhn.3\">"
        "<verse osisID=\"Jhn.3.16\">For God so loved the world that he gave his one and only Son.</verse>"
        "</chapter></div>"
        "</osisText></osis>";
    auto idFx = eng.Import(osisXml, "xml",
                           bb::ImportOptions{std::string("OSIX"), std::string(), false, false});
    CHECK(idFx.ok() && idFx.value() == "OSIX");
    CHECK(eng.VerseCount("OSIX").ok() && eng.VerseCount("OSIX").value() == 1);
    auto vFx = eng.GetVerse("OSIX", "JHN", 3, 16);
    CHECK(vFx.ok() && vFx.value().text.find("loved the world") != std::string::npos);

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
    CHECK(!eng.Import("<XMLBIBLE></XMLBIBLE>", "xml", {}).ok());   // no verses
    CHECK(!eng.Import("<song></song>", "xml", {}).ok());           // not a bible at all
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

    // ---- the chapter lock: the search can be scoped to the open passage --------
    // The reference panes' book pill locks the search to what is on screen.
    // "shall" lives in BOTH books of this fixture (John 3:16 "shall not perish",
    // Psalm 23:1 "I shall not want"), so the same query shows the scope really
    // narrows — it is a narrowing of the SEARCH, not a label on the box.
    {
        auto both = eng.Search("shall", "TEST");
        bool anyPsalm = false, anyJohn = false;
        for (const auto& h : both.ok() ? both.value() : std::vector<bb::BibleSearchHit>{}) {
            if (h.bookId == "PSA") anyPsalm = true;
            if (h.bookId == "JHN") anyJohn = true;
        }
        CHECK(anyPsalm && anyJohn);   // the unscoped query spans two books

        auto lockedPsalm = eng.Search("shall", "TEST", "PSA", 23);
        CHECK(lockedPsalm.ok() && !lockedPsalm.value().empty());
        for (const auto& h : lockedPsalm.value())
            CHECK(h.bookId == "PSA" && h.chapter == 23);

        auto lockedJohn = eng.Search("shall", "TEST", "JHN", 3);
        CHECK(lockedJohn.ok() && !lockedJohn.value().empty());
        for (const auto& h : lockedJohn.value())
            CHECK(h.bookId == "JHN" && h.chapter == 3);

        // Locking to the BOOK is the wider step of the same lock: every chapter
        // of John, never a Psalm.
        auto lockedBook = eng.Search("shall", "TEST", "JHN", 0);
        CHECK(lockedBook.ok() && !lockedBook.value().empty());
        for (const auto& h : lockedBook.value())
            CHECK(h.bookId == "JHN");

        // A scope the query does not reach answers NOTHING. Falling back to the
        // whole Bible here is exactly the bug the pill promises not to have.
        auto unreached = eng.Search("eternal life", "TEST", "PSA", 23);
        CHECK(unreached.ok() && unreached.value().empty());

        // A typed REFERENCE respects the lock too (both tiers of Search).
        auto refLockedOut = eng.Search("John 3:16", "TEST", "PSA", 23);
        CHECK(refLockedOut.ok() && refLockedOut.value().empty());
        auto refLockedIn = eng.Search("John 3:16", "TEST", "JHN", 3);
        CHECK(refLockedIn.ok() && !refLockedIn.value().empty());
        CHECK(refLockedIn.ok() && refLockedIn.value().front().reference == "JHN 3:16");
    }

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

// ---------------------------------------------------------------------------
// The cheap ways to browse a Bible (Scripture tab): details, outline, one chapter.
// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
// Installed-bible persistence: an import survives a full engine restart when a
// store path is set; the store stays OFF (in-memory only) without one.
// ---------------------------------------------------------------------------
void TestBiblePersistence() {
    auto& fs = bps::platform::PlatformAccessor::Get().Filesystem();
    const std::string root = "/tmp/bps_bible_store";
    const std::string store = root + "/bibles.json";
    (void)fs.RemoveAll(root);

    auto& eng = bb::BibleEngine::Instance();
    const char* xml =
        "<bible abbrev=\"PRZ\" name=\"Persist Bible\">"
        "<book bnum=\"1\" bsname=\"GEN\" bname=\"Genesis\">"
        "<chapter number=\"1\">"
        "<verse number=\"1\">In the beginning God created the heaven and the earth.</verse>"
        "<verse number=\"2\">And the earth was without form and void.</verse>"
        "<verse number=\"3\">And God said, Let there be light.</verse>"
        "</chapter></book></bible>";

    // ---- lifecycle 1: import with a store path -> the store holds the bible ----
    CHECK(eng.Initialize().ok());
    CHECK(eng.Start().ok());
    eng.SetStorePath(store);
    auto id = eng.Import(xml, "xml", bb::ImportOptions{std::string("PRZ"), std::string(), false, false});
    CHECK(id.ok() && id.value() == "PRZ");
    CHECK(eng.BibleCount() == 1);
    CHECK(fs.Exists(store));
    CHECK(eng.Metadata("PRZ").ok() && eng.Metadata("PRZ").value().name == "Persist Bible");

    // The store keeps the canonical shape only (books + chapters), so the saved
    // JSON must read back through GetChapter/Outline identically.
    auto before = eng.GetChapter("PRZ", "GEN", 1);
    CHECK(before.ok() && before.value().verses.size() == 3);

    // ---- a full restart: Shutdown clears memory, Initialize restores from disk ----
    CHECK(eng.Stop().ok());
    CHECK(eng.Shutdown().ok());
    CHECK(eng.BibleCount() == 0);
    CHECK(eng.Initialize().ok());
    CHECK(eng.Start().ok());
    CHECK(eng.BibleCount() == 1);   // restored from the store, no re-import
    auto meta = eng.Metadata("PRZ");
    CHECK(meta.ok() && meta.value().name == "Persist Bible");
    auto after = eng.GetChapter("PRZ", "GEN", 1);
    CHECK(after.ok() && after.value().verses.size() == 3);
    CHECK(after.ok() && after.value().verses[2].text == "And God said, Let there be light.");
    auto outline = eng.Outline("PRZ");
    CHECK(outline.ok() && outline.value().size() == 1);
    CHECK(outline.ok() && outline.value().front().verseCounts == std::vector<int>{3});
    CHECK(eng.VerseCount("PRZ").ok() && eng.VerseCount("PRZ").value() == 3);

    // ---- remove persists too: a deleted translation stays deleted ----
    CHECK(eng.RemoveBible("PRZ").ok());
    CHECK(eng.BibleCount() == 0);
    CHECK(eng.Stop().ok());
    CHECK(eng.Shutdown().ok());
    CHECK(eng.Initialize().ok());
    CHECK(eng.Start().ok());
    CHECK(eng.BibleCount() == 0);   // not resurrected by the store

    // ---- no store path = no persistence (the unit-test default) ----
    eng.SetStorePath("");
    auto mem = eng.Import(xml, "xml", bb::ImportOptions{std::string("PRZ"), std::string(), false, false});
    CHECK(mem.ok());
    CHECK(eng.BibleCount() == 1);
    CHECK(eng.Stop().ok());
    CHECK(eng.Shutdown().ok());
    CHECK(eng.Initialize().ok());
    CHECK(eng.Start().ok());
    CHECK(eng.BibleCount() == 0);   // in-memory only: a restart loses it

    CHECK(eng.Stop().ok());
    CHECK(eng.Shutdown().ok());
    (void)fs.RemoveAll(root);
}

// ---------------------------------------------------------------------------
// User-data persistence: notes, highlights, and collections survive a full
// engine restart when a store path is set (they ride the same JSON file as
// the bibles, schema 2) and stay in memory without one — the parity The
// Table's library has had all along (docs/specs/24 §User Data).
// ---------------------------------------------------------------------------
void TestBibleUserDataPersist() {
    auto& fs = bps::platform::PlatformAccessor::Get().Filesystem();
    const std::string root = "/tmp/bps_bible_userdata";
    const std::string store = root + "/bibles.json";
    (void)fs.RemoveAll(root);

    auto& eng = bb::BibleEngine::Instance();
    const char* xml =
        "<bible abbrev=\"PRZ\" name=\"Userdata Bible\">"
        "<book bnum=\"1\" bsname=\"GEN\" bname=\"Genesis\">"
        "<chapter number=\"1\">"
        "<verse number=\"1\">In the beginning God created the heaven and the earth.</verse>"
        "<verse number=\"2\">And the earth was without form and void.</verse>"
        "<verse number=\"3\">And God said, Let there be light.</verse>"
        "</chapter></book></bible>";

    CHECK(eng.Initialize().ok());
    CHECK(eng.Start().ok());
    eng.SetStorePath(store);
    CHECK(eng.Import(xml, "xml", bb::ImportOptions{std::string("PRZ"), std::string(), false, false}).ok());

    auto noteRef = eng.ResolveReference("Genesis 1:3", "PRZ");
    auto hiRef = eng.ResolveReference("Genesis 1:1", "PRZ");
    auto colRef = eng.ResolveReference("Genesis 1:2", "PRZ");
    CHECK(noteRef.ok() && hiRef.ok() && colRef.ok());
    CHECK(eng.AddNote("PRZ", noteRef.value(), "preach here").ok());
    CHECK(eng.SetHighlight("PRZ", hiRef.value(), true).ok());
    CHECK(eng.AddCollection("Advent").ok());
    CHECK(eng.AddToCollection("Advent", colRef.value()).ok());

    // The user-data save rewrites the ONE store file, so the bible itself
    // must still read back (the disk-merge kept its bibles array).
    CHECK(fs.Exists(store));
    CHECK(eng.BibleCount() == 1);
    CHECK(eng.Metadata("PRZ").ok() && eng.Metadata("PRZ").value().name == "Userdata Bible");

    // ---- full restart: user data comes back with the translation ----
    CHECK(eng.Stop().ok());
    CHECK(eng.Shutdown().ok());
    CHECK(eng.Initialize().ok());
    CHECK(eng.Start().ok());
    CHECK(eng.BibleCount() == 1);
    auto notes = eng.Notes("PRZ", noteRef.value());
    CHECK(notes.ok() && notes.value().size() == 1);
    CHECK(notes.ok() && notes.value().front().text == "preach here");
    CHECK(notes.ok() && notes.value().front().id.starts_with("note-"));
    // The whole-bible roster (the notes drawer's source) sees it too.
    auto roster = eng.Notes("PRZ");
    CHECK(roster.ok() && roster.value().size() == 1);
    auto highlights = eng.Highlights("PRZ");
    CHECK(highlights.ok() && highlights.value().size() == 1);
    CHECK(highlights.ok() && highlights.value().front().bookId == "GEN"
          && highlights.value().front().verseStart == 1);
    auto col = eng.Collection("Advent");
    CHECK(col.ok() && col.value().size() == 1);
    CHECK(col.ok() && col.value().front().bookId == "GEN" && col.value().front().chapter == 1
          && col.value().front().verseStart == 2);

    // ---- removals persist too: a deleted note stays deleted ----
    CHECK(eng.RemoveNote("PRZ", noteRef.value()).ok());
    CHECK(eng.Notes("PRZ", noteRef.value()).value().empty());
    CHECK(eng.SetHighlight("PRZ", hiRef.value(), false).ok());
    CHECK(eng.Stop().ok());
    CHECK(eng.Shutdown().ok());
    CHECK(eng.Initialize().ok());
    CHECK(eng.Start().ok());
    CHECK(eng.Notes("PRZ", noteRef.value()).value().empty());
    CHECK(eng.Notes("PRZ").value().empty());
    CHECK(eng.Highlights("PRZ").value().empty());
    CHECK(eng.Collection("Advent").value().size() == 1);   // untouched survives

    // ---- no store path = no persistence (the unit-test default) ----
    eng.SetStorePath("");
    CHECK(eng.Reset().ok());
    CHECK(eng.Import(xml, "xml", bb::ImportOptions{std::string("PRZ"), std::string(), false, false}).ok());
    CHECK(eng.AddNote("PRZ", noteRef.value(), "ephemeral").ok());
    CHECK(eng.Stop().ok());
    CHECK(eng.Shutdown().ok());
    CHECK(eng.Initialize().ok());
    CHECK(eng.Start().ok());
    CHECK(eng.BibleCount() == 0);
    CHECK(eng.Notes("PRZ").value().empty());

    CHECK(eng.Stop().ok());
    CHECK(eng.Shutdown().ok());
    (void)fs.RemoveAll(root);
}

void TestBibleOutline() {
    auto& eng = bb::BibleEngine::Instance();
    CHECK(eng.Initialize().ok());
    CHECK(eng.Start().ok());
    const char* xml =
        "<bible abbrev=\"TST\" name=\"Test Bible\">"
        "<book bnum=\"43\" bsname=\"JHN\" bname=\"John\">"
        "<chapter number=\"10\"><verse number=\"1\">ten one</verse><verse number=\"2\">ten two</verse></chapter>"
        "<chapter number=\"2\"><verse number=\"1\">two one</verse></chapter>"
        "</book>"
        "<book bnum=\"1\" bsname=\"GEN\" bname=\"Genesis\">"
        "<chapter number=\"1\"><verse number=\"1\">In the beginning</verse><verse number=\"2\">and void</verse><verse number=\"3\">let there be light</verse></chapter>"
        "</book></bible>";
    auto id = eng.Import(xml, "xml", bb::ImportOptions{std::string("TST"), std::string(), false, true});
    CHECK(id.ok());

    auto meta = eng.Metadata("TST");
    CHECK(meta.ok() && meta.value().name == "Test Bible");
    CHECK(!eng.Metadata("NOPE").ok());

    auto outline = eng.Outline("TST");
    CHECK(outline.ok() && outline.value().size() == 2);
    if (outline.ok() && outline.value().size() == 2) {
        const auto& all = outline.value();
        auto byId = [&](const char* id) { return std::find_if(all.begin(), all.end(), [&](const bb::BookOutline& o) { return o.book.id == id; }); };
        auto gen = byId("GEN"), jhn = byId("JHN");
        CHECK(gen != all.end() && gen->chapters == std::vector<int>{1} && gen->verseCounts == std::vector<int>{3});
        CHECK(jhn != all.end() && jhn->chapters == std::vector<int>({2, 10}));   // numeric order, not "10" before "2"
        CHECK(jhn != all.end() && jhn->verseCounts == std::vector<int>({1, 2}));
    }
    CHECK(!eng.Outline("NOPE").ok());

    auto chapter = eng.GetChapter("TST", "JHN", 10);
    CHECK(chapter.ok() && chapter.value().verses.size() == 2 && chapter.value().verses[1].text == "ten two");
    CHECK(!eng.GetChapter("TST", "JHN", 3).ok() && !eng.GetChapter("NOPE", "JHN", 10).ok());
    CHECK(eng.RemoveBible("TST").ok());
}

// ---------------------------------------------------------------------------
// Scripture on slides (FreeShow's rules): a template with placeholders, filled with the picked verses.
// ---------------------------------------------------------------------------
void TestBookFill() {
    namespace pf = bps::presentation;
    auto text = [](std::string body, std::string bind = {}, double fontSize = 60) {
        pf::ContentBlock b;
        b.kind = "text";
        b.text = std::move(body);
        b.bind = std::move(bind);
        b.width = 1810; b.height = 835;
        b.metaJson = std::format(R"({{"fontSize":{}}})", fontSize);
        return b;
    };
    const std::vector<pf::ContentBlock> tmpl = {
        text("{scripture_number} {scripture_text}"), text("{scripture_reference}"), text("{scripture_name}"), text("{scripture_reference_last}"),
    };
    auto verse = [](int n, std::string t) { return pf::BookItem{ n, std::move(t) }; };
    pf::BookSource src;
    src.family = pf::ContentFamily::Scripture;      // named, never inferred (BookFill::ContentFamily)
    src.versionName = "King James Version (KJV)"; src.book = "Genesis"; src.bookAbbr = "GEN"; src.chapter = 1;
    src.copyright = "Public domain.";
    src.items = { verse(1, "In the beginning God created the heaven and the earth."), verse(2, "And the earth was without form, and void."), verse(3, "And God said, Let there be light.") };

    // ---- references ----
    CHECK(pf::NumberRange({ 1, 2, 3, 5 }) == "1-3, 5" && pf::NumberRange({ 7 }) == "7" && pf::NumberRange({ 5, 1, 2 }) == "1-2, 5");
    CHECK(pf::BookReference("Genesis", 1, { 1, 2, 3 }, pf::ContentFamily::Scripture) == "Genesis 1:1-3"
          && pf::BookReference("Genesis", 1, {}, pf::ContentFamily::Scripture) == "Genesis 1");
    CHECK(pf::HasPlaceholders(tmpl) && !pf::HasPlaceholders({ text("just words", "text") }));

    // ---- a preview: everything picked, on one slide ----
    auto one = pf::FillBookSlides(tmpl, src, {}, /*onlyFirst=*/true);
    CHECK(one.size() == 1);
    if (!one.empty()) {
        const auto& b = one[0].blocks;
        CHECK(b[0].text == "1 In the beginning God created the heaven and the earth. 2 And the earth was without form, and void. 3 And God said, Let there be light.");
        CHECK(b[1].text == "Genesis 1:1-3" && b[2].text == "King James Version" && b[3].text == "Genesis 1:1-3");
        CHECK(one[0].reference == "Genesis 1:1-3" && one[0].blocks.size() == tmpl.size());
    }
    CHECK(pf::FillBookSlides(tmpl, {}, {}).empty());   // nothing picked

    // ---- the options ----
    pf::BookFillOptions noNumbers; noNumbers.numbers = false;
    CHECK(pf::FillBookSlides(tmpl, src, noNumbers, true)[0].blocks[0].text.rfind("In the beginning", 0) == 0);
    pf::BookSource single = src; single.items = { src.items[1] };
    // tmpl reserves a number slot ({scripture_number}), so the number stays even though
    // the reference also names the verse (the slot is the author's explicit request).
    CHECK(pf::FillBookSlides(tmpl, single, {}, true)[0].blocks[0].text == "2 And the earth was without form, and void.");
    CHECK(pf::FillBookSlides(tmpl, single, {}, true)[0].blocks[1].text == "Genesis 1:2");
    // without a reserved number slot the de-dup still fires (the reference says it)
    CHECK(pf::FillBookSlides({ text("{scripture_text}"), text("{scripture_reference}") }, single, {}, true)[0].blocks[0].text == "And the earth was without form, and void.");
    // and in a template that does not name the verse at all the number always stays
    CHECK(pf::FillBookSlides({ text("{scripture_text}") }, single, {}, true)[0].blocks[0].text == "2 And the earth was without form, and void.");
    // a template that RESERVES a number slot ({scripture_number}) keeps the number even
    // for a single verse with the verse in the reference — the marker is the author's
    // explicit "render the number here" request (the old heuristic suppressed it and the
    // number vanished everywhere)
    CHECK(pf::FillBookSlides({ text("{scripture_number} {scripture_text}"), text("{scripture_reference}") }, single, {}, true)[0].blocks[0].text == "2 And the earth was without form, and void.");
    // the other Bibles of a parallel set are blank, {scripture_number} is only a style marker
    CHECK(pf::FillBookSlides({ text("{scripture2_text}|{scripture1_name}|{scripture_number}x") }, src, {}, true)[0].blocks[0].text == "|King James Version|x");

    // ---- {table_code}: the citation's code alone, and The Table's only ----
    // A sermon is cited by its date code; a template with room for a date code
    // has nowhere to put the whole citation line {scripture_book} writes. Blank
    // for Scripture (a verse has no date code), and a template whose ONLY
    // placeholder is the code still counts as one the engine fills.
    pf::BookSource sermon = src;
    sermon.book = "47-0412 - Faith Is The Substance";
    sermon.code = "47-0412";
    sermon.family = pf::ContentFamily::Table;
    sermon.chapter = 0;   // a fact about the data: a sermon has no chapter
    CHECK(pf::FillBookSlides({ text("{table_code}") }, sermon, {}, true)[0].blocks[0].text == "47-0412");
    CHECK(pf::FillBookSlides({ text("Code {table_code}") }, sermon, {}, true)[0].blocks[0].text == "Code 47-0412");
    // The code and the paragraphs on THIS slide, the way a printed reference
    // writes them (the title has its own token, {table_title}).
    CHECK(pf::FillBookSlides({ text("{table_code_paragraphs}") }, sermon, {}, true)[0].blocks[0].text == "47-0412 1-3");
    CHECK(pf::FillBookSlides({ text("{table_code_paragraphs}") }, src, {}, true)[0].blocks[0].text == "1-3");
    CHECK(pf::FillBookSlides({ text("{table_code}") }, src, {}, true)[0].blocks[0].text.empty());
    CHECK(pf::HasPlaceholders({ text("{table_code}") }));
    CHECK(pf::HasPlaceholders({ text("{meta_copyright}") }));
    CHECK(!pf::HasPlaceholders({ text("just words", "text") }));

    // ---- sharing verses out over slides ----
    pf::BookSource five = src;
    five.items.clear();
    for (int i = 1; i <= 5; ++i) five.items.push_back(verse(i, "Verse number " + std::to_string(i) + " text."));
    pf::BookFillOptions fixed; fixed.maxPerSlide = 2;
    auto slides = pf::FillBookSlides(tmpl, five, fixed);
    CHECK(slides.size() == 3);
    if (slides.size() == 3) {
        CHECK(slides[0].reference == "Genesis 1:1-2" && slides[1].reference == "Genesis 1:3-4" && slides[2].reference == "Genesis 1:5");
        CHECK(slides[0].blocks[3].text.empty() && slides[1].blocks[3].text.empty() && slides[2].blocks[3].text == "Genesis 1:1-5");   // {scripture_reference_last}: the last slide only
        CHECK(slides[0].blocks[1].text == "Genesis 1:1-2" && slides[2].title == "Genesis 1:5");
    }
    five.items.pop_back();   // four verses, three to a slide would be 2 slides: shared evenly, 2 + 2
    fixed.maxPerSlide = 3;
    auto even = pf::FillBookSlides(tmpl, five, fixed);
    CHECK(even.size() == 2 && even[0].reference == "Genesis 1:1-2" && even[1].reference == "Genesis 1:3-4");
    // THE TEMPLATE'S TEXT BOX NO LONGER DECIDES ANYTHING. It used to be a second,
    // tighter budget (its estimated character capacity over six), which is how
    // "Max words per slide: 40" silently produced four-word parts. The box's
    // geometry is deliberately the same as the old smart-split check's small one:
    // the slides a fill produces must not change with it any more. (`five` is the
    // four-verse set left after the EVEN-share check above.)
    std::vector<pf::ContentBlock> smallBox = { text("{scripture_text}", {}, 60) };
    smallBox[0].width = 400; smallBox[0].height = 200;       // a box the old rule measured as ~2 lines
    std::vector<pf::ContentBlock> bigBox = { text("{scripture_text}", {}, 60) };
    bigBox[0].width = 1600; bigBox[0].height = 900;
    CHECK(pf::FillBookSlides(smallBox, five, {}).size() == pf::FillBookSlides(bigBox, five, {}).size());
    CHECK(pf::FillBookSlides(smallBox, five, {}).size() == 2);   // Scripture's own cap of 3, box or no box
    pf::BookSource fiveVerses = five;
    fiveVerses.items.push_back(verse(5, "Verse number 5 text."));
    // The packing cap is a HARD CAP, and it is no longer a control: Scripture's
    // number comes from the caller (AppSettings gives it three), so five verses
    // still split 3 + 2 rather than taking all five.
    auto capped = pf::FillBookSlides(tmpl, fiveVerses, {});   // big box, Scripture's cap of 3
    CHECK(capped.size() == 2 && capped[0].reference == "Genesis 1:1-3" && capped[1].reference == "Genesis 1:4-5");
    // ...and THE TABLE takes one paragraph to a slide: the same five items take a
    // slide each, whatever the box holds (a sermon's paragraph is a whole thought).
    pf::BookFillOptions oneEach; oneEach.maxPerSlide = 1;
    auto paragraphs = pf::FillBookSlides(tmpl, fiveVerses, oneEach);
    CHECK(paragraphs.size() == 5 && paragraphs[0].reference == "Genesis 1:1" && paragraphs[4].reference == "Genesis 1:5");
    pf::BookFillOptions loose; loose.maxPerSlide = 100;  // max raised out of the way: nothing else caps the slide
    CHECK(pf::FillBookSlides(tmpl, five, loose).size() == 1);

    // ---- long verses divided ----
    pf::BookSource longOne = src;
    longOne.items = { verse(1, "one two three four five six seven eight nine ten eleven twelve") };
    // The budget is in WORDS ("max words per slide"), not characters, and it is
    // THE rule. Twelve words with a four-word budget is three parts of four — the
    // even share and "fill each part" are the same answer when the text divides
    // exactly.
    pf::BookFillOptions split; split.splitLongItems = true; split.wordsPerSlide = 4; split.maxPerSlide = 1;
    auto parts = pf::FillBookSlides({ text("{scripture_text}") }, longOne, split);
    CHECK(parts.size() == 3);
    if (!parts.empty()) CHECK(parts[0].blocks[0].text == "1 one two three four" && parts[1].blocks[0].text.rfind("five", 0) == 0);   // the number only on the first part
    if (parts.size() == 3) {
        // Exactly the budget per part — the old rule's "tolerance" could not be
        // predicted from the number on screen, and is gone.
        CHECK(parts[0].blocks[0].text == "1 one two three four");
        CHECK(parts[1].blocks[0].text == "five six seven eight");
        CHECK(parts[2].blocks[0].text == "nine ten eleven twelve");
    }
    // NO LEFTOVER PARTS. The rule is "the fewest parts the budget allows, holding
    // as close to the same number of words as they can" — a report put it as "find
    // a way to treat this so we don't always have all this three words remainder",
    // with a paragraph that came out as one full slide and one holding "can be
    // done.". Thirteen words at a four-word budget is four parts (four three-word
    // parts, not three of four with a stray word pushed onto the last): 4 + 3 + 3 + 3,
    // and 4 + 4 + 4 + 1 is exactly the shape that does not exist any more.
    pf::BookSource thirteen = src;
    thirteen.items = { verse(1, "one two three four five six seven eight nine ten eleven twelve thirteen") };
    auto tail = pf::FillBookSlides({ text("{scripture_text}") }, thirteen, split);
    CHECK(tail.size() == 4);
    if (tail.size() == 4) {
        CHECK(tail[0].blocks[0].text == "1 one two three four");
        CHECK(tail[1].blocks[0].text == "five six seven");
        CHECK(tail[2].blocks[0].text == "eight nine ten");
        CHECK(tail[3].blocks[0].text == "eleven twelve thirteen");
    }
    // The reported shape itself: a 43-word paragraph at a 40-word budget is two
    // parts of 22 and 21 — NOT 40 + 3. Every part is base or base+1 of
    // total/parts, so the budget is respected AND no part is the remainder.
    {
        std::string words43;
        for (int n = 1; n <= 43; ++n) {
            if (!words43.empty()) words43 += ' ';
            words43 += "w" + std::to_string(n);
        }
        pf::BookSource paragraph = src;
        paragraph.items = { verse(4, words43) };
        pf::BookFillOptions wide = split;
        wide.wordsPerSlide = 40;
        auto even = pf::FillBookSlides({ text("{scripture_text}") }, paragraph, wide);
        CHECK(even.size() == 2);
        if (even.size() == 2) {
            const std::string first = even[0].blocks[0].text;
            const std::string second = even[1].blocks[0].text;
            CHECK(first.rfind("4 w1 ", 0) == 0);                   // the number, then the words
            CHECK(first.find(" w22") != std::string::npos);        // 22 words on the first part
            CHECK(first.find(" w23") == std::string::npos);
            CHECK(second.rfind("w23 ", 0) == 0);                   // and 21 on the second
            CHECK(second.find(" w43") != std::string::npos);       // right to the end — not a three-word tail
        }
    }
    split.splitSuffix = true;
    auto lettered = pf::FillBookSlides({ text("{scripture_text}") }, longOne, split);
    CHECK(lettered.size() >= 3 && lettered[0].blocks[0].text.rfind("1a ", 0) == 0 && lettered[1].blocks[0].text.rfind("1b ", 0) == 0);
    CHECK(lettered[0].reference == "Genesis 1:1" && lettered[1].reference == "Genesis 1:1");

    // ---- an old-style template (no placeholders): its bound blocks get the verses ----
    auto old = pf::FillBookSlides({ text("Title", "text"), text("Ref", "ref"), text("Static") }, src, {}, true);
    CHECK(old.size() == 1 && old[0].blocks[0].text.rfind("1 In the beginning", 0) == 0 && old[0].blocks[1].text == "Genesis 1:1-3" && old[0].blocks[2].text == "Static");

    // ---- THE FAMILY IS NAMED, NOT INFERRED FROM A MISSING CHAPTER ----
    //
    // A Bible writes its chapter and joins its items with a colon; a citation
    // writes no chapter (its `book` IS the citation line) and joins with a space.
    // The JOINER follows the FAMILY, not the chapter — the old `chapter > 0` test
    // made "chapter zero" a hidden synonym for "a sermon", so a Bible-shaped
    // selection that happened to have no chapter (or a sermon whose citation came
    // with a number) produced the other family's reference.
    CHECK(pf::BookReference("47-0412 - Faith Is The Substance", 0, { 1, 2, 3 }, pf::ContentFamily::Table)
          == "47-0412 - Faith Is The Substance 1-3");
    CHECK(pf::BookReference("Genesis", 0, { 1, 2, 3 }, pf::ContentFamily::Scripture) == "Genesis:1-3");
    // a sermon's citation never grows a chapter, even when a caller sets one
    CHECK(pf::BookReference("47-0412 - Faith", 5, { 2 }, pf::ContentFamily::Table) == "47-0412 - Faith 2");

    // The three BIBLE-ONLY facts answer empty on a sermon whatever the caller put
    // in them (this is what kept the tab's own brand string, which used to be
    // assigned to `versionName`, out of {scripture_name} — a slot that means
    // "King James Version") ...
    const std::vector<pf::ContentBlock> facts = { text("N={scripture_name}|A={scripture_book_abbr}|C={scripture_chapter}") };
    CHECK(pf::FillBookSlides(facts, sermon, {}, true)[0].blocks[0].text == "N=|A=|C=");
    // ...and they DO answer for a Bible carrying the very same fields.
    CHECK(pf::FillBookSlides(facts, src, {}, true)[0].blocks[0].text == "N=King James Version|A=GEN|C=1");

    // The single-item de-duplication (the reference already names the one picked
    // item, so its leading number would say it twice) used to list only the
    // scripture spellings, so the same single-paragraph pick kept a redundant
    // leading number under The Table and dropped it under Scripture. One rule, one
    // answer, both families.
    pf::BookSource oneParagraph = sermon;
    oneParagraph.items = { verse(3, "I trust that sometime when I come back again to Oakland") };
    const auto tableSingle = pf::FillBookSlides({ text("{table_text}"), text("{table_reference}") }, oneParagraph, {}, true);
    CHECK(tableSingle[0].blocks[0].text == "I trust that sometime when I come back again to Oakland");
    CHECK(tableSingle[0].blocks[1].text == "47-0412 - Faith Is The Substance 3");

    // NOTHING IS MEASURED FROM THE TEMPLATE'S TEXT BOX ANY MORE. The box was a
    // SECOND budget (its estimated capacity over six), which is how "Max words per
    // slide: 40" silently became four words a slide; and because the estimate read
    // the body slot's geometry, the same design split differently depending only on
    // which spelling its author typed ({scripture_text} vs {scripture2_text}). Both
    // are the same non-question now: the words-per-slide number alone decides, so a
    // selection inside the budget fills the same however the box is spelled.
    pf::BookFillOptions noBoxCap; noBoxCap.maxPerSlide = 100;   // the cap out of the way: the word budget is all that is left
    std::vector<pf::ContentBlock> secondBible = smallBox;
    secondBible[0].text = "{scripture2_text}";
    CHECK(pf::FillBookSlides(smallBox, five, noBoxCap).size() == 1);        // ~16 words, inside the default 40
    CHECK(pf::FillBookSlides(secondBible, five, noBoxCap).size() == 1);     // ...in its second-Bible spelling too

    // ---- EVERY FIELD THE PALETTE OFFERS SHOWS A VALUE ON ITS OWN FAMILY ----
    //
    // The menu (MetadataFields.qml) offers exactly these tokens, and this is the
    // check that each one a slide can carry renders something: a field the engine
    // resolves to EMPTY on the family that owns it is a field that looks
    // implemented in the menu and shows nothing on the slide — which is how
    // {meta_copyright} and {scripture_book_abbr} behaved on a sermon while there
    // was no way to say "not applicable" rather than "nobody filled this in".
    // The two fixtures are filled the way the two services fill them.
    src.copyright = "Public domain.";
    sermon.year = "1947";
    sermon.title = "Faith Is The Substance";
    const auto field = [&](const char* token, const pf::BookSource& from) {
        return pf::FillBookSlides({ text(std::string("{") + token + "}") }, from, {}, true)[0].blocks[0].text;
    };
    for (const char* token : { "scripture_text", "scripture_verses", "scripture_reference", "scripture_reference_full",
                               "scripture_reference_last", "scripture_book", "scripture_book_abbr", "scripture_chapter",
                               "scripture_name", "meta_copyright" }) {
        const std::string got = field(token, src);
        CHECK(!got.empty() && got.find('{') == std::string::npos);   // a real value, never the raw token
    }
    for (const char* token : { "table_text", "table_paragraphs", "table_reference", "table_reference_full",
                               "table_reference_last", "table_citation", "table_code", "table_code_paragraphs",
                               "table_year", "table_title" })
        CHECK(!field(token, sermon).empty());
    // {scripture_number} / {table_number} are STYLE MARKERS, not values: they
    // resolve to nothing on purpose (the engine draws the number separately), which
    // is why they are not in the lists above.
    CHECK(field("scripture_number", src).empty() && field("table_number", sermon).empty());

    // ---- the registry's boot check asks the ENGINE this -------------------------
    //
    // MetadataFields.qml's palette offers these tokens per family and validates
    // itself against the engine at load, so a chip the fill cannot answer for is
    // reported instead of rendering nothing. The engine's half of that question is
    // pinned here — the YESes a chip needs, and the NOs that keep a wrong chip out
    // of the menu (a Bible-only fact asked of the sermon family), plus the NO that
    // catches a MISSPELT token, which the fill blanks rather than leaking braces.
    for (const char* token : { "scripture_text", "scripture_verses", "scripture_reference", "scripture_reference_full",
                               "scripture_reference_last", "scripture_book", "scripture_book_abbr", "scripture_chapter",
                               "scripture_name", "meta_copyright", "scripture_number" })
        CHECK(pf::FamilyAnswersToken(pf::ContentFamily::Scripture, token));
    for (const char* token : { "table_text", "table_paragraphs", "table_reference", "table_reference_full",
                               "table_reference_last", "table_citation", "table_code", "table_code_paragraphs",
                               "table_year", "table_title", "table_number" })
        CHECK(pf::FamilyAnswersToken(pf::ContentFamily::Table, token));
    CHECK(!pf::FamilyAnswersToken(pf::ContentFamily::Table, "scripture_name")
          && !pf::FamilyAnswersToken(pf::ContentFamily::Table, "scripture_book_abbr")
          && !pf::FamilyAnswersToken(pf::ContentFamily::Table, "scripture_chapter"));
    CHECK(!pf::FamilyAnswersToken(pf::ContentFamily::Scripture, "nonsense_token")
          && !pf::FamilyAnswersToken(pf::ContentFamily::Table, "nonsense_token")
          && !pf::FamilyAnswersToken(pf::ContentFamily::Table, "table_code_paragraph")   // a typo, not a key
          && !pf::FamilyAnswersToken(pf::ContentFamily::Scripture, "scripture_texts"));
}
