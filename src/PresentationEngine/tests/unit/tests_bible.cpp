// Unit tests: Bible Engine (docs/specs/24).
// Split from tests/unit/main.cpp so a single phase can run alone:
//   ./bps_unit_tests bible
#include "TestHarness.hpp"

#include "modules/presentation/ScriptureSlides.hpp"
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
void TestScriptureSlides() {
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
    auto verse = [](int n, std::string t) { return pf::ScriptureVerse{ n, std::move(t) }; };
    pf::ScriptureSource src;
    src.versionName = "King James Version (KJV)"; src.book = "Genesis"; src.bookAbbr = "GEN"; src.chapter = 1;
    src.verses = { verse(1, "In the beginning God created the heaven and the earth."), verse(2, "And the earth was without form, and void."), verse(3, "And God said, Let there be light.") };

    // ---- references ----
    CHECK(pf::ScriptureVerseRange({ 1, 2, 3, 5 }) == "1-3, 5" && pf::ScriptureVerseRange({ 7 }) == "7" && pf::ScriptureVerseRange({ 5, 1, 2 }) == "1-2, 5");
    CHECK(pf::ScriptureReference("Genesis", 1, { 1, 2, 3 }) == "Genesis 1:1-3" && pf::ScriptureReference("Genesis", 1, {}) == "Genesis 1");
    CHECK(pf::HasScriptureValues(tmpl) && !pf::HasScriptureValues({ text("just words", "text") }));

    // ---- a preview: everything picked, on one slide ----
    auto one = pf::BuildScriptureSlides(tmpl, src, {}, /*onlyFirst=*/true);
    CHECK(one.size() == 1);
    if (!one.empty()) {
        const auto& b = one[0].blocks;
        CHECK(b[0].text == "1 In the beginning God created the heaven and the earth. 2 And the earth was without form, and void. 3 And God said, Let there be light.");
        CHECK(b[1].text == "Genesis 1:1-3" && b[2].text == "King James Version" && b[3].text == "Genesis 1:1-3");
        CHECK(one[0].reference == "Genesis 1:1-3" && one[0].blocks.size() == tmpl.size());
    }
    CHECK(pf::BuildScriptureSlides(tmpl, {}, {}).empty());   // nothing picked

    // ---- the options ----
    pf::ScriptureSettings noNumbers; noNumbers.verseNumbers = false;
    CHECK(pf::BuildScriptureSlides(tmpl, src, noNumbers, true)[0].blocks[0].text.rfind("In the beginning", 0) == 0);
    pf::ScriptureSettings lines; lines.versesOnIndividualLines = true;
    CHECK(pf::BuildScriptureSlides(tmpl, src, lines, true)[0].blocks[0].text.find("\n2 And") != std::string::npos);
    pf::ScriptureSource single = src; single.verses = { src.verses[1] };
    // tmpl reserves a number slot ({scripture_number}), so the number stays even though
    // the reference also names the verse (the slot is the author's explicit request).
    CHECK(pf::BuildScriptureSlides(tmpl, single, {}, true)[0].blocks[0].text == "2 And the earth was without form, and void.");
    CHECK(pf::BuildScriptureSlides(tmpl, single, {}, true)[0].blocks[1].text == "Genesis 1:2");
    // without a reserved number slot the de-dup still fires (the reference says it)
    CHECK(pf::BuildScriptureSlides({ text("{scripture_text}"), text("{scripture_reference}") }, single, {}, true)[0].blocks[0].text == "And the earth was without form, and void.");
    // and in a template that does not name the verse at all the number always stays
    CHECK(pf::BuildScriptureSlides({ text("{scripture_text}") }, single, {}, true)[0].blocks[0].text == "2 And the earth was without form, and void.");
    // a template that RESERVES a number slot ({scripture_number}) keeps the number even
    // for a single verse with the verse in the reference — the marker is the author's
    // explicit "render the number here" request (the old heuristic suppressed it and the
    // number vanished everywhere)
    CHECK(pf::BuildScriptureSlides({ text("{scripture_number} {scripture_text}"), text("{scripture_reference}") }, single, {}, true)[0].blocks[0].text == "2 And the earth was without form, and void.");
    // the other Bibles of a parallel set are blank, {scripture_number} is only a style marker
    CHECK(pf::BuildScriptureSlides({ text("{scripture2_text}|{scripture1_name}|{scripture_number}x") }, src, {}, true)[0].blocks[0].text == "|King James Version|x");

    // ---- sharing verses out over slides ----
    pf::ScriptureSource five = src;
    five.verses.clear();
    for (int i = 1; i <= 5; ++i) five.verses.push_back(verse(i, "Verse number " + std::to_string(i) + " text."));
    pf::ScriptureSettings fixed; fixed.smartSplit = false; fixed.versesPerSlide = 2;
    auto slides = pf::BuildScriptureSlides(tmpl, five, fixed);
    CHECK(slides.size() == 3);
    if (slides.size() == 3) {
        CHECK(slides[0].reference == "Genesis 1:1-2" && slides[1].reference == "Genesis 1:3-4" && slides[2].reference == "Genesis 1:5");
        CHECK(slides[0].blocks[3].text.empty() && slides[1].blocks[3].text.empty() && slides[2].blocks[3].text == "Genesis 1:1-5");   // {scripture_reference_last}: the last slide only
        CHECK(slides[0].blocks[1].text == "Genesis 1:1-2" && slides[2].title == "Genesis 1:5");
    }
    five.verses.pop_back();   // four verses, three to a slide would be 2 slides: shared evenly, 2 + 2
    fixed.versesPerSlide = 3;
    auto even = pf::BuildScriptureSlides(tmpl, five, fixed);
    CHECK(even.size() == 2 && even[0].reference == "Genesis 1:1-2" && even[1].reference == "Genesis 1:3-4");
    // smart split: a small text box holds few verses, a big one all of them
    std::vector<pf::ContentBlock> smallBox = { text("{scripture_text}", {}, 60) };
    smallBox[0].width = 400; smallBox[0].height = 200;       // ~13 characters a line, 2 lines
    CHECK(pf::BuildScriptureSlides(smallBox, five, {}).size() > 1);
    CHECK(pf::BuildScriptureSlides(tmpl, five, {}).size() == 1);

    // ---- long verses divided ----
    pf::ScriptureSource longOne = src;
    longOne.verses = { verse(1, "one two three four five six seven eight nine ten eleven twelve") };
    pf::ScriptureSettings split; split.splitLongVerses = true; split.longVersesChars = 20; split.smartSplit = false; split.versesPerSlide = 1;
    auto parts = pf::BuildScriptureSlides({ text("{scripture_text}") }, longOne, split);
    CHECK(parts.size() >= 3);
    if (!parts.empty()) CHECK(parts[0].blocks[0].text == "1 one two three four" && parts[1].blocks[0].text.rfind("five", 0) == 0);   // the number only on the first part
    split.splitLongVersesSuffix = true;
    auto lettered = pf::BuildScriptureSlides({ text("{scripture_text}") }, longOne, split);
    CHECK(lettered.size() >= 3 && lettered[0].blocks[0].text.rfind("1a ", 0) == 0 && lettered[1].blocks[0].text.rfind("1b ", 0) == 0);
    CHECK(lettered[0].reference == "Genesis 1:1" && lettered[1].reference == "Genesis 1:1");

    // ---- an old-style template (no placeholders): its bound blocks get the verses ----
    auto old = pf::BuildScriptureSlides({ text("Title", "text"), text("Ref", "ref"), text("Static") }, src, {}, true);
    CHECK(old.size() == 1 && old[0].blocks[0].text.rfind("1 In the beginning", 0) == 0 && old[0].blocks[1].text == "Genesis 1:1-3" && old[0].blocks[2].text == "Static");
}
