// Unit tests: importing (FreeShow's formats, in the engine): the format list, the plain-text converter every lyric format rides on,
// CSV, FreeShow's own show files, the song providers' bridge, Bibles, and turning an imported show into a presentation.
//   ./bps_unit_tests import
#include "TestHarness.hpp"

#include <set>

#include "modules/import/ImportEngine.hpp"
#include "modules/import/SongText.hpp"
#include "modules/content/Vfs.hpp"

namespace im = bps::import;

namespace {

im::ImportFile File(std::string name, std::string ext, std::string content) {
    im::ImportFile f;
    f.name = std::move(name); f.extension = std::move(ext); f.content = std::move(content);
    return f;
}

std::vector<std::string> Groups(const im::ImportedShow& s) {
    std::vector<std::string> out;
    for (const auto& sec : s.sections) out.push_back(sec.group);
    return out;
}

} // namespace

void TestImportFormats() {
    const auto& formats = im::ImportFormats();
    CHECK(formats.size() >= 30);
    std::set<std::string> ids;
    for (const im::ImportFormat& f : formats) {
        CHECK(ids.insert(f.id).second);
        CHECK(!f.name.empty() && !f.section.empty() && !f.icon.empty());
        for (const std::string& e : f.extensions) CHECK(!e.empty() && e.find('.') == std::string::npos && std::none_of(e.begin(), e.end(), [](unsigned char c) { return std::isupper(c); }));
    }
    // every FreeShow format is there, in its section
    for (const char* id : { "freeshow", "freeshow_project", "freeshow_template", "freeshow_action", "freeshow_stage", "freeshow_theme" })
        CHECK(im::FindImportFormat(id) && im::FindImportFormat(id)->section == "freeshow");
    for (const char* id : { "lessons", "pdf", "powerpoint" }) CHECK(im::FindImportFormat(id) && im::FindImportFormat(id)->section == "media");
    for (const char* id : { "txt", "csv", "chordpro", "word", "propresenter", "easyworship", "videopsalm", "openlp", "opensong", "mediashout",
                            "quelea", "softprojector", "songbeamer", "easyslides", "verseview" })
        CHECK(im::FindImportFormat(id) && im::FindImportFormat(id)->section == "text");
    CHECK(im::FindImportFormat("nope") == nullptr);
    CHECK(im::FindImportFormat("easyworship")->tutorial.find("SongsWords.db") != std::string::npos);

    // what reads a ".csv" / ".cho" / ".xml"
    auto csv = im::FormatsForExtension("CSV");
    CHECK(csv.size() == 1 && csv[0]->id == "csv");
    bool sawChordpro = false;
    for (const auto* f : im::FormatsForExtension("cho")) sawChordpro |= f->id == "chordpro";
    CHECK(sawChordpro && im::FormatsForExtension("xml").size() >= 3);

    // a format that is not written yet, and one that does not exist
    auto missing = im::ImportFiles("mediashout", { File("a", "mdb", "x") });
    CHECK(!missing.ok() && missing.error().code == bps::Err::Unsupported);
    CHECK(!im::ImportFiles("nope", {}).ok() && im::ImportFiles("nope", {}).error().code == bps::Err::NotFound);
}

