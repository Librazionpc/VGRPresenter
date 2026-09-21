// Unit tests: the design libraries behind the dock's Overlays and Templates tabs.
//   ./bps_unit_tests library
#include "TestHarness.hpp"

#include <cmath>
#include <limits>

#include "modules/library/DesignCatalogs.hpp"
#include "modules/presentation/BlockValidator.hpp"
#include "modules/presentation/ShowEditor.hpp"
#include "modules/presentation/TextFormat.hpp"

namespace {

namespace lib = bps::library;
namespace pres = bps::presentation;

std::vector<std::string> Names(const std::vector<lib::Design>& list) {
    std::vector<std::string> out;
    for (const auto& d : list) out.push_back(d.name);
    return out;
}

std::vector<std::string> Ids(const std::vector<pres::ContentBlock>& blocks) {
    std::vector<std::string> out;
    for (const auto& b : blocks) out.push_back(b.id);
    return out;
}

pres::ContentBlock TextBlock(std::string text, double x = 10, double y = 20, std::string id = {}) {
    pres::ContentBlock b;
    b.id = std::move(id);   // empty for a block being ADDED (the engine issues its id); a canvas being SET carries ids
    b.kind = "text";
    b.text = std::move(text);
    b.x = x; b.y = y; b.width = 300; b.height = 90;
    b.bind = "title";
    b.metaJson = R"({"bold":true,"align":"center","fontSize":24})";
    b.style.backgroundColor = "#ff0b57a2";
    b.style.cornerRadius = 6;
    b.style.borderEnabled = true;
    b.style.borderColor = "#ffffff";
    return b;
}

} // namespace

