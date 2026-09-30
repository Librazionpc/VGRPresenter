// Unit tests: the TheTableLibrary — the sermon reference library behind
// "The Table" (books = years, chapters = sermons, verses = paragraphs).
// Drives the REAL module: txt import, PDF import (a real minimal PDF with an
// uncompressed content stream, built in-test), persistence round-trip,
// chapter lookup and search. The library file lives in /tmp so the user's
// real library is never touched.
//   ./bps_unit_tests table
#include "TestHarness.hpp"

#include "modules/library/TheTableLibrary.hpp"
#include "modules/search/SearchEngine.hpp"

#include <cstdio>
#include <filesystem>
#include <fstream>

namespace lib = bps::library;

namespace {
// A Windows-real temp dir (Git-Bash's /tmp is a virtual path the engine's
// file layer cannot open for writing).
const std::string kRoot =
    (std::filesystem::temp_directory_path() / "bps_table_test").generic_string();

std::string PathOf(const std::string& name) { return kRoot + "/" + name; }

void WriteFileBytes(const std::string& path, const std::string& data) {
    std::filesystem::create_directories(kRoot);
    std::ofstream out(path, std::ios::binary);
    out.write(data.data(), static_cast<std::streamsize>(data.size()));
}

// A minimal single-page PDF with an UNCOMPRESSED content stream — every
// parser path (string harvesting, Td line breaks) exercised without zlib.
// Lines inside one text object are HARD-WRAPPED source lines: our extractor
// joins them into one paragraph (exactly what the real sermon PDFs need —
// their line breaks are visual, not paragraph breaks).
std::string MinimalPdf(const std::string& line1, const std::string& line2) {
    std::string stream;
    stream += "BT /F1 12 Tf 72 720 Td (" + line1 + ") Tj 0 -20 Td (" + line2 + ") Tj ET\n";
    std::string pdf = "%PDF-1.4\n";
    pdf += "1 0 obj << /Length " + std::to_string(stream.size()) + " >>\nstream\n";
    pdf += stream;
    pdf += "endstream\nendobj\n";
    pdf += "trailer << /Root 1 0 R >>\n%%EOF";
    return pdf;
}

// A PDF of several pages (one content stream each) that all carry a running head ("The Spoken Word"), a "Page N" footer and a bare
// page number, around one changing body line - what the clean-up stage has to take out.
std::string RunningHeadPdf(int pages) {
    std::string pdf = "%PDF-1.4\n";
    for (int p = 1; p <= pages; ++p) {
        std::string stream = "BT /F1 12 Tf 72 760 Td (The Spoken Word) Tj ET\n";
        static const char* const kWords[] = { "alpha", "bravo", "charlie", "delta", "echo", "foxtrot", "golf", "hotel" };
        stream += "BT /F1 12 Tf 72 700 Td (Body " + std::string(kWords[p % 8]) + " of the sermon carries its own words and ends here.) Tj ET\n";
        stream += "BT /F1 12 Tf 72 40 Td (Page " + std::to_string(p) + ") Tj ET\n";
        stream += "BT /F1 12 Tf 300 20 Td (" + std::to_string(p) + ") Tj ET\n";
        pdf += std::to_string(p) + " 0 obj << /Length " + std::to_string(stream.size()) + " >>\nstream\n" + stream + "endstream\nendobj\n";
    }
    pdf += "trailer << /Root 1 0 R >>\n%%EOF";
    return pdf;
}
} // namespace