void TestSongText() {
    // ---- group headers, in every way FreeShow accepts ----
    const std::string song =
        "Title=Amazing Grace\nAuthor=John Newton\n\n"
        "[Verse 1]\nAmazing grace how sweet the sound\nThat saved a wretch like me\n\n"
        "Chorus:\nMy chains are gone\nI've been set free\nx2\n\n"
        "[Verse 2]\nT'was grace that taught my heart to fear\nAnd grace my fears relieved\n";
    im::ImportedShow show = im::ParseSongText(song);
    CHECK(show.name == "Amazing Grace" && show.meta["title"] == "Amazing Grace" && show.meta["author"] == "John Newton");
    CHECK(Groups(show) == std::vector<std::string>({ "verse", "chorus", "chorus", "verse" }));   // the chorus is repeated twice (x2)
    if (show.sections.size() == 4) {
        CHECK(show.sections[0].slides[0].text == "Amazing grace how sweet the sound\nThat saved a wretch like me");   // the header is not words
        CHECK(show.sections[1].slides[0].text == "My chains are gone\nI've been set free");                            // and neither is the x2
        CHECK(show.sections[0].label == "Verse" && show.sections[1].label == "Chorus");
    }

    // ---- a name given by the caller wins; notes after "---" ----
    im::SongTextOptions named; named.name = "My Song";
    auto withNotes = im::ParseSongText("[Verse]\nLine one\nLine two\n---\nPlay softly", named);
    CHECK(withNotes.name == "My Song" && withNotes.sections.size() == 1);
    if (!withNotes.sections.empty()) CHECK(withNotes.sections[0].slides[0].text == "Line one\nLine two" && withNotes.sections[0].slides[0].notes == "Play softly");

    // ---- notes= and unknown metadata; a URL at the end is where it came from ----
    auto meta = im::ParseSongText("[Verse]\nHello there\n\nnotes=slow\nMood=happy\nCCLI=123\n\nhttps://example.com/song");
    CHECK(meta.notes == "slow" && meta.meta["Mood"] == "happy" && meta.meta["CCLI"] == "123" && meta.meta["publisher"] == "https://example.com/song");

    // ---- chords over words are folded into the line, and kept beside it ----
    auto chorded = im::ParseSongText("[Verse]\nC       G\nAmazing grace how sweet");
    CHECK(chorded.sections.size() == 1);
    if (!chorded.sections.empty()) {
        const auto& slide = chorded.sections[0].slides[0];
        CHECK(slide.text == "Amazing grace how sweet");
        CHECK(slide.chordsJson.find("\"key\":\"C\"") != std::string::npos && slide.chordsJson.find("\"key\":\"G\"") != std::string::npos);
    }
    auto inlineChords = im::ParseSongText("[Verse]\n[C]Amazing [G]grace");
    CHECK(inlineChords.sections[0].slides[0].text == "Amazing grace" && inlineChords.sections[0].slides[0].chordsJson.find("\"pos\":8") != std::string::npos);

    // ---- limits on slide length: the rest go to child slides ----
    im::SongTextOptions split; split.splitLines = 2;
    auto long1 = im::ParseSongText("[Verse]\nl1\nl2\nl3\nl4\nl5", split);
    CHECK(long1.sections.size() == 1 && long1.sections[0].slides.size() == 3 && long1.sections[0].slides[1].text == "l3\nl4");

    // ---- a header with nothing under it is an empty slide; blank input has no sections ----
    auto empty = im::ParseSongText("[Intro]\n\n[Verse]\nWords here");
    CHECK(Groups(empty) == std::vector<std::string>({ "intro", "verse" }) && empty.sections[0].slides.size() == 1);
    CHECK(im::ParseSongText("").sections.empty());

    // ---- the helpers ----
    CHECK(im::FindGroupMatch("Verse 1") == "verse" && im::FindGroupMatch("[Chorus]") == "chorus" && im::FindGroupMatch("Pre-Chorus") == "pre_chorus");
    CHECK(im::FindGroupMatch("Bridge x2") == "bridge" && im::FindGroupMatch("Hello world").empty() && im::FindGroupMatch("").empty());
    CHECK(im::GroupLabel("pre_chorus") == "Pre-Chorus" && im::GroupLabel("verse") == "Verse" && im::GroupLabel("Coda") == "Coda");
    CHECK(im::TextSimilarity("hello", "hello") == 1.0 && im::TextSimilarity("", "") == 1.0);
    CHECK(im::TextSimilarity("hello world", "hello there") > 0.4 && im::TextSimilarity("abc", "xyz") < 0.1);
}

