// Unit tests: the TheTableLibrary — the sermon reference library behind
// "The Table" (books = years, chapters = sermons, verses = paragraphs).
// Drives the REAL module: txt import, PDF import (a real minimal PDF with an
// uncompressed content stream, built in-test), persistence round-trip,
// chapter lookup and search. The library file lives in /tmp so the user's
// real library is never touched.
//   ./bps_unit_tests table
#include "TestHarness.hpp"

#include "modules/library/TheTableLibrary.hpp"

#include <filesystem>
#include <fstream>

namespace lib = bps::library;

namespace {
const std::string kRoot = "/tmp/bps_table_test";

std::string PathOf(const std::string& name) { return kRoot + "/" + name; }

void WriteFileBytes(const std::string& path, const std::string& data) {
    std::filesystem::create_directories(kRoot);
    std::ofstream out(path, std::ios::binary);
    out.write(data.data(), static_cast<std::streamsize>(data.size()));
}

// A minimal single-page PDF with an UNCOMPRESSED content stream — every
// parser path (string harvesting, Td line breaks) exercised without zlib.
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
    auto books = lib->Books();
    CHECK(books.size() == 1);
    CHECK(books[0].name == "1953");
    CHECK(books[0].id == "Y1953");
    CHECK(books[0].chapters.size() == 2);
    CHECK(books[0].chapters[0].title == "0217 Only Believe");
    CHECK(books[0].chapters[0].verses.size() == 2);
    CHECK(books[0].chapters[0].verses[0].heading == "0217 Only Believe");
    CHECK(books[0].chapters[1].title == "0218 My Angel");
    CHECK(books[0].chapters[1].verses.size() == 2);

    // ---- chapter lookup ----
    auto ch = lib->GetChapter("Y1953", 2);
    CHECK(ch.ok());
    CHECK(ch.value().verses[0].text.find("My angel") == 0);

    // ---- persistence: reopen, everything still there ----
    auto reopened = lib::TheTableLibrary::Open(PathOf("table.json"));
    auto books2 = reopened->Books();
    CHECK(books2.size() == 1);
    CHECK(books2[0].chapters.size() == 2);
    CHECK(reopened->VerseCount() == 4);

    // ---- search: all-terms matching, reference shape ----
    auto hits = reopened->Search("angel road");
    CHECK(hits.ok());
    CHECK(hits.value().size() == 1);
    CHECK(hits.value()[0].reference == "1953 2:1");
    CHECK(hits.value()[0].bookId == "Y1953");
    CHECK(hits.value()[0].chapter == 2);

    auto miss = reopened->Search("angel unmountable");
    CHECK(miss.ok());
    CHECK(miss.value().empty());

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
}