void TestTheTableLibrary() {
    std::filesystem::remove_all(kRoot);

    auto lib = lib::TheTableLibrary::Open(PathOf("table.json"));
    CHECK(lib->Books().empty());

    // ---- txt import: year from the path, code+title from the file name ----
    auto r1 = lib->ImportSermon("downloads/1953/53_0217_Only_Believe.txt",
                                "Only believe, all things are possible to them that believe.\n\n"
                                "The second paragraph carries more words so it survives the length filter easily.");
    CHECK(r1.ok());

    // ---- PDF import (real parser path) ----
    WriteFileBytes(PathOf("53_0218_My_Angel.pdf"),
                   MinimalPdf("My angel shall go before thee this night, and the road is prepared.",
                              "And the second line of the angel sermon continues with more text content."));
    auto r2 = lib->ImportSermon("downloads/1953/53_0218_My_Angel.pdf",
                                 PathOf("53_0218_My_Angel.pdf") /* not read here: content passed in */);
    CHECK(!r2.ok());   // raw path string is not PDF bytes — the honest failure

    // Correct call: bytes as content.
    std::ifstream in(PathOf("53_0218_My_Angel.pdf"), std::ios::binary);
    std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    auto r3 = lib->ImportSermon("downloads/1953/53_0218_My_Angel.pdf", bytes);
    CHECK(r3.ok());

    // ---- shape: one book, two chapters, paragraphs as verses ----
    // The PDF's two source lines are hard-wrapped halves of ONE paragraph, so
    // its chapter is one verse; the txt's blank line splits it in two.
    auto books = lib->Books();
    CHECK(books.size() == 1);
    CHECK(books[0].name == "1953");
    CHECK(books[0].id == "Y1953");
    CHECK(books[0].chapters.size() == 2);
    CHECK(books[0].chapters[0].title == "0217 Only Believe");
    CHECK(books[0].chapters[0].verses.size() == 2);
    CHECK(books[0].chapters[0].verses[0].heading == "0217 Only Believe");
    CHECK(books[0].chapters[1].title == "0218 My Angel");
    CHECK(books[0].chapters[1].verses.size() == 1);
    CHECK(books[0].chapters[1].verses[0].text.find("My angel") == 0);
    CHECK(books[0].chapters[1].verses[0].text.find("second line") != std::string::npos);

    // ---- chapter lookup ----
    auto ch = lib->GetChapter("Y1953", 2);
    CHECK(ch.ok());
    CHECK(ch.value().verses[0].text.find("My angel") == 0);

    // ---- persistence: reopen, everything still there ----
    auto reopened = lib::TheTableLibrary::Open(PathOf("table.json"));
    auto books2 = reopened->Books();
    CHECK(books2.size() == 1);
    CHECK(books2[0].chapters.size() == 2);
    CHECK(reopened->VerseCount() == 3);

    // ---- search: all-terms matching, reference shape ----
    // The reference is the citation line ("53-0218 - My Angel 1"): year minus
    // the "19", a dash, the sermon's own date-code, the title stripped to
    // alphanumerics, then the paragraph number.
    // Search rides the Search Engine's index (one document per sermon — the
    // boot path indexes every sermon, and the library's Search() ranks
    // candidates through it). The test must mirror production: index what was
    // imported, or the candidate stage answers empty and nothing is scanned.
    CHECK(lib->IndexWithSearchEngine().ok());

    auto hits = reopened->Search("angel road");
    CHECK(hits.ok());
    CHECK(hits.value().size() == 1);
    CHECK(hits.value()[0].reference == "53-0218 - My Angel 1");
    CHECK(hits.value()[0].bookId == "Y1953");
    CHECK(hits.value()[0].chapter == 2);

    // ---- citation(): sermon-level and paragraph-level ----
    CHECK(lib->Citation("Y1953", 1, 0) == "53-0217 - Only Believe");
    CHECK(lib->Citation("Y1953", 1, 2) == "53-0217 - Only Believe 2");
    CHECK(lib->Citation("Y1953", 2, 1) == "53-0218 - My Angel 1");
    CHECK(lib->Citation("Y1953", 99).empty());   // unknown chapter
    // A code-less sermon keeps its slot empty instead of inventing one.
    auto rNoCode = lib->ImportSermon("downloads/1953/Believest Thou This.txt",
                                     "Believest thou this? We would know for sure here.\n\n"
                                     "The second paragraph stands on its own words entirely.");
    CHECK(rNoCode.ok());
    CHECK(lib->Citation("Y1953", 3, 1) == "53 - Believest Thou This 1");
    // Punctuation runs collapse to one space, alphanumerics only.
    auto rPunct = lib->ImportSermon("downloads/1947/47_0412_Faith_Is_The_Substance.txt",
                                    "Faith is the substance of things hoped for, the evidence unseen.\n\n"
                                    "The second paragraph carries more words so it survives the length filter easily.");
    CHECK(rPunct.ok());
    CHECK(lib->Citation("Y1947", 1, 3) == "47-0412 - Faith Is The Substance 3");
    // The later code-tier searches need the newer sermons indexed too (upsert,
    // same as production's re-index after an import).
    CHECK(lib->IndexWithSearchEngine().ok());

    auto miss = reopened->Search("angel unmountable");
    CHECK(miss.ok());
    CHECK(miss.value().empty());

    // ---- code-tier: a citation-CODE query names the sermon itself ----
    // (Quick search's "47-1100X" used to show only Bible rows: the tier compared
    // the typed "47-0412" against the STORED bare date-code "0412" and never
    // matched. The tier now tests every citation form: yy-code, bare code, and
    // the title's leading code-token with its trailing letter.)
    // (These run against `lib` — it holds the 47_0412 sermon; `reopened` was
    // opened before that import and must not see it.)
    auto exact = lib->Search("47-0412");
    CHECK(exact.ok() && !exact.value().empty());
    CHECK(exact.value()[0].reference == "47-0412 - Faith Is The Substance");   // sermon-level row (verse 0)
    CHECK(exact.value()[0].verse == 0);
    auto prefix = lib->Search("47-");
    CHECK(prefix.ok() && prefix.value().size() >= 1);   // the year's sermon(s), one row each
    bool found47 = false;
    for (const auto& h : prefix.value())
        found47 |= h.bookId == "Y1947" && h.verse == 0;
    CHECK(found47);
    auto lettered = lib->Search("53-0218a");   // typed letter ("x" shape) vs stored "0218"
    CHECK(lettered.ok() && !lettered.value().empty());
    CHECK(lettered.value()[0].bookId == "Y1953");
    auto bare = lib->Search("0218");
    CHECK(bare.ok() && !bare.value().empty());
    CHECK(bare.value()[0].bookId == "Y1953");
    // A bare "47" is NOT a code (Genesis 47, Luke 1:47): the tier stays out —
    // the word scan answers (or nothing does), never a sermon listing.
    auto bareYear = lib->Search("47");
    CHECK(bareYear.ok());
    for (const auto& h : bareYear.value())
        CHECK(!(h.verse == 0 && h.bookId == "Y1947"));   // no sermon-level code row
    // This test indexed its sermons into the SHARED Search Engine; the next
    // suite owns "table:*" documents and asserts a clean slate — drop ours
    // (production never shares one process across two libraries, tests do).
    CHECK(lib->UnindexFromSearchEngine().ok());

    // ---- second year becomes a second book ----
    auto r4 = reopened->ImportSermon("downloads/1954/54_0101_New_Year.txt",
                                     "A new year sermon with enough words in its first paragraph to stand as a verse.\n\n"
                                     "And the closing paragraph also carries more than the forty characters needed.");
    CHECK(r4.ok());
    auto books3 = reopened->Books();
    CHECK(books3.size() == 2);
    CHECK(books3[1].name == "1954");
    auto saved = reopened->Save();
    CHECK(saved.ok());

    // ---- folder import: a year tree in one sweep, duplicates skipped ----
    const std::string folder = kRoot + "/downloads";
    std::filesystem::create_directories(folder + "/1955");
    WriteFileBytes(folder + "/1955/55_0101_First.txt",
                   "The first folder sermon opens with a paragraph long enough to survive the forty character filter.\n\n"
                   "And its second paragraph also carries more than the forty characters needed to stand.");
    WriteFileBytes(folder + "/1955/55_0102_Second.txt",
                   "The second folder sermon also opens with a paragraph long enough to survive the length filter here.\n\n"
                   "Its closing paragraph likewise carries more than the forty characters needed to stand as one.");
    auto batch = lib::TheTableLibrary::Open(PathOf("table.json"));
    auto imported = batch->ImportFolder(folder);
    CHECK(imported.ok());
    CHECK(imported.value().imported == 2);
    CHECK(imported.value().skipped == 0);
    CHECK(batch->Books().size() == 3);

    // Re-running the same folder imports nothing new (year + title dedup).
    auto again = batch->ImportFolder(folder);
    CHECK(again.ok());
    CHECK(again.value().imported == 0);
    CHECK(again.value().skipped == 2);
    CHECK(batch->Books().size() == 3);

    // The extension's case does not matter (a "SERMON.PDF" is a sermon), and the year is the folder NEAREST the file - not one far above it.
    std::filesystem::create_directories(folder + "/1956");
    WriteFileBytes(folder + "/1956/56_0103_Upper_Case.PDF",
                   MinimalPdf("The uppercase-extension sermon has a first line long enough to pass the paragraph filter,",
                              "and a second line that carries the same paragraph on to its end."));
    std::filesystem::create_directories(folder + "/2099/1957");
    WriteFileBytes(folder + "/2099/1957/57_0104_Nested.txt",
                   "A sermon under a far-away 2099 folder still belongs to the 1957 folder beside it, as this long paragraph shows.");
    auto more = batch->ImportFolder(folder);
    CHECK(more.ok() && more.value().imported == 2 && more.value().skipped == 2);
    bool has1956 = false, has1957 = false, has2099 = false;
    for (const auto& b : batch->Books()) { has1956 |= b.name == "1956"; has1957 |= b.name == "1957"; has2099 |= b.name == "2099"; }
    CHECK(has1956 && has1957 && !has2099);

    // ---- the clean-up stage, before anything is imported ----
    // A .txt of unknown make: BOM, Windows-1252 quotes, control characters, a soft hyphen, runs of spaces and blank lines.
    {
        const std::string messy = std::string("\xEF\xBB\xBF") + "  He said,\x93" "Come\x94   unto\xAD" " me.\r\n\r\n\r\n\r\n\tSecond\x0C" "  paragraph\x01" " here.\r\n";
        auto clean = lib::TheTableLibrary::ConvertToCleanText("x/1958/58_0101_Messy.txt", messy);
        CHECK(clean.ok());
        CHECK(clean.value() == "He said,\xE2\x80\x9C" "Come\xE2\x80\x9D unto me.\n\nSecond paragraph here.");
        CHECK(!lib::TheTableLibrary::ConvertToCleanText("x/1958/58_0102_Empty.txt", " \r\n\t\r\n").ok());
        CHECK(!lib::TheTableLibrary::ConvertToCleanText("x/1958/58_0103_Bad.pdf", "not a pdf at all, sorry").ok());
    }
    // A PDF's running head, footer and page numbers are not sermon text; its body lines are.
    {
        auto clean = lib::TheTableLibrary::ConvertToCleanText("x/1958/58_0104_Heads.pdf", RunningHeadPdf(5));
        CHECK(clean.ok());
        CHECK(clean.value().find("Spoken Word") == std::string::npos);
        CHECK(clean.value().find("Page ") == std::string::npos);
        CHECK(clean.value().find("Body bravo") != std::string::npos);
        CHECK(clean.value().find("Body foxtrot") != std::string::npos);
        // the pages' own numbers (1..5) went with the footers; nothing but the body text is left
        CHECK(clean.value().find(" 1") == std::string::npos && clean.value().find("\n\n1") == std::string::npos);
    }
    // The PDFs' stand-in punctuation: "_" a dash, "^" a broken-off sentence.
    {
        auto clean = lib::TheTableLibrary::ConvertToCleanText(
            "x/1958/58_0105_Marks.pdf", MinimalPdf("We will^Some of them said after_after that it was so, and it was so.", "^me feel good to hear that, brethren."));
        CHECK(clean.ok());
        CHECK(clean.value().find("will\xE2\x80\xA6 Some") != std::string::npos);
        CHECK(clean.value().find("after\xE2\x80\x94" "after") != std::string::npos);
        CHECK(clean.value().find("^") == std::string::npos && clean.value().find("_") == std::string::npos);

        // the running head, glued with no spaces to a page number on either side, is struck; an ordinary sentence saying "the spoken
        // Word" (mixed case, spaced) is untouched.
        auto banner = lib::TheTableLibrary::ConvertToCleanText(
            "x/1958/58_0106_Banner.pdf", MinimalPdf("Coming down 2THESPOKENWORD the road, when Billy come down, he had faith in",
                                                     "the spoken Word of God, THESPOKENWORDIS what He believed with his whole heart."));
        CHECK(banner.ok());
        CHECK(banner.value().find("SPOKENWORD") == std::string::npos);
        CHECK(banner.value().find("Coming down  the road") == std::string::npos);   // no leftover double space
        CHECK(banner.value().find("Coming down the road") != std::string::npos);
        CHECK(banner.value().find("the spoken Word of God") != std::string::npos);   // the sermon's own words, left alone

        // the early sermons' inline "E-2" labels start paragraphs
        auto coded = lib::TheTableLibrary::ConvertToCleanText(
            "x/1953/53_0106_Coded.pdf", MinimalPdf("E-1 The first coded paragraph opens here and it ends with a full stop. E-2 The second one",
                                                    "follows straight on with more words, and E-3 inside a sentence is left alone."));
        CHECK(coded.ok());
        CHECK(coded.value().find("full stop.\n\nE-2 The second one follows") != std::string::npos);
        CHECK(coded.value().find("\n\nE-3") == std::string::npos);
    }
    // A sermon that numbers ITS OWN paragraphs ("2 And now...") but whose opening (verse 1) our own blank-line splitting still cut into
    // several pieces: all those pieces fold into verse 1, not become wrongly-numbered verses of their own; and a sermon that never
    // numbers its own paragraphs keeps one verse per blank-line paragraph, as before.
    {
        auto numbered = lib::TheTableLibrary::Open(PathOf("numbered.json"));
        auto r = numbered->ImportSermon("x/1959/59_0101_Opening.txt",
            "The opening carries on for a while, long enough to pass the paragraph length filter on its own here.\n\n"
            "And it keeps going in a second unlabeled block that is still part of that same opening, not a verse of its own.\n\n"
            "2 Here the sermon starts numbering its own paragraphs, and this one easily clears the length filter too.\n\n"
            "3 And a third numbered paragraph follows it, again long enough on its own to pass the same filter.");
        CHECK(r.ok());
        auto ch = numbered->GetChapter("Y1959", 1);
        CHECK(ch.ok());
        if (ch.ok()) {
            CHECK(ch.value().verses.size() == 3);
            CHECK(ch.value().verses[0].number == 1);
            CHECK(ch.value().verses[0].text.find("opening carries on") != std::string::npos);
            CHECK(ch.value().verses[0].text.find("second unlabeled block") != std::string::npos);
            CHECK(ch.value().verses[1].number == 2 && ch.value().verses[1].text.rfind("2 ", 0) == std::string::npos);
            CHECK(ch.value().verses[2].number == 3 && ch.value().verses[2].text.find("third numbered paragraph") != std::string::npos);
        }

        auto plain = lib::TheTableLibrary::Open(PathOf("plain.json"));
        auto r2 = plain->ImportSermon("x/1959/59_0102_Plain.txt",
            "A sermon with no numbering of its own carries plain paragraphs, this one long enough to pass the filter easily.\n\n"
            "And its second paragraph, likewise long enough on its own, stays its own separate verse - not merged with the first.");
        CHECK(r2.ok());
        auto ch2 = plain->GetChapter("Y1959", 1);
        CHECK(ch2.ok());
        if (ch2.ok()) {
            CHECK(ch2.value().verses.size() == 2);
            CHECK(ch2.value().verses[0].number == 1 && ch2.value().verses[1].number == 2);
        }
    }
    // The folder import keeps each PDF's converted text (in its year's folder) and reuses it - edited text is what goes in.
    {
        const std::string kept = kRoot + "/converted";
        std::filesystem::remove_all(kept);
        auto first = lib::TheTableLibrary::Open(PathOf("kept1.json"));
        auto done = first->ImportFolder(folder, {}, kept);
        CHECK(done.ok());
        const std::string txt = kept + "/v1/1956/56_0103_Upper_Case.txt";
        CHECK(std::filesystem::exists(txt));
        CHECK(!std::filesystem::exists(kept + "/v1/1955"));   // (a .txt source is already text: nothing to keep)
        {
            std::ifstream f(txt, std::ios::binary);
            const std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
            CHECK(text.find("uppercase-extension sermon") != std::string::npos);
            std::ofstream out(txt, std::ios::binary | std::ios::app);
            out << "\n\nA line added to the kept text by hand, long enough to be a paragraph of its own here.\n";
        }
        auto second = lib::TheTableLibrary::Open(PathOf("kept2.json"));
        CHECK(second->ImportFolder(folder, {}, kept).ok());
        bool edited = false;
        for (const auto& b : second->Books())
            for (const auto& c : b.chapters)
                for (const auto& v : c.verses) edited |= v.text.find("added to the kept text by hand") != std::string::npos;
        CHECK(edited);
    }

    // The index lists the years and their sermons without the words (Books() copies every paragraph of every sermon).
    const auto index = batch->BookIndex();
    const auto full = batch->Books();
    CHECK(index.size() == full.size());
    for (size_t i = 0; i < index.size() && i < full.size(); ++i) {
        CHECK(index[i].id == full[i].id && index[i].name == full[i].name && index[i].chapters.size() == full[i].chapters.size());
        for (size_t c = 0; c < index[i].chapters.size() && c < full[i].chapters.size(); ++c)
            CHECK(index[i].chapters[c].verseCount == full[i].chapters[c].verses.size() && index[i].chapters[c].title == full[i].chapters[c].title);
    }
}

