// Unit tests: projects (FreeShow's Projects panel in the engine) - the folder tree, items and how they move, what may be dropped where.
//   ./bps_unit_tests library
#include "TestHarness.hpp"

#include "modules/library/ProjectLibrary.hpp"

namespace {

namespace lb = bps::library;

const std::string kProjectsRoot = "/tmp/bps_projects_test";

bps::platform::IFilesystem& ProjectFs() { return bps::platform::PlatformAccessor::Get().Filesystem(); }

std::vector<std::string> Names(const std::vector<lb::ProjectItem>& items) {
    std::vector<std::string> out;
    for (const auto& i : items) out.push_back(i.name);
    return out;
}

lb::ProjectItem Item(std::string type, std::string ref, std::string name = {}) {
    lb::ProjectItem i; i.type = std::move(type); i.ref = std::move(ref); i.name = std::move(name); return i;
}

} // namespace

void TestDropRules() {
    // a project takes shows and media, and scripture and overlays...
    for (const char* kind : { "show_drawer", "media", "audio", "audio_effect", "overlay", "player", "scripture", "effect", "screen", "ndi", "camera", "files" })
        CHECK(lb::AcceptsDrop("project", kind));
    // ...but a slide is not something a project holds, and a template goes only onto the slides
    CHECK(!lb::AcceptsDrop("project", "slide") && !lb::AcceptsDrop("project", "template") && !lb::AcceptsDrop("project", "folder"));
    CHECK(lb::AcceptsDrop("all_slides", "template") && !lb::AcceptsDrop("all_slides", "media"));
    CHECK(lb::AcceptsDrop("overlays", "slide") && lb::AcceptsDrop("templates", "slide") && !lb::AcceptsDrop("overlays", "show"));
    CHECK(lb::AcceptsDrop("slides", "scripture") && lb::AcceptsDrop("slides", "camera") && !lb::AcceptsDrop("slides", "folder"));
    CHECK(lb::AcceptsDrop("edit", "media") && !lb::AcceptsDrop("edit", "overlay"));
    // moving things that are already there: a show within its project, a folder in the tree
    CHECK(!lb::AcceptsDrop("project", "show") && lb::AcceptsDrop("project", "show", /*reorder=*/true));
    CHECK(lb::AcceptsDrop("projects", "folder", true) && lb::AcceptsDrop("projects", "project", true) && !lb::AcceptsDrop("projects", "folder"));
    CHECK(!lb::AcceptsDrop("nowhere", "media") && !lb::AcceptsDrop("nowhere", "media", true));

    // what a drop becomes
    CHECK(lb::ItemTypeForDrop("show_drawer") == "show" && lb::ItemTypeForDrop("audio_effect") == "audio" && lb::ItemTypeForDrop("overlay") == "overlay");
    CHECK(lb::ItemTypeForDrop("files", "Photo.JPG") == "image" && lb::ItemTypeForDrop("files", "clip.mp4") == "video" && lb::ItemTypeForDrop("files", "song.mp3") == "audio");
    CHECK(lb::ItemTypeForDrop("files", "slides.pdf") == "pdf" && lb::ItemTypeForDrop("files", "notes.docx").empty());
    CHECK(lb::ItemTypeForDrop("media", "unknown.xyz") == "media" && lb::ItemTypeForDrop("slide").empty());
}