void TestImportFiles() {
    // ---- plain text: one show per file, named after the file when it has no title ----
    auto txt = im::ImportFiles("txt", { File("Song A", "txt", "[Verse]\nOne two three"), File("Empty", "txt", "") });
    CHECK(txt.ok() && txt.value().files == 2 && txt.value().shows.size() == 1 && txt.value().warnings.size() == 1);
    CHECK(txt.value().shows[0].name == "Song A" && txt.value().warnings[0].find("Empty") == 0);

    // ---- CSV: a slide per line, a text box (line) per field ----
    auto csv = im::ImportFiles("csv", { File("Words", "csv", "One,Two\r\n\"Three, four\",Five\r\n\r\n\"\"\"Quoted\"\"\"\r\n") });
    CHECK(csv.ok() && csv.value().shows.size() == 1);
    if (csv.ok() && csv.value().shows.size() == 1) {
        const auto& s = csv.value().shows[0];
        CHECK(s.name == "Words" && s.sections.size() == 3);
        CHECK(s.sections[0].slides[0].text == "One\nTwo" && s.sections[1].slides[0].text == "Three, four\nFive" && s.sections[2].slides[0].text == "\"Quoted\"");
    }

    // ---- FreeShow's own show file (as the show, and as [id, show]) ----
    const std::string fs =
        R"({"name":"Great Is Thy Faithfulness","category":null,"meta":{"title":"Great Is Thy Faithfulness","author":"Chisholm"},)"
        R"("settings":{"activeLayout":"L1"},)"
        R"("slides":{"a":{"group":"verse","color":null,"notes":"softly","items":[{"lines":[{"text":[{"value":"Great is Thy faithfulness"}]},{"text":[{"value":"O God my Father"}]}]}],"children":["a2"]},)"
        R"("a2":{"group":null,"items":[{"lines":[{"text":[{"value":"There is no shadow"}]}]}]},)"
        R"("b":{"group":"chorus","items":[{"lines":[{"text":[{"value":"Morning by morning"}]}]}]}},)"
        R"("layouts":{"L1":{"name":"Default","notes":"a note","slides":[{"id":"a"},{"id":"b"},{"id":"a"}]}}})";
    auto shows = im::ImportFiles("freeshow", { File("x", "show", fs), File("y", "show", "[\"id1\"," + fs + "]"), File("bad", "show", "{ nope") });
    CHECK(shows.ok() && shows.value().shows.size() == 2 && shows.value().warnings.size() == 1);
    if (shows.ok() && !shows.value().shows.empty()) {
        const auto& s = shows.value().shows[0];
        CHECK(s.name == "Great Is Thy Faithfulness" && s.meta.at("author") == "Chisholm" && s.notes == "a note");
        CHECK(Groups(s) == std::vector<std::string>({ "verse", "chorus", "verse" }));   // the layout's order, a slide can repeat
        CHECK(s.sections[0].slides.size() == 2 && s.sections[0].slides[0].text == "Great is Thy faithfulness\nO God my Father" && s.sections[0].slides[0].notes == "softly");
        CHECK(s.sections[0].slides[1].text == "There is no shadow");
    }

    // ---- the song providers the engine already had ----
    auto cho = im::ImportFiles("chordpro", { File("Grace", "cho", "{title: Amazing Grace}\n{author: John Newton}\n\n[Verse 1]\n[C]Amazing [G]grace how [C]sweet\n\n[Chorus]\nMy chains are gone\n") });
    CHECK(cho.ok() && cho.value().shows.size() == 1);
    if (cho.ok() && !cho.value().shows.empty()) {
        const auto& s = cho.value().shows[0];
        CHECK(s.name == "Amazing Grace" && s.origin == "chordpro" && s.sections.size() >= 2);
        if (s.sections.size() >= 2) {
            CHECK(s.sections[0].group == "verse" && s.sections[0].slides[0].text == "Amazing grace how sweet" && !s.sections[0].slides[0].chordsJson.empty());
            CHECK(s.sections[1].group == "chorus");
        }
    }
    auto sqlite = im::ImportFiles("openlp", { File("db", "sqlite", "x") });
    CHECK(sqlite.ok() && sqlite.value().shows.empty() && sqlite.value().warnings.size() == 1);

    // ---- FreeShow PROJECT: a zip of show files (built with the engine's own
    // ZipWriter — the same reader the import path uses must read what the
    // app's packaging produces), and the JSON-array shape (an array of
    // [id, show]). Every show inside imports in order; a broken member is
    // its own warning.
    {
        const std::string showA =
            R"({"name":"Project Song A","slides":{"a":{"group":"verse","items":[{"lines":[{"text":[{"value":"Alpha"}]}]}]}}})";
        const std::string showB =
            R"({"name":"Project Song B","slides":{"b":{"group":"chorus","items":[{"lines":[{"text":[{"value":"Beta"}]}]}]}}})";
        content::ZipWriter zw;
        CHECK(zw.AddEntry("shows/one.show", std::vector<uint8_t>(showA.begin(), showA.end())).ok());
        CHECK(zw.AddEntry("shows/two.show", std::vector<uint8_t>(showB.begin(), showB.end())).ok());
        auto zipBytes = zw.Finalize();
        CHECK(zipBytes.ok());
        if (zipBytes.ok()) {
            auto zip = im::ImportFiles("freeshow_project",
                                       { File("bundle", "project",
                                              std::string(zipBytes.value().begin(), zipBytes.value().end())) });
            CHECK(zip.ok() && zip.value().shows.size() == 2);
            if (zip.ok() && zip.value().shows.size() == 2) {
                CHECK(zip.value().shows[0].name == "Project Song A" && zip.value().shows[1].name == "Project Song B");
                CHECK(zip.value().shows[0].sections[0].slides[0].text == "Alpha");
            }
        }
        // A broken member inside an otherwise good archive fails alone.
        content::ZipWriter zw2;
        CHECK(zw2.AddEntry("good.show", std::vector<uint8_t>(showA.begin(), showA.end())).ok());
        CHECK(zw2.AddEntry("bad.show", std::vector<uint8_t>{ '{', 'n', 'o' }).ok());
        auto zip2 = zw2.Finalize();
        CHECK(zip2.ok());
        if (zip2.ok()) {
            auto mixed = im::ImportFiles("freeshow_project",
                                         { File("mixed", "project",
                                                std::string(zip2.value().begin(), zip2.value().end())) });
            CHECK(mixed.ok() && mixed.value().shows.size() == 1 && mixed.value().warnings.size() == 1);
        }
        // The JSON-array shape (not a zip): ["id1", show], ["id2", show]
        const std::string jsonArray =
            std::string("[") + "\"id1\"," + showA + "," + "\"id2\"," + showB + "]";
        auto list = im::ImportFiles("freeshow_project", { File("list", "shows", jsonArray) });
        CHECK(list.ok() && list.value().shows.size() == 2 && list.value().shows[1].name == "Project Song B");
    }

    // ---- ProPresenter BUNDLE: a zip of OpenSong-shaped show XMLs ----
    {
        const std::string proShow =
            R"(<song><title>Bundle Song</title><verse name="v1"><lines>[C]Bundle line one</lines></verse></song>)";
        content::ZipWriter zw;
        CHECK(zw.AddEntry("shows/song.pro6", std::vector<uint8_t>(proShow.begin(), proShow.end())).ok());
        auto zipBytes = zw.Finalize();
        CHECK(zipBytes.ok());
        if (zipBytes.ok()) {
            auto bundle = im::ImportFiles("propresenter",
                                          { File("pack", "probundle",
                                                 std::string(zipBytes.value().begin(), zipBytes.value().end())) });
            CHECK(bundle.ok() && bundle.value().shows.size() == 1);
            if (bundle.ok() && !bundle.value().shows.empty()) {
                CHECK(bundle.value().shows[0].name == "Bundle Song");
                CHECK(bundle.value().shows[0].sections[0].slides[0].text == "Bundle line one");
            }
        }
    }

    // ---- Bibles go to the BibleEngine ----
    auto& engine = bb::BibleEngine::Instance();
    CHECK(engine.Initialize().ok() && engine.Start().ok());
    const char* zefania = "<bible abbrev=\"IMP\" name=\"Import Test\"><book bnum=\"43\" bsname=\"JHN\" bname=\"John\"><chapter number=\"3\">"
                          "<verse number=\"16\">For God so loved the world</verse></chapter></book></bible>";
    auto bible = im::ImportFiles("bible_xml", { File("imp", "xml", zefania), File("junk", "xml", "not a bible") });
    CHECK(bible.ok() && bible.value().bibles.size() == 1 && bible.value().warnings.size() == 1 && bible.value().shows.empty());
    if (bible.ok() && bible.value().bibles.size() == 1) {
        CHECK(engine.GetVerse(bible.value().bibles[0], "JHN", 3, 16).ok());
        CHECK(engine.RemoveBible(bible.value().bibles[0]).ok());
    }
}