// Real sermon PDFs, when the folder is on this machine (src/downloads): the ones an earlier reader could not decode - hex glyph codes
// through a ToUnicode table - must now read as text. Skipped where the folder is not there.
void TestTheTableRealPdfs() {
    const char* candidates[] = { "../src/downloads_vgr", "../../src/downloads_vgr", "src/downloads_vgr", "../../../src/downloads_vgr",
                                  "../src/downloads", "../../src/downloads", "src/downloads", "../../../src/downloads" };
    std::string root;
    for (const char* c : candidates)
        if (std::filesystem::exists(c)) { root = c; break; }
    if (root.empty()) return;

    const std::string file = (std::filesystem::temp_directory_path() / "bps_real_pdfs" / "t.json").generic_string();
    std::filesystem::create_directories(std::filesystem::path(file).parent_path());
    std::filesystem::remove(file);
    auto library = lib::TheTableLibrary::Open(file);
    const std::vector<std::string> samples = {
        "1960/60_0515E_Adoption_#1.pdf", "1962/62_0117_Presuming.pdf", "1964/64_0125_Turn_On_The_Light.pdf",
        "1953/53_0215_Jesus_Christ_The_Same_Yesterday,_Today,_And_Forever.pdf",
        "1960/60_0515E__Adoption_1.pdf", "1962/62_0117_Presuming.pdf", "1964/64_0125_Turn_On_The_Light.pdf",
        "1953/53_0215_Jesus_Christ_The_Same_Yesterday_Today_And_Forever.pdf",
    };
    for (const std::string& s : samples) {
        const std::string path = root + "/" + s;
        if (!std::filesystem::exists(path)) continue;
        std::ifstream in(path, std::ios::binary);
        const std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        // the converted text, written out to read
        if (auto clean = lib::TheTableLibrary::ConvertToCleanText(path, bytes); clean.ok()) {
            const std::string out = (std::filesystem::temp_directory_path() / "bps_real_pdfs" / (std::filesystem::path(s).stem().string() + ".txt")).generic_string();
            std::ofstream(out, std::ios::binary) << clean.value() << "\n";
            std::printf("[sermon] converted text: %s\n", out.c_str());
        }
        auto r = library->ImportSermon(path, bytes);
        std::printf("[sermon] %s -> %s\n", s.c_str(), r.ok() ? r.value().c_str() : r.error().message.c_str());
        CHECK(r.ok());
        if (!r.ok()) continue;
        const auto index = library->BookIndex();
        const auto& book = index.back();
        auto ch = library->GetChapter(book.id, book.chapters.back().number);
        CHECK(ch.ok());
        if (!ch.ok()) continue;
        std::printf("         %zu paragraphs\n", ch.value().verses.size());
        for (size_t i = 0; i < ch.value().verses.size() && i < 4; ++i)
            std::printf("         [%zu] %.160s\n", i + 1, ch.value().verses[i].text.c_str());
        CHECK(ch.value().verses.size() >= 5);
    }
    // The whole folder, when asked for (VGR_TABLE_FULL=1; it takes about half a minute): how many sermons convert, and what the rest say.
    if (std::getenv("VGR_TABLE_FULL")) {
        const std::string all = (std::filesystem::temp_directory_path() / "bps_real_pdfs" / "full.json").generic_string();
        const std::string kept = (std::filesystem::temp_directory_path() / "bps_real_pdfs" / "texts").generic_string();
        std::filesystem::remove(all);
        std::filesystem::remove_all(kept);
        auto whole = lib::TheTableLibrary::Open(all);
        auto report = whole->ImportFolder(root, {}, kept);
        CHECK(report.ok());
        if (report.ok())
            std::printf("[folder] %d imported, %d skipped, %d failed; converted texts in %s\n", report.value().imported, report.value().skipped, report.value().failed, kept.c_str());
    }
    std::fflush(stdout);
}