void TestProjectLibrary() {
    (void)ProjectFs().RemoveAll(kProjectsRoot);
    const std::string file = kProjectsRoot + "/projects.json";
    int64_t clock = 1'000'000'000'000;
    lb::ProjectLibrary lib(file);
    lib.SetClock([&] { return clock; });
    CHECK(lib.Load().ok() && lib.Tree().empty());

    // ---- the tree ----
    auto sunday = lib.Create("Sunday Service");
    auto folder = lib.CreateFolder("2026");
    auto inFolder = lib.Create("Easter", folder.value());
    CHECK(sunday.ok() && folder.ok() && inFolder.ok());
    CHECK(lib.Create("x", "no-such-folder").error().code == bps::Err::NotFound);
    auto tree = lib.Tree();
    CHECK(tree.size() == 3 && tree[0].type == "folder" && tree[0].name == "2026" && tree[0].itemCount == 1);   // folders first
    CHECK(tree[1].type == "project" && tree[1].name == "Easter" && tree[1].depth == 1 && tree[2].name == "Sunday Service" && tree[2].depth == 0);
    CHECK(lib.Create("   ").ok() && lib.Get(lib.Create("  ").value()).value().name == "New project");

    // ---- rename, move, duplicate, delete ----
    CHECK(lib.Rename(sunday.value(), "Sunday Morning").ok() && lib.Get(sunday.value()).value().name == "Sunday Morning");
    CHECK(!lib.Rename(sunday.value(), "  ").ok() && !lib.Rename("nope", "x").ok());
    CHECK(lib.Move(sunday.value(), folder.value()).ok() && lib.Get(sunday.value()).value().parent == folder.value());
    auto inner = lib.CreateFolder("Inner", folder.value());
    CHECK(!lib.Move(folder.value(), inner.value()).ok() && !lib.Move(folder.value(), folder.value()).ok());    // not into itself or below it
    CHECK(lib.Move(inner.value(), "").ok() && lib.Move(folder.value(), inner.value()).ok());                   // ...but a folder can go elsewhere
    CHECK(!lib.Move(sunday.value(), "no-such-folder").ok());
    auto copy = lib.Duplicate(sunday.value());
    CHECK(copy.ok() && lib.Get(copy.value()).value().name == "Sunday Morning (copy)" && lib.Get(copy.value()).value().parent == folder.value());
    CHECK(lib.Delete(copy.value()).ok() && !lib.Get(copy.value()).ok() && !lib.Delete(copy.value()).ok());
    // deleting a folder keeps what is in it: it goes up
    CHECK(lib.Delete(folder.value()).ok());
    CHECK(lib.Get(sunday.value()).value().parent == inner.value());   // the folder was inside "Inner"

    // ---- items ----
    const std::string p = sunday.value();
    auto added = lib.AddItems(p, { Item("show", "C:/shows/Amazing Grace.vgr"), Item("media", "C:/media/Loop.mp4", "Loop"), Item("section", "", "Worship"), Item("overlay", "overlay-1") });
    CHECK(added.ok() && added.value() == std::vector<std::string>({ "pi-1", "pi-2", "pi-3", "pi-4" }));
    CHECK(Names(lib.Get(p).value().items) == std::vector<std::string>({ "Amazing Grace", "Loop", "Worship", "overlay-1" }));   // a name from the file when none is given
    CHECK(!lib.AddItems(p, { Item("show", "a.vgr"), Item("slide", "x") }).ok() && lib.Get(p).value().items.size() == 4);      // one bad item: nothing added
    CHECK(!lib.AddItems(p, { Item("show", "") }).ok() && lib.AddItems(p, { Item("section", "") }, 0).ok());
    CHECK(lib.Get(p).value().items[0].type == "section" && lib.Get(p).value().items[0].id == "pi-5");   // at the front; ids never reused

    // ---- moving items (the mover: lift them out, put them back together) ----
    CHECK(lib.RemoveItem(p, 0).ok());   // back to four
    CHECK(lib.MoveItems(p, { 0 }, 3).ok());
    CHECK(Names(lib.Get(p).value().items) == std::vector<std::string>({ "Loop", "Worship", "Amazing Grace", "overlay-1" }));
    CHECK(lib.MoveItems(p, { 1, 3 }, 0).ok());
    CHECK(Names(lib.Get(p).value().items) == std::vector<std::string>({ "Worship", "overlay-1", "Loop", "Amazing Grace" }));
    CHECK(lib.MoveItems(p, { 0, 1 }, 4).ok() && Names(lib.Get(p).value().items) == std::vector<std::string>({ "Loop", "Amazing Grace", "Worship", "overlay-1" }));
    CHECK(!lib.MoveItems(p, { 9 }, 0).ok() && !lib.MoveItems(p, { 0 }, 9).ok() && lib.MoveItems(p, {}, 0).ok());

    // ---- item details ----
    CHECK(lib.SetItemLayout(p, 1, "Short").ok() && lib.Get(p).value().items[1].layout == "Short" && !lib.SetItemLayout(p, 0, "x").ok());   // only a show has a layout
    CHECK(lib.SetItemColor(p, 2, "#ff0000").ok() && !lib.SetItemColor(p, 1, "#ff0000").ok());                                            // only a section has a colour
    CHECK(lib.RenameItem(p, 0, "Intro loop").ok() && lib.Get(p).value().items[0].name == "Intro loop");
    CHECK(lib.SetSectionsLocked(p, true).ok() && !lib.RemoveItem(p, 2).ok() && !lib.RenameItem(p, 2, "x").ok() && lib.RemoveItem(p, 0).ok());   // locked sections stay
    CHECK(lib.SetSectionsLocked(p, false).ok() && lib.RemoveItem(p, 1).ok());
    CHECK(!lib.RemoveItem(p, 99).ok());

    // ---- drops (the engine decides what a drop is) ----
    lb::DropPayload files{ "files", { { "C:/x/photo.png", "photo.png", "", "{}" }, { "C:/x/song.mp3", "song.mp3", "", "{}" }, { "C:/x/notes.docx", "notes.docx", "", "{}" } } };
    auto dropped = lib.DropOnProject(p, files, 0);
    CHECK(dropped.ok() && dropped.value().size() == 2);   // the .docx is not something a project holds
    CHECK(lib.Get(p).value().items[0].type == "image" && lib.Get(p).value().items[1].type == "audio");
    CHECK(!lib.DropOnProject(p, { "slide", { { "s1", "Slide 1", "", "{}" } } }).ok());                          // the wrong kind of thing
    CHECK(!lib.DropOnProject(p, { "files", { { "C:/x/notes.docx", "notes.docx", "", "{}" } } }).ok());          // nothing usable
    auto scripture = lib.DropOnProject(p, { "scripture", { { "John 3:16", "John 3:16", "", "{\"bible\":\"kjv\"}" } } });
    CHECK(scripture.ok() && lib.Get(p).value().items.back().type == "scripture" && lib.Get(p).value().items.back().metaJson.find("kjv") != std::string::npos);

    // ---- recently used: opened in the last five days, and only when there are two ----
    clock += 1000;
    CHECK(lib.RecentlyUsed().empty() || lib.RecentlyUsed().size() >= 2);
    auto other = lib.Create("Youth Night");
    CHECK(lib.Open(other.value()).ok() && lib.Open(p).ok());
    auto recent = lib.RecentlyUsed();
    CHECK(recent.size() >= 2 && recent[0].id == p);   // newest first
    clock += 6ll * 24 * 3600 * 1000;
    CHECK(lib.RecentlyUsed().empty());                // six days later: nothing is recent
    CHECK(lib.SetArchived(other.value(), true).ok() && lib.Tree().back().archived);   // archived projects go last

    // ---- kept between runs, and the damaged-file cases ----
    {
        lb::ProjectLibrary again(file);
        CHECK(again.Load().ok());
        auto reread = again.Get(p);
        CHECK(reread.ok() && reread.value().name == "Sunday Morning" && reread.value().items.size() == lib.Get(p).value().items.size());
        CHECK(reread.value().items[2].layout == "Short" && again.Tree().size() == lib.Tree().size());
        CHECK(again.Get(other.value()).value().archived);
    }
    CHECK(ProjectFs().Write(file, "{ nope").ok());
    { lb::ProjectLibrary broken(file); CHECK(!broken.Load().ok() && broken.Tree().empty()); }
    // an item of an unknown type, and something filed in a folder that is gone, are tidied on load
    CHECK(ProjectFs().Write(file, R"({"version":1,"folders":[{"id":"folder-1","name":"F","parent":"folder-9"}],"projects":[{"id":"project-1","name":"P","parent":"folder-9","items":[{"id":"pi-1","type":"show","ref":"a.vgr","name":"A"},{"id":"pi-2","type":"weird","ref":"x","name":"X"},{"id":"pi-3","type":"show","ref":"","name":"E"}]}]})").ok());
    {
        lb::ProjectLibrary tidy(file);
        CHECK(tidy.Load().ok() && tidy.Get("project-1").value().items.size() == 1 && tidy.Get("project-1").value().parent.empty());
        CHECK(tidy.Folders()[0].parent.empty());
        CHECK(tidy.Create("New").value() == "project-2");   // ids never collide with the file's
    }
    (void)ProjectFs().RemoveAll(kProjectsRoot);
}