// The overlay library: what ships, categories, filing, search, edits, persistence.
void TestOverlayLibrary() {
    auto& fs = bps::platform::PlatformAccessor::Get().Filesystem();
    const std::string root = "/tmp/bps_overlay_library";
    const std::string file = root + "/overlays.json";
    (void)fs.RemoveAll(root);

    // ---- a fresh library holds only what the app ships: the Visuals category and its overlays ----
    {
        auto library = lib::MakeOverlayLibrary(file);
        CHECK(library->Load().ok());
        const auto cats = library->Categories();
        CHECK(cats.size() == 1 && cats[0].id == "visuals" && cats[0].name == "Visuals" && cats[0].isDefault);
        CHECK(cats[0].icon == "star");
        const auto all = library->Designs();
        CHECK(all.size() == 6);
        for (const auto& d : all) CHECK(d.isDefault && d.category == "visuals" && !d.blocks.empty());
        const auto counts = library->Counts();
        CHECK(counts.all == 6 && counts.unlabeled == 0 && counts.byCategory.at("visuals") == 6);
        // Name order, and the shipped overlays carry FreeShow's flags.
        CHECK(Names(all) == (std::vector<std::string>{ "Clock", "Clock (Analog)", "Name", "Recording", "Rounded", "Vignette" }));
        const auto name = library->Get("name");
        CHECK(name.ok() && name.value().displayDuration == 4 && name.value().blocks.size() == 4);
        CHECK(library->Get("rounded").value().locked && library->Get("vignette").value().locked && !library->Get("clock").value().locked);
        CHECK(!library->Get("nope").ok());
        // They are made of the Edit screen's own blocks, on its stage, with engine-issued ids.
        const auto analog = library->Get("clock_analog").value();
        CHECK(analog.blocks.size() == 1 && analog.blocks[0].kind == "clock" && analog.blocks[0].id == "item-1");
        CHECK(analog.blocks[0].metaJson.find("analog") != std::string::npos);
        CHECK(analog.blocks[0].width == analog.blocks[0].height);
        for (const auto& d : all)
            for (const auto& b : d.blocks) {
                CHECK(b.x >= 0 && b.y >= 0 && b.x + b.width <= lib::kStageWidth + 0.01 && b.y + b.height <= lib::kStageHeight + 0.01);
            }
        CHECK(library->Get("rounded").value().blocks[0].kind == "corners" && library->Get("vignette").value().blocks[0].kind == "vignette");
        // The title is typed as "Title" and shown in capitals through the text Case option, not typed in capitals.
        CHECK(library->Get("name").value().blocks[3].text == "Title" && library->Get("name").value().blocks[3].style.backgroundColor == "#006fcf"
              && library->Get("name").value().blocks[3].metaJson.find("\"textCase\":\"upper\"") != std::string::npos);
        // Transparent is really transparent (the recording frame is an outline, with no stand-in fill), and the clocks carry their colour.
        CHECK(library->Get("recording").value().blocks[0].style.backgroundColor == "transparent" && library->Get("recording").value().blocks[0].style.borderEnabled);
        CHECK(library->Get("clock").value().blocks[0].metaJson.find("\"color\":\"#ffffff\"") != std::string::npos);
        // The library itself says which extra block kinds its designs may hold (the Edit screen's Add menu follows it).
        CHECK(library->Config().extraBlockKinds == (std::vector<std::string>{ "vignette", "corners" }));
        // Loading does not write a file by itself.
        CHECK(!fs.Exists(file));
    }

    // ---- categories ----
    auto library = lib::MakeOverlayLibrary(file);
    CHECK(library->Load().ok());
    CHECK(!library->CreateCategory("   ").ok());
    auto notice = library->CreateCategory("  Notice ", "info");
    CHECK(notice.ok() && notice.value().name == "Notice" && notice.value().icon == "info" && !notice.value().isDefault);
    CHECK(library->CreateCategory("notice").error().code == bps::Err::AlreadyExists);
    CHECK(library->CreateCategory("VISUALS").error().code == bps::Err::AlreadyExists);   // the shipped name is taken too
    auto offers = library->CreateCategory("Offers");
    CHECK(offers.ok() && offers.value().icon == "folder" && offers.value().id != notice.value().id);
    CHECK(library->Categories().size() == 3 && library->Categories()[0].id == "visuals");
    CHECK(library->RenameCategory(offers.value().id, "Promos").ok());
    CHECK(library->RenameCategory(offers.value().id, "notice").error().code == bps::Err::AlreadyExists);
    CHECK(library->RenameCategory("visuals", "X").error().code == bps::Err::InvalidState);
    CHECK(library->RenameCategory("cat-999", "X").error().code == bps::Err::NotFound);
    CHECK(library->SetCategoryIcon(offers.value().id, "cash").ok());
    CHECK(library->SetCategoryIcon("visuals", "cash").error().code == bps::Err::InvalidState);
    CHECK(library->DeleteCategory("visuals").error().code == bps::Err::InvalidState);

    // ---- designs: a new one holds the starter blocks, with engine-issued ids ----
    auto first = library->Create({}, notice.value().id);
    CHECK(first.ok() && first.value().name == "Overlay" && first.value().category == notice.value().id);
    CHECK(!first.value().isDefault && Ids(first.value().blocks) == (std::vector<std::string>{ "item-1", "item-2" }));
    CHECK(first.value().blocks[1].kind == "text" && first.value().background == "transparent");
    auto second = library->Create();
    CHECK(second.ok() && second.value().name == "Overlay 2" && second.value().category.empty());
    CHECK(library->Create({}, {}).value().name == "Overlay 3");
    CHECK(library->Create("x", "cat-999").error().code == bps::Err::InvalidArgument);
    auto lowerThird = library->Create("Guest lower third", offers.value().id);
    CHECK(lowerThird.ok());
    const std::string thirdId = lowerThird.value().id;

    {
        const auto counts = library->Counts();
        CHECK(counts.all == 10 && counts.unlabeled == 2);
        CHECK(counts.byCategory.at("visuals") == 6 && counts.byCategory.at(notice.value().id) == 1
              && counts.byCategory.at(offers.value().id) == 1);
    }

    // Filters: a category, the unlabeled ones, an unknown category.
    CHECK(library->Designs("visuals").size() == 6);
    CHECK(Names(library->Designs(notice.value().id)) == std::vector<std::string>{ "Overlay" });
    CHECK(Names(library->Designs(std::string(lib::kUnlabeled))) == (std::vector<std::string>{ "Overlay 2", "Overlay 3" }));
    CHECK(library->Designs("cat-999").empty());
    // Natural name order: "Overlay 2" before "Overlay 10".
    for (int i = 0; i < 8; ++i) CHECK(library->Create().ok());
    {
        const auto unlabeled = Names(library->Designs(std::string(lib::kUnlabeled)));
        CHECK(unlabeled.size() == 10 && unlabeled[0] == "Overlay 2" && unlabeled[8] == "Overlay 10" && unlabeled[9] == "Overlay 11");
    }
    // Search: every word, any order, prefix first, inside a filter too.
    CHECK(Names(library->Designs({}, "clock")) == (std::vector<std::string>{ "Clock", "Clock (Analog)" }));
    CHECK(Names(library->Designs({}, "third guest")) == std::vector<std::string>{ "Guest lower third" });
    CHECK(Names(library->Designs({}, "ANALOG")) == std::vector<std::string>{ "Clock (Analog)" });
    CHECK(Names(library->Designs({}, "o")).front().rfind("O", 0) == 0);   // names starting with the word come first
    CHECK(library->Designs({}, "zzz").empty());
    CHECK(Names(library->Designs("visuals", "clock")).size() == 2 && library->Designs(offers.value().id, "clock").empty());
    CHECK(library->Designs({}, "   ").size() == library->Designs().size());

    // Edits.
    CHECK(library->Rename(thirdId, "  Speaker  ").ok() && library->Get(thirdId).value().name == "Speaker");
    CHECK(library->Rename(thirdId, " ").error().code == bps::Err::InvalidArgument);
    CHECK(library->Rename("ov-999", "x").error().code == bps::Err::NotFound);
    CHECK(library->SetColor(thirdId, "#ff0000").ok() && library->Get(thirdId).value().color == "#ff0000");
    CHECK(library->SetCategory(thirdId, "visuals").ok() && library->Get(thirdId).value().category == "visuals");
    CHECK(library->SetCategory(thirdId, "cat-999").error().code == bps::Err::InvalidArgument);
    CHECK(library->Get(thirdId).value().category == "visuals");   // a refused edit changes nothing
    CHECK(library->SetCategory(thirdId, "").ok() && library->Get(thirdId).value().category.empty());
    CHECK(library->SetLocked(thirdId, true).ok() && library->Get(thirdId).value().locked);
    CHECK(library->SetPlaceUnderSlide(thirdId, true).ok() && library->Get(thirdId).value().placeUnderSlide);
    CHECK(library->SetDisplayDuration(thirdId, 6.5).ok() && library->Get(thirdId).value().displayDuration == 6.5);
    CHECK(library->SetDisplayDuration(thirdId, -1).error().code == bps::Err::InvalidArgument);
    CHECK(library->SetContentType(thirdId, "song").ok() && library->Get(thirdId).value().contentType == "song");

    // ---- content: the canvas the Edit screen edits ----
    CHECK(library->SetContent(thirdId, "#ff101010", { TextBlock("Hello \"quoted\"\nline", 10, 20, "c1") }).ok());
    {
        const auto d = library->Get(thirdId).value();
        CHECK(d.background == "#ff101010" && d.blocks.size() == 1 && d.blocks[0].text == "Hello \"quoted\"\nline");
    }
    CHECK(library->SetContent(thirdId, "", {}).ok() && library->Get(thirdId).value().background == "transparent");
    CHECK(library->SetContent("ov-999", "", {}).error().code == bps::Err::NotFound);

    // Block edits follow the show editor's rules: the engine claims the id.
    auto a = library->AddBlock(thirdId, TextBlock("A"));
    auto b = library->AddBlock(thirdId, TextBlock("B"));
    CHECK(a.ok() && b.ok() && a.value() != b.value() && Ids(library->Get(thirdId).value().blocks) == (std::vector<std::string>{ a.value(), b.value() }));
    auto between = library->AddBlock(thirdId, TextBlock("between"), a.value());   // directly above A
    CHECK(between.ok() && Ids(library->Get(thirdId).value().blocks) == (std::vector<std::string>{ a.value(), between.value(), b.value() }));
    CHECK(library->AddBlock(thirdId, TextBlock("x"), "item-999").error().code == bps::Err::NotFound);
    auto copy = library->DuplicateBlock(thirdId, a.value());
    CHECK(copy.ok() && library->Block(thirdId, copy.value()).value().x == 30 && library->Block(thirdId, copy.value()).value().y == 40
          && library->Block(thirdId, copy.value()).value().text == "A");
    CHECK(library->Block(thirdId, a.value()).value().x == 10);   // the original did not move
    CHECK(library->RemoveBlock(thirdId, between.value()).ok() && !library->Block(thirdId, between.value()).ok());
    CHECK(library->RemoveBlock(thirdId, between.value()).error().code == bps::Err::NotFound);
    CHECK(library->DuplicateBlock(thirdId, "item-999").error().code == bps::Err::NotFound);
    CHECK(library->AddBlock("ov-999", TextBlock("x")).error().code == bps::Err::NotFound);
    CHECK(library->Block("ov-999", "item-1").error().code == bps::Err::NotFound);

    // Duplicate: a plain copy, never shipped or locked.
    auto dup = library->Duplicate("name");
    CHECK(dup.ok() && dup.value().name == "Name copy" && !dup.value().isDefault && !dup.value().locked
          && dup.value().id != "name" && dup.value().blocks.size() == 4 && dup.value().category == "visuals"
          && dup.value().displayDuration == 4);
    auto lockedCopy = library->Duplicate(thirdId);
    CHECK(lockedCopy.ok() && !lockedCopy.value().locked);
    CHECK(library->Duplicate("ov-999").error().code == bps::Err::NotFound);

    // Deleting a category files its designs under "unlabeled"; the designs stay.
    CHECK(library->DeleteCategory(notice.value().id).ok());
    CHECK(library->Get(first.value().id).value().category.empty());
    CHECK(library->Categories().size() == 2 && library->DeleteCategory(notice.value().id).error().code == bps::Err::NotFound);

    // Deleting: a user's is just gone, a shipped one stays gone.
    CHECK(library->Delete(first.value().id).ok() && !library->Get(first.value().id).ok());
    CHECK(library->Delete(first.value().id).error().code == bps::Err::NotFound);
    CHECK(library->Delete("vignette").ok() && library->Designs("visuals").size() == 6);   // 5 shipped + "Name copy" (the Guest one is unfiled)

    // ---- it is all remembered, blocks included ----
    {
        auto again = lib::MakeOverlayLibrary(file);
        CHECK(again->Load().ok());
        CHECK(again->Categories().size() == 2 && again->Categories()[1].name == "Promos" && again->Categories()[1].icon == "cash");
        CHECK(!again->Get("vignette").ok());                   // the deleted shipped overlay did not come back
        CHECK(!again->Get(first.value().id).ok());
        const auto speaker = again->Get(thirdId);
        CHECK(speaker.ok() && speaker.value().name == "Speaker" && speaker.value().color == "#ff0000" && speaker.value().category.empty()
              && speaker.value().locked && speaker.value().placeUnderSlide && speaker.value().displayDuration == 6.5
              && speaker.value().contentType == "song");
        CHECK(speaker.value().blocks.size() == 3 && speaker.value().background == "transparent");
        const auto saved = again->Block(thirdId, a.value());
        CHECK(saved.ok() && saved.value().kind == "text" && saved.value().text == "A" && saved.value().x == 10 && saved.value().width == 300
              && saved.value().bind == "title" && saved.value().metaJson.find("\"bold\":true") != std::string::npos
              && saved.value().style.backgroundColor == "#ff0b57a2" && saved.value().style.cornerRadius == 6
              && saved.value().style.borderEnabled && saved.value().style.borderColor == "#ffffff");
        CHECK(again->Get("clock_analog").value().blocks[0].metaJson.find("analog") != std::string::npos
              && again->Get("name").value().blocks.size() == 4);
        CHECK(again->Counts().all == library->Counts().all);
        // Ids are not reused for live designs.
        auto fresh = again->Create("Fresh");
        CHECK(fresh.ok());
        for (const auto& d : again->Designs()) CHECK(d.id != fresh.value().id || d.name == "Fresh");
        CHECK(again->Delete(fresh.value().id).ok());

        // Restoring what ships brings back only what was deleted.
        CHECK(again->RestoreDefaults().value() == 1 && again->Get("vignette").ok());
        CHECK(again->RestoreDefaults().value() == 0);
        auto restored = lib::MakeOverlayLibrary(file);
        CHECK(restored->Load().ok() && restored->Get("vignette").ok());
    }

    // A shipped overlay that was edited is kept as edited, and deleting them all leaves none after a restart.
    {
        auto edit = lib::MakeOverlayLibrary(file);
        CHECK(edit->Load().ok());
        CHECK(edit->Rename("clock", "Big clock").ok());
        for (const char* id : { "recording", "clock_analog", "name", "rounded", "vignette" }) CHECK(edit->Delete(id).ok());
        auto again = lib::MakeOverlayLibrary(file);
        CHECK(again->Load().ok());
        CHECK(again->Get("clock").value().name == "Big clock");
        CHECK(!again->Get("recording").ok() && !again->Get("name").ok() && !again->Get("rounded").ok());
        CHECK(again->RestoreDefaults().value() == 5 && again->Get("clock").value().name == "Big clock");
    }

    // ---- a damaged file is reported and what ships is still there ----
    {
        CHECK(fs.Write(file, "{ this is not json").ok());
        auto broken = lib::MakeOverlayLibrary(file);
        const auto loaded = broken->Load();
        CHECK(!loaded.ok() && loaded.error().code == bps::Err::ParseError);
        CHECK(broken->Designs().size() == 6 && broken->Categories().size() == 1);
    }

    // ---- a design filed under a category that is gone is unlabeled, and the app's category flag wins ----
    {
        CHECK(fs.Write(file,
                       R"({"version":1,"categories":[{"id":"visuals","name":"Renamed","icon":"x","default":false}],)"
                       R"("designs":[{"id":"ov-5","name":"Orphan","category":"cat-77"}],)"
                       R"("deletedDefaults":["clock","recording","clock_analog","name","rounded","vignette"]})")
                  .ok());
        auto odd = lib::MakeOverlayLibrary(file);
        CHECK(odd->Load().ok());
        CHECK(odd->Categories().size() == 1 && odd->Categories()[0].isDefault);
        const auto orphan = odd->Get("ov-5");
        CHECK(orphan.ok() && orphan.value().category.empty() && orphan.value().blocks.empty() && orphan.value().background == "transparent");
        CHECK(odd->Designs().size() == 1);
        CHECK(odd->Create().value().id == "ov-6");
    }

    (void)fs.RemoveAll(root);
}