// User data — notes and highlights persisted with the library (the Bible
// module's AddNote/SetHighlight, keyed "bookId:chapter:verse"). The JSON
// round-trip is the point: a note must survive a full Load of the file.
void TestTheTableUserData() {
    std::filesystem::remove_all(kRoot);
    auto lib = lib::TheTableLibrary::Open(PathOf("userdata.json"));
    auto r1 = lib->ImportSermon("downloads/1953/53_0217_Only_Believe.txt",
                                "Only believe, all things are possible to them that believe.\n\n"
                                "The second paragraph carries more words so it survives the length filter easily.");
    CHECK(r1.ok());

    // A note on paragraph 1; an unknown sermon/paragraph is refused.
    CHECK(lib->AddNote("Y1953", 1, 1, "start here every time").ok());
    CHECK(!lib->AddNote("Y1953", 1, 9, "no such paragraph").ok());
    CHECK(!lib->AddNote("Y9999", 1, 1, "no such sermon").ok());
    CHECK(!lib->AddNote("Y1953", 1, 1, "").ok());
    auto one = lib->Notes("Y1953", 1, 1);
    CHECK(one.ok() && one.value().size() == 1);
    CHECK(one.ok() && one.value().front().text == "start here every time");
    CHECK(one.ok() && one.value().front().ref == "53-0217 - Only Believe 1");
    CHECK(lib->Notes("Y1953", 1, 2).ok() && lib->Notes("Y1953", 1, 2).value().empty());

    // Adding to the same paragraph REPLACES the note (one note per paragraph).
    CHECK(lib->AddNote("Y1953", 1, 1, "revised wording").ok());
    CHECK(lib->Notes("Y1953", 1, 1).ok() && lib->Notes("Y1953", 1, 1).value().size() == 1);
    CHECK(lib->Notes("Y1953", 1, 1).value().front().text == "revised wording");

    // A second note (later paragraph) sorts most-recent-first in the sweep.
    CHECK(lib->AddNote("Y1953", 1, 2, "the closing thought").ok());
    auto all = lib->Notes();
    CHECK(all.ok() && all.value().size() == 2);
    CHECK(all.ok() && all.value().front().text == "the closing thought");

    // Highlights: set, query, clear.
    CHECK(lib->SetHighlight("Y1953", 1, 1, true).ok());
    CHECK(!lib->SetHighlight("Y1953", 1, 9, true).ok());
    CHECK(lib->IsHighlighted("Y1953", 1, 1).ok() && lib->IsHighlighted("Y1953", 1, 1).value());
    CHECK(lib->IsHighlighted("Y1953", 1, 2).ok() && !lib->IsHighlighted("Y1953", 1, 2).value());
    auto marks = lib->Highlights();
    CHECK(marks.ok() && marks.value().size() == 1 && marks.value().front() == "Y1953:1:1");

    // PERSISTENCE: reopen the file from disk — notes and highlights both come back.
    auto reopened = lib::TheTableLibrary::Open(PathOf("userdata.json"));
    auto survived = reopened->Notes("Y1953", 1, 1);
    CHECK(survived.ok() && survived.value().size() == 1);
    CHECK(survived.ok() && survived.value().front().text == "revised wording");
    auto allSurvived = reopened->Notes();
    CHECK(allSurvived.ok() && allSurvived.value().size() == 2);
    auto marksSurvived = reopened->Highlights();
    CHECK(marksSurvived.ok() && marksSurvived.value().size() == 1
          && marksSurvived.value().front() == "Y1953:1:1");
    CHECK(reopened->IsHighlighted("Y1953", 1, 1).ok() && reopened->IsHighlighted("Y1953", 1, 1).value());

    // Clearing a highlight persists too.
    CHECK(reopened->SetHighlight("Y1953", 1, 1, false).ok());
    auto rechecked = lib::TheTableLibrary::Open(PathOf("userdata.json"));
    CHECK(rechecked->IsHighlighted("Y1953", 1, 1).ok() && !rechecked->IsHighlighted("Y1953", 1, 1).value());
    CHECK(rechecked->Notes().ok() && rechecked->Notes().value().size() == 2);

    // Removal: gone from memory AND from the reopened file (the delete must
    // write through, not just drop the in-memory entry).
    CHECK(rechecked->RemoveNote("Y1953", 1, 2).ok());
    CHECK(rechecked->Notes("Y1953", 1, 2).ok() && rechecked->Notes("Y1953", 1, 2).value().empty());
    CHECK(rechecked->Notes().ok() && rechecked->Notes().value().size() == 1);
    auto reagain = lib::TheTableLibrary::Open(PathOf("userdata.json"));
    CHECK(reagain->Notes().ok() && reagain->Notes().value().size() == 1);
    CHECK(reagain->Notes("Y1953", 1, 1).ok() && reagain->Notes("Y1953", 1, 1).value().front().text == "revised wording");

    std::filesystem::remove_all(kRoot);
}

