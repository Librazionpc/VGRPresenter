// Unit tests: the overlay library (the dock's Overlays tab).
//   ./bps_unit_tests overlays
#include "TestHarness.hpp"

#include "modules/overlays/OverlayLibrary.hpp"

namespace {

namespace ov = bps::overlays;

std::vector<std::string> Names(const std::vector<ov::Overlay>& list) {
    std::vector<std::string> out;
    for (const auto& o : list) out.push_back(o.name);
    return out;
}

} // namespace

void TestOverlayLibrary() {
    auto& fs = bps::platform::PlatformAccessor::Get().Filesystem();
    const std::string root = "/tmp/bps_overlay_library";
    const std::string file = root + "/overlays.json";
    (void)fs.RemoveAll(root);

    // ---- a fresh library holds only what the app ships with: the Visuals category and its overlays ----
    {
        ov::OverlayLibrary lib(file);
        CHECK(lib.Load().ok());
        const auto cats = lib.Categories();
        CHECK(cats.size() == 1 && cats[0].id == "visuals" && cats[0].name == "Visuals" && cats[0].isDefault);
        CHECK(cats[0].icon == "star");
        const auto all = lib.Overlays();
        CHECK(all.size() == 6);
        for (const auto& o : all) CHECK(o.isDefault && o.category == "visuals" && !o.elements.empty());
        const auto counts = lib.Counts();
        CHECK(counts.all == 6 && counts.unlabeled == 0 && counts.byCategory.at("visuals") == 6);
        // Name order, and the defaults carry FreeShow's flags.
        CHECK(Names(all) == (std::vector<std::string>{ "Clock", "Clock (Analog)", "Name", "Recording", "Rounded", "Vignette" }));
        const auto name = lib.Get("name");
        CHECK(name.ok() && name.value().displayDuration == 4 && name.value().elements.size() == 4);
        CHECK(lib.Get("rounded").value().locked && lib.Get("vignette").value().locked && !lib.Get("clock").value().locked);
        CHECK(lib.Get("clock_analog").value().elements[0].analog);
        CHECK(!lib.Get("nope").ok());
        // Loading does not write a file by itself.
        CHECK(!fs.Exists(file));
    }

    // ---- categories ----
    ov::OverlayLibrary lib(file);
    CHECK(lib.Load().ok());
    CHECK(!lib.CreateCategory("   ").ok());
    auto notice = lib.CreateCategory("  Notice ", "info");
    CHECK(notice.ok() && notice.value().name == "Notice" && notice.value().icon == "info" && !notice.value().isDefault);
    CHECK(lib.CreateCategory("notice").error().code == bps::Err::AlreadyExists);
    CHECK(lib.CreateCategory("VISUALS").error().code == bps::Err::AlreadyExists);   // the default's name is taken too
    auto offers = lib.CreateCategory("Offers");
    CHECK(offers.ok() && offers.value().icon == "folder" && offers.value().id != notice.value().id);
    CHECK(lib.Categories().size() == 3 && lib.Categories()[0].id == "visuals");
    CHECK(lib.RenameCategory(offers.value().id, "Promos").ok());
    CHECK(lib.RenameCategory(offers.value().id, "notice").error().code == bps::Err::AlreadyExists);
    CHECK(lib.RenameCategory("visuals", "X").error().code == bps::Err::InvalidState);
    CHECK(lib.RenameCategory("cat-999", "X").error().code == bps::Err::NotFound);
    CHECK(lib.SetCategoryIcon(offers.value().id, "cash").ok());
    CHECK(lib.SetCategoryIcon("visuals", "cash").error().code == bps::Err::InvalidState);
    CHECK(lib.DeleteCategory("visuals").error().code == bps::Err::InvalidState);

    // ---- overlays ----
    auto first = lib.CreateOverlay({}, notice.value().id);
    CHECK(first.ok() && first.value().name == "Overlay" && first.value().category == notice.value().id);
    CHECK(!first.value().isDefault && first.value().elements.size() == 2);
    auto second = lib.CreateOverlay();
    CHECK(second.ok() && second.value().name == "Overlay 2" && second.value().category.empty());
    CHECK(lib.CreateOverlay({}, {}).value().name == "Overlay 3");
    CHECK(lib.CreateOverlay("x", "cat-999").error().code == bps::Err::InvalidArgument);
    auto lowerThird = lib.CreateOverlay("Guest lower third", offers.value().id);
    CHECK(lowerThird.ok());
    const std::string thirdId = lowerThird.value().id;

    {
        const auto counts = lib.Counts();
        CHECK(counts.all == 10 && counts.unlabeled == 2);
        CHECK(counts.byCategory.at("visuals") == 6 && counts.byCategory.at(notice.value().id) == 1
              && counts.byCategory.at(offers.value().id) == 1);
    }

    // Filters: a category, the unlabeled ones, an unknown category.
    CHECK(lib.Overlays("visuals").size() == 6);
    CHECK(Names(lib.Overlays(notice.value().id)) == std::vector<std::string>{ "Overlay" });
    CHECK(Names(lib.Overlays(std::string(ov::kUnlabeled))) == (std::vector<std::string>{ "Overlay 2", "Overlay 3" }));
    CHECK(lib.Overlays("cat-999").empty());
    // Natural name order: "Overlay 2" before "Overlay 10".
    for (int i = 0; i < 8; ++i) CHECK(lib.CreateOverlay().ok());
    {
        const auto unlabeled = Names(lib.Overlays(std::string(ov::kUnlabeled)));
        CHECK(unlabeled.size() == 10 && unlabeled[0] == "Overlay 2" && unlabeled[8] == "Overlay 10" && unlabeled[9] == "Overlay 11");
    }
    // Search: every word, any order, prefix first, inside a filter too.
    CHECK(Names(lib.Overlays({}, "clock")) == (std::vector<std::string>{ "Clock", "Clock (Analog)" }));
    CHECK(Names(lib.Overlays({}, "third guest")) == std::vector<std::string>{ "Guest lower third" });
    CHECK(Names(lib.Overlays({}, "ANALOG")) == std::vector<std::string>{ "Clock (Analog)" });
    CHECK(Names(lib.Overlays({}, "o")).front().rfind("O", 0) == 0);   // names starting with the word come first
    CHECK(lib.Overlays({}, "zzz").empty());
    CHECK(Names(lib.Overlays("visuals", "clock")).size() == 2 && lib.Overlays(offers.value().id, "clock").empty());
    CHECK(lib.Overlays({}, "   ").size() == lib.Overlays().size());

    // Edits.
    CHECK(lib.Rename(thirdId, "  Speaker  ").ok() && lib.Get(thirdId).value().name == "Speaker");
    CHECK(lib.Rename(thirdId, " ").error().code == bps::Err::InvalidArgument);
    CHECK(lib.Rename("ov-999", "x").error().code == bps::Err::NotFound);
    CHECK(lib.SetColor(thirdId, "#ff0000").ok() && lib.Get(thirdId).value().color == "#ff0000");
    CHECK(lib.SetCategory(thirdId, "visuals").ok() && lib.Get(thirdId).value().category == "visuals");
    CHECK(lib.SetCategory(thirdId, "cat-999").error().code == bps::Err::InvalidArgument);
    CHECK(lib.Get(thirdId).value().category == "visuals");   // a refused edit changes nothing
    CHECK(lib.SetCategory(thirdId, "").ok() && lib.Get(thirdId).value().category.empty());
    CHECK(lib.SetLocked(thirdId, true).ok() && lib.Get(thirdId).value().locked);
    CHECK(lib.SetDisplayDuration(thirdId, 6.5).ok() && lib.Get(thirdId).value().displayDuration == 6.5);
    CHECK(lib.SetDisplayDuration(thirdId, -1).error().code == bps::Err::InvalidArgument);
    {
        ov::OverlayElement e;
        e.kind = ov::OverlayElement::Kind::Text;
        e.x = 10; e.y = 20; e.width = 300; e.height = 90; e.text = "Hello \"quoted\"\nline"; e.bold = true; e.align = "center";
        CHECK(lib.SetElements(thirdId, { e }).ok());
    }

    // Duplicate: a plain copy, never default or locked.
    auto copy = lib.Duplicate("name");
    CHECK(copy.ok() && copy.value().name == "Name copy" && !copy.value().isDefault && !copy.value().locked
          && copy.value().id != "name" && copy.value().elements.size() == 4 && copy.value().category == "visuals"
          && copy.value().displayDuration == 4);
    auto lockedCopy = lib.Duplicate(thirdId);
    CHECK(lockedCopy.ok() && !lockedCopy.value().locked);
    CHECK(lib.Duplicate("ov-999").error().code == bps::Err::NotFound);

    // Deleting a category files its overlays under "unlabeled"; the overlays stay.
    CHECK(lib.DeleteCategory(notice.value().id).ok());
    CHECK(lib.Get(first.value().id).value().category.empty());
    CHECK(lib.Categories().size() == 2 && lib.DeleteCategory(notice.value().id).error().code == bps::Err::NotFound);

    // Deleting overlays: a user's is just gone, a default stays gone.
    CHECK(lib.Delete(first.value().id).ok() && !lib.Get(first.value().id).ok());
    CHECK(lib.Delete(first.value().id).error().code == bps::Err::NotFound);
    CHECK(lib.Delete("vignette").ok() && lib.Overlays("visuals").size() == 6);   // 5 defaults + the "Name copy"

    // ---- it is all remembered ----
    {
        ov::OverlayLibrary again(file);
        CHECK(again.Load().ok());
        CHECK(again.Categories().size() == 2 && again.Categories()[1].name == "Promos" && again.Categories()[1].icon == "cash");
        CHECK(!again.Get("vignette").ok());                   // the deleted default did not come back
        CHECK(!again.Get(first.value().id).ok());
        const auto speaker = again.Get(thirdId);
        CHECK(speaker.ok() && speaker.value().name == "Speaker" && speaker.value().color == "#ff0000" && speaker.value().category.empty()
              && speaker.value().locked && speaker.value().displayDuration == 6.5 && speaker.value().elements.size() == 1);
        const auto& e = speaker.value().elements[0];
        CHECK(e.kind == ov::OverlayElement::Kind::Text && e.x == 10 && e.width == 300 && e.text == "Hello \"quoted\"\nline"
              && e.bold && e.align == "center");
        CHECK(again.Get("clock_analog").value().elements[0].analog && again.Get("name").value().elements.size() == 4);
        CHECK(again.Counts().all == lib.Counts().all);
        // Ids are not reused for live overlays.
        auto fresh = again.CreateOverlay("Fresh");
        CHECK(fresh.ok());
        for (const auto& o : again.Overlays()) CHECK(o.id != fresh.value().id || o.name == "Fresh");
        CHECK(again.Delete(fresh.value().id).ok());

        // Restoring the defaults brings back only what was deleted.
        CHECK(again.RestoreDefaults().value() == 1 && again.Get("vignette").ok());
        CHECK(again.RestoreDefaults().value() == 0);
        ov::OverlayLibrary restored(file);
        CHECK(restored.Load().ok() && restored.Get("vignette").ok());
    }

    // An edited default is kept as edited, and deleting every default leaves none behind after a restart.
    {
        ov::OverlayLibrary edit(file);
        CHECK(edit.Load().ok());
        CHECK(edit.Rename("clock", "Big clock").ok());
        for (const char* id : { "recording", "clock_analog", "name", "rounded", "vignette" }) CHECK(edit.Delete(id).ok());
        ov::OverlayLibrary again(file);
        CHECK(again.Load().ok());
        CHECK(again.Get("clock").value().name == "Big clock");
        CHECK(!again.Get("recording").ok() && !again.Get("name").ok() && !again.Get("rounded").ok());
        CHECK(again.RestoreDefaults().value() == 5 && again.Get("clock").value().name == "Big clock");
    }

    // ---- a damaged file is reported and the defaults are still there ----
    {
        CHECK(fs.Write(file, "{ this is not json").ok());
        ov::OverlayLibrary broken(file);
        const auto loaded = broken.Load();
        CHECK(!loaded.ok() && loaded.error().code == bps::Err::ParseError);
        CHECK(broken.Overlays().size() == 6 && broken.Categories().size() == 1);
    }

    // ---- an overlay filed under a category that is gone is unlabeled, and the app's category flag wins ----
    {
        CHECK(fs.Write(file,
                       R"({"version":1,"categories":[{"id":"visuals","name":"Renamed","icon":"x","default":false}],)"
                       R"("overlays":[{"id":"ov-5","name":"Orphan","category":"cat-77","items":[{"kind":99,"x":1}]}],)"
                       R"("deletedDefaults":["clock","recording","clock_analog","name","rounded","vignette"]})")
                  .ok());
        ov::OverlayLibrary odd(file);
        CHECK(odd.Load().ok());
        CHECK(odd.Categories().size() == 1 && odd.Categories()[0].isDefault);
        const auto orphan = odd.Get("ov-5");
        CHECK(orphan.ok() && orphan.value().category.empty() && orphan.value().elements.size() == 1
              && orphan.value().elements[0].kind == ov::OverlayElement::Kind::Box);   // an unknown kind falls back to a box
        CHECK(odd.Overlays().size() == 1);
        CHECK(odd.CreateOverlay().value().id == "ov-6");
    }

    (void)fs.RemoveAll(root);
}