// The template library: FreeShow's starter set ships in it (the song / presentation / scripture
// categories and their templates), the user's own work lives beside it, and a template is a real
// SlideTemplate.
void TestTemplateLibrary() {
    auto& fs = bps::platform::PlatformAccessor::Get().Filesystem();
    const std::string root = "/tmp/bps_template_library";
    const std::string file = root + "/templates.json";
    (void)fs.RemoveAll(root);

    auto library = lib::MakeTemplateLibrary(file);
    CHECK(library->Load().ok());
    // What ships: the three default categories and FreeShow's starter templates in them.
    const auto config = lib::TemplateLibraryConfig();
    CHECK(config.defaultCategories.size() == 3 && !config.defaultDesigns.empty());
    CHECK(library->Categories().size() == 3);
    const size_t shippedCount = config.defaultDesigns.size();
    CHECK(library->Designs().size() == shippedCount);
    {
        const auto counts = library->Counts();
        CHECK(counts.all == shippedCount && counts.unlabeled == 0 && counts.byCategory.size() == 3);
    }
    CHECK(library->RestoreDefaults().value() == 0);
    CHECK(library->Config().extraBlockKinds.empty());   // templates add no block kinds of their own
    // Every shipped template shows the slide's own content: at least one block is bound, to a field a slide has,
    // and it says which kind of show category it fits.
    for (const auto& t : config.defaultDesigns) {
        bool bound = false;
        for (const auto& b : t.blocks) {
            if (b.bind.empty()) continue;
            bound = true;
            CHECK(b.bind == "title" || b.bind == "text" || b.bind == "line1" || b.bind == "line2" || b.bind == "ref"
                  || b.bind == "notes");
        }
        CHECK(bound);
        CHECK(!t.contentType.empty() && t.isDefault && !t.category.empty());
    }

    // The user makes categories and templates beside what ships.
    auto songs = library->CreateCategory("Songs", "music");
    CHECK(songs.ok() && !songs.value().isDefault);
    CHECK(library->Categories().size() == 4);
    CHECK(library->RenameCategory(songs.value().id, "Worship songs").ok());   // a user's category renames
    CHECK(library->DeleteCategory(songs.value().id).ok() && library->Categories().size() == 3);
    songs = library->CreateCategory("Songs", "music");

    auto lyrics = library->Create({}, songs.value().id);
    CHECK(lyrics.ok() && lyrics.value().name == "Template" && lyrics.value().id.rfind("tpl-", 0) == 0 && !lyrics.value().isDefault);
    // A new template starts as a title and a body bound to a slide's fields.
    CHECK(lyrics.value().blocks.size() == 2 && lyrics.value().blocks[0].bind == "title" && lyrics.value().blocks[1].bind == "text");
    CHECK(lyrics.value().blocks[0].kind == "text" && lyrics.value().blocks[1].width > 0);
    CHECK(library->Create().value().name == "Template 2");
    CHECK(library->Counts().all == shippedCount + 2 && library->Counts().unlabeled == 1 && library->Counts().byCategory.at(songs.value().id) == 1);

    // Deleting the user's template is final; a shipped one comes back with RestoreDefaults().
    CHECK(library->Delete(lyrics.value().id).ok() && !library->Get(lyrics.value().id).ok());
    const std::string firstShipped = config.defaultDesigns[0].id;
    CHECK(library->Delete(firstShipped).ok());
    CHECK(library->Designs().size() == shippedCount);
    CHECK(library->RestoreDefaults().value() == 1);
    CHECK(library->Designs().size() == shippedCount + 1);

    // A library template is a slide template a show can embed, `bind` and all.
    auto notes = library->Create("Sermon notes", songs.value().id);
    CHECK(notes.ok() && library->SetContentType(notes.value().id, "notes").ok());
    CHECK(library->SetContent(notes.value().id, "#ff000000", { TextBlock("Body", 10, 20, "c1") }).ok());
    {
        const auto t = library->AsTemplate(notes.value().id);
        CHECK(t.ok() && t.value().id == notes.value().id && t.value().name == "Sermon notes" && t.value().contentType == "notes"
              && t.value().background == "#ff000000" && t.value().blocks.size() == 1 && t.value().blocks[0].bind == "title");
    }
    CHECK(library->AsTemplate("tpl-999").error().code == bps::Err::NotFound);

    // Remembered across a restart, in its own file.
    {
        auto again = lib::MakeTemplateLibrary(file);
        CHECK(again->Load().ok());
        CHECK(again->Categories().size() == 4 && again->Categories()[3].name == "Songs" && again->Categories()[3].icon == "music");
        const auto saved = again->Get(notes.value().id);
        CHECK(saved.ok() && saved.value().name == "Sermon notes" && saved.value().contentType == "notes"
              && saved.value().blocks.size() == 1 && saved.value().blocks[0].bind == "title");
        // What ships, restored + the user's two (Template 2 and Sermon notes; the deleted Template is gone).
        CHECK(again->Designs().size() == shippedCount + 2);
        // The shipped set is intact after the restart (the restore above brought the deleted one back).
        CHECK(again->Get(firstShipped).ok());
    }

    CHECK(lib::OverlayLibraryConfig().noun == "overlay" && lib::TemplateLibraryConfig().noun == "template");
    (void)fs.RemoveAll(root);
}