void TestShowFromImported() {
    im::ImportedShow show;
    show.name = "Song";
    show.category = "song";
    show.meta = { { "title", "Song" }, { "author", "Me" } };
    show.notes = "play it slowly";
    show.origin = "txt";
    auto section = [](const char* group, const char* label, const char* text) {
        im::ImportedSection s; s.group = group; s.label = label; s.slides.push_back({ text, "n", "" }); return s;
    };
    show.sections = { section("verse", "Verse", "first"), section("chorus", "Chorus", "sing"), section("verse", "Verse", "second"), section("chorus", "Chorus", "sing") };
    show.sections[2].slides.push_back({ "second, part two", {}, "[[{\"pos\":0,\"key\":\"C\"}]]" });

    // ---- with no template: a plain centred text box ----
    auto p = im::ShowFromImported(show);
    CHECK(p.name == "Song" && p.slides.size() == 5 && p.categories.size() == 1 && p.categories[0].contentType == "song");
    CHECK(p.metaJson.find("\"author\":\"Me\"") != std::string::npos && p.metaJson.find("play it slowly") != std::string::npos && p.metaJson.find("\"origin\":\"txt\"") != std::string::npos);
    if (p.slides.size() == 5) {
        CHECK(p.slides[0].title == "Verse 1" && p.slides[1].title == "Chorus" && p.slides[2].title == "Verse 2" && p.slides[3].title == "Verse 2 (2)" && p.slides[4].title == "Chorus");
        CHECK(p.slides[0].text == "first" && p.slides[0].notes == "n" && p.slides[0].tags == std::vector<std::string>({ "verse" }) && p.slides[0].categoryId == p.categories[0].id);
        CHECK(p.slides[0].blocks.size() == 1 && p.slides[0].blocks[0].text == "first" && p.slides[0].blocks[0].bind == "text");
        CHECK(p.slides[3].metaJson.find("\"chords\"") != std::string::npos && p.slides[3].metaJson.find("\"group\":\"verse\"") != std::string::npos);
        std::set<std::string> ids;
        for (const auto& s : p.slides) CHECK(ids.insert(s.id).second);   // every slide has its own id
    }

    // ---- with a template: its bound blocks show the words, the label, the notes; the rest is copied ----
    im::ShowBuildOptions options;
    options.background = "#101010";
    auto box = [](const char* bind, const char* text) { bps::presentation::ContentBlock b; b.kind = "text"; b.bind = bind; b.text = text; return b; };
    options.templateBlocks = { box("text", "placeholder"), box("title", ""), box("notes", ""), box("", "static") };
    auto t = im::ShowFromImported(show, options);
    if (!t.slides.empty()) {
        const auto& b = t.slides[0].blocks;
        CHECK(b.size() == 4 && b[0].text == "first" && b[1].text == "Verse 1" && b[2].text == "n" && b[3].text == "static");
        CHECK(b[0].id != b[1].id && t.slides[0].background == "#101010");
    }

    // ---- the clipboard converter ----
    auto pasted = im::ShowFromClipboardText("[Verse]\nSome words\nMore words");
    CHECK(pasted.origin == "clipboard" && pasted.sections.size() == 1);
}