// Search Engine integration (docs/specs/20 §DocumentAdapterRegistry, the Bible
// precedent): IndexWithSearchEngine puts one document per sermon into the
// platform index — type "table", upsert-safe, removable — so the app-wide
// search finds the sermons alongside everything else. Runs after the library
// tests, on a fresh library file, so the document ids are deterministic.
void TestTheTableSearchIndexing() {
    auto& eng = s::SearchEngine::Instance();
    CHECK(eng.Initialize().ok());

    // A clean slate for the document ids this test owns ("table:*") — and for
    // the library file itself, so the suite is idempotent (a previous run's
    // sermons would otherwise still be in it).
    std::filesystem::remove_all(kRoot);
    auto lib = lib::TheTableLibrary::Open(PathOf("index.json"));
    CHECK(lib->UnindexFromSearchEngine().ok());
    CHECK(lib->Books().empty());

    // Two sermons in one year book.
    CHECK(lib->ImportSermon("downloads/1953/53_0217_Only_Believe.txt",
                            "Only believe, all things are possible to them that believe in His name today.\n\n"
                            "The second paragraph carries more words so it survives the length filter easily.").ok());
    CHECK(lib->ImportSermon("downloads/1953/53_0218_My_Angel.txt",
                            "My angel shall go before thee and the road is prepared for the journey ahead of you.\n\n"
                            "Its closing paragraph also carries more than the forty characters needed to stand.").ok());

    // Indexing: one document per sermon.
    auto indexed = lib->IndexWithSearchEngine();
    CHECK(indexed.ok());
    CHECK(indexed.value() == 2);

    // The library's own documents are searchable through the engine, by text...
    auto hit = eng.Search("road is prepared");
    CHECK(hit.ok() && !hit.value().empty());
    bool foundAngel = false;
    for (const auto& r : hit.value())
        foundAngel |= r.documentId == "table:Y1953:2" && r.type == "table";
    CHECK(foundAngel);

    // ...by title (the sermon code keeps it unique)...
    auto byTitle = eng.Search("0217");
    CHECK(byTitle.ok() && !byTitle.value().empty());
    bool foundOnly = false;
    for (const auto& r : byTitle.value())
        foundOnly |= r.documentId == "table:Y1953:1";
    CHECK(foundOnly);

    // ...and through the type filter.
    s::SearchFilter tableOnly;
    tableOnly.type = "table";
    auto typed = eng.Search("paragraph", tableOnly);
    CHECK(typed.ok());
    for (const auto& r : typed.value()) CHECK(r.type == "table");

    // A re-index upserts (no duplicates), and the document content follows the library.
    CHECK(lib->IndexWithSearchEngine().ok());
    auto afterUpsert = eng.Search("road is prepared", tableOnly);
    CHECK(afterUpsert.ok());
    size_t angelDocs = 0;
    for (const auto& r : afterUpsert.value())
        angelDocs += r.documentId == "table:Y1953:2" ? 1u : 0u;
    CHECK(angelDocs == 1);

    // Other content in the index is untouched (the upsert never clobbers the rest).
    s::SearchDocument song;
    song.id = "index-test-song";
    song.type = "song";
    song.title = "Index Test Song";
    song.content = "unrelated hymn words entirely";
    CHECK(eng.IndexDocument(song).ok());
    CHECK(lib->IndexWithSearchEngine().ok());
    auto songStill = eng.Search("unrelated hymn");
    CHECK(songStill.ok() && !songStill.value().empty());

    // Unindexing drops exactly this library's documents.
    CHECK(lib->UnindexFromSearchEngine().ok());
    auto gone = eng.Search("road is prepared", tableOnly);
    CHECK(gone.ok() && gone.value().empty());
    auto songRemains = eng.Search("unrelated hymn");
    CHECK(songRemains.ok() && !songRemains.value().empty());

    CHECK(eng.Shutdown().ok());
    CHECK(eng.Initialize().ok());   // other tests re-start the engine
}