// Text lists: the engine turns typed lines into list items (the Text tab's "List" option).
void TestTextListFormat() {
    namespace pf = bps::presentation;

    // The styles: "none" first, keys unique, every real style has a sample marker.
    const auto& styles = pf::ListStyles();
    CHECK(!styles.empty() && styles[0].key == "none" && styles[0].sample.empty());
    for (size_t i = 0; i < styles.size(); ++i) {
        for (size_t j = i + 1; j < styles.size(); ++j) CHECK(styles[i].key != styles[j].key);
        if (i > 0) CHECK(!styles[i].sample.empty() && !styles[i].label.empty());
    }
    auto sampleOf = [&](const char* key) {
        for (const auto& s : styles) if (s.key == key) return s.sample;
        return std::string("?");
    };
    CHECK(sampleOf("decimal") == "1." && sampleOf("disc") == "•" && sampleOf("dash") == "–" && sampleOf("lower-alpha") == "a."
          && sampleOf("upper-roman") == "I." && sampleOf("decimal-leading-zero") == "01.");

    // Every non-blank line becomes an item; blank lines are left alone and do not count.
    CHECK(pf::ApplyList("one\ntwo\nthree", "decimal") == "1. one\n2. two\n3. three");
    CHECK(pf::ApplyList("one\n\ntwo", "decimal") == "1. one\n\n2. two");
    CHECK(pf::ApplyList("  \none", "disc") == "  \n• one");
    CHECK(pf::ApplyList("a\nb", "dash") == "– a\n– b");
    CHECK(pf::ApplyList("only", "square") == "▪ only");
    CHECK(pf::ApplyList("", "decimal").empty());
    CHECK(pf::ApplyList("a\n", "decimal") == "1. a\n");   // a trailing newline stays

    // No list: the text is returned as it is.
    CHECK(pf::ApplyList("a\nb", "") == "a\nb" && pf::ApplyList("a\nb", "none") == "a\nb" && pf::ApplyList("a\nb", "nonsense") == "a\nb");

    // Markers: letters run on past z, roman numerals, leading zero, greek, and a number past what a style can spell falls back to digits.
    CHECK(pf::ListMarker("lower-alpha", 1) == "a." && pf::ListMarker("lower-alpha", 26) == "z." && pf::ListMarker("lower-alpha", 27) == "aa."
          && pf::ListMarker("upper-alpha", 28) == "AB.");
    CHECK(pf::ListMarker("lower-roman", 4) == "iv." && pf::ListMarker("lower-roman", 9) == "ix." && pf::ListMarker("upper-roman", 14) == "XIV."
          && pf::ListMarker("lower-roman", 1994) == "mcmxciv." && pf::ListMarker("lower-roman", 4000) == "4000.");
    CHECK(pf::ListMarker("decimal-leading-zero", 7) == "07." && pf::ListMarker("decimal-leading-zero", 12) == "12.");
    CHECK(pf::ListMarker("lower-greek", 1) == "α." && pf::ListMarker("lower-greek", 24) == "ω." && pf::ListMarker("lower-greek", 25) == "25.");
    CHECK(pf::ListMarker("none", 1).empty() && pf::ListMarker("", 3).empty() && pf::ListMarker("bogus", 3).empty());
    // Twelve lines number up to 12.
    std::string twelve;
    for (int i = 0; i < 12; ++i) twelve += (i ? "\nx" : "x");
    CHECK(pf::ApplyList(twelve, "decimal").find("\n12. x") != std::string::npos);
}

// The engine's rules for content blocks: what BlockValidator accepts, and that every way blocks enter the engine uses it.
void TestBlockValidator() {
    namespace pf = bps::presentation;
    namespace lib = bps::library;
    auto good = [](std::string id = "item-1") {
        pf::ContentBlock b;
        b.id = std::move(id);
        b.kind = "text";
        b.text = "hi";
        b.x = 10; b.y = 20; b.width = 100; b.height = 40;
        b.style.backgroundColor = "#80000000";
        b.style.borderColor = "white";
        b.metaJson = R"({"fontSize":20})";
        b.bind = "line1";
        return b;
    };
    auto rejects = [&](auto change, const char* why) {
        pf::ContentBlock b = good();
        change(b);
        const auto r = pf::CheckBlock(b, /*requireId=*/true);
        CHECK(!r.ok() && r.error().code == bps::Err::InvalidArgument);
        if (!r.ok()) CHECK(r.error().message.find(why) != std::string::npos);
    };

    // A sound block passes, with or without an id (an added block has none yet).
    CHECK(pf::CheckBlock(good(), true).ok() && pf::ValidateBlock(good(), true).empty());
    { auto b = good(""); CHECK(pf::CheckBlock(b, false).ok() && !pf::CheckBlock(b, true).ok()); }
    // The colours the UI produces, all accepted.
    for (const char* c : { "transparent", "#fff", "#8fff", "#ff0b57a2", "#0b57a2", "red", "dodgerblue" }) CHECK(pf::IsColorText(c));
    for (const char* c : { "", "#", "#12", "#12345", "#gggggg", "rgb(1,2,3)", "12", "#ff0b57a2ff" }) CHECK(!pf::IsColorText(c));

    // Every rule, one at a time.
    rejects([](auto& b) { b.kind = ""; }, "kind");
    rejects([](auto& b) { b.kind = "bad kind!"; }, "kind");
    rejects([](auto& b) { b.id = std::string(81, 'x'); }, "id");
    rejects([](auto& b) { b.x = std::nan(""); }, "x and y");
    rejects([](auto& b) { b.y = 1e9; }, "x and y");
    rejects([](auto& b) { b.width = -1; }, "width");
    rejects([](auto& b) { b.height = std::numeric_limits<double>::infinity(); }, "width and height");
    rejects([](auto& b) { b.text = std::string(200001, 'a'); }, "text");
    rejects([](auto& b) { b.style.padding = -3; }, "padding");
    rejects([](auto& b) { b.style.cornerRadius = std::nan(""); }, "corner radius");
    rejects([](auto& b) { b.style.backgroundColor = "not a colour"; }, "background");
    rejects([](auto& b) { b.style.borderColor = "#zzz"; }, "border");
    rejects([](auto& b) { b.style.borderStyle = "wavy"; }, "border style");
    rejects([](auto& b) { b.metaJson = "{ nope"; }, "JSON object");
    rejects([](auto& b) { b.metaJson = "[1,2]"; }, "JSON object");
    rejects([](auto& b) { b.bind = "has space"; }, "field name");
    // Several problems: the first is named and the rest counted.
    { auto b = good(); b.kind = ""; b.width = -1;
      const auto r = pf::CheckBlock(b, true);
      CHECK(!r.ok() && r.error().message.find("block 'item-1'") != std::string::npos && r.error().message.find("and 1 more") != std::string::npos); }

    // A list: unique ids, at most kMaxBlocksPerList.
    CHECK(pf::CheckBlocks({ good("a"), good("b") }).ok() && pf::CheckBlocks({}).ok());
    { const auto r = pf::CheckBlocks({ good("a"), good("a") });
      CHECK(!r.ok() && r.error().message.find("same id") != std::string::npos); }
    { std::vector<pf::ContentBlock> many;
      for (size_t i = 0; i <= pf::kMaxBlocksPerList; ++i) many.push_back(good("b" + std::to_string(i)));
      CHECK(!pf::CheckBlocks(many).ok()); many.pop_back(); CHECK(pf::CheckBlocks(many).ok()); }

    // ---- a design library refuses bad content and keeps what it had ----
    auto& fs = bps::platform::PlatformAccessor::Get().Filesystem();
    const std::string root = "/tmp/bps_block_validation";
    (void)fs.RemoveAll(root);
    auto library = lib::MakeOverlayLibrary(root + "/overlays.json");
    CHECK(library->Load().ok());
    auto made = library->Create("Probe");
    CHECK(made.ok());
    const std::string id = made.value().id;
    CHECK(library->SetContent(id, "#ff101010", { good("a"), good("b") }).ok());
    const auto before = library->Get(id).value();
    CHECK(!library->SetContent(id, "", { good("a"), good("a") }).ok());                                   // duplicate ids
    { auto nan = good("c"); nan.width = std::nan(""); CHECK(!library->SetContent(id, "", { nan }).ok()); }   // not a number
    CHECK(!library->SetContent(id, "no such colour!", { good("a") }).ok());                              // bad background
    CHECK(!library->SetContent(id, "", { good("") }).ok());                                              // a settled canvas has ids
    { const auto after = library->Get(id).value(); CHECK(after.blocks.size() == 2 && after.background == before.background && after.blocks[0].id == "a"); }
    { auto bad = good(""); bad.style.backgroundColor = "??"; CHECK(!library->AddBlock(id, bad).ok()); }
    CHECK(library->Get(id).value().blocks.size() == 2);
    { auto fresh = good(""); const auto added = library->AddBlock(id, fresh); CHECK(added.ok() && library->Get(id).value().blocks.size() == 3); }

    // ---- ShowEditor: every way a block or a list of blocks enters a show is checked ----
    pf::Presentation show;
    pf::Slide slide;
    slide.title = "s";
    auto slideId = pf::ShowEditor::AddSlide(show, slide);
    CHECK(slideId.ok());
    CHECK(pf::ShowEditor::AddBlock(show, slideId.value(), good("")).ok());
    { auto bad = good(""); bad.width = -5; CHECK(!pf::ShowEditor::AddBlock(show, slideId.value(), bad).ok()); }
    CHECK(show.slides[0].blocks.size() == 1);
    { pf::Slide replaced; replaced.blocks = { good("x"), good("x") };
      CHECK(!pf::ShowEditor::UpdateSlide(show, slideId.value(), replaced).ok()); CHECK(show.slides[0].blocks.size() == 1); }
    { pf::Slide replaced; replaced.blocks = { good("x"), good("") };                                       // a block with no id is given one
      CHECK(pf::ShowEditor::UpdateSlide(show, slideId.value(), replaced).ok());
      CHECK(show.slides[0].blocks.size() == 2 && !show.slides[0].blocks[1].id.empty() && show.slides[0].blocks[1].id != "x"); }
    { pf::SlideTemplate tmpl; tmpl.name = "t"; auto bad = good("a"); bad.kind = ""; tmpl.blocks = { bad };
      CHECK(!pf::ShowEditor::AddTemplate(show, tmpl).ok()); tmpl.blocks = { good("a") }; CHECK(pf::ShowEditor::AddTemplate(show, tmpl).ok()); }
    { pf::Overlay overlay; overlay.name = "o"; auto bad = good("a"); bad.x = std::nan(""); overlay.blocks = { bad };
      CHECK(!pf::ShowEditor::AddOverlay(show, overlay).ok()); overlay.blocks = { good("a") }; CHECK(pf::ShowEditor::AddOverlay(show, overlay).ok()); }
    (void)fs.RemoveAll(root);
}
