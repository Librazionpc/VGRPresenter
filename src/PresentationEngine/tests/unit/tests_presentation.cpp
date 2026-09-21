// Unit tests: Presentation Engine (docs/specs/19).
// Split from tests/unit/main.cpp so a single phase can run alone:
//   ./bps_unit_tests presentation
#include "TestHarness.hpp"

void TestPresentationStateMachine() {
    p::PresentationStateMachine sm;
    CHECK(sm.State() == p::PresentationState::Created);
    CHECK(sm.TransitionTo(p::PresentationState::Loaded).ok());
    CHECK(sm.TransitionTo(p::PresentationState::Validated).ok());
    CHECK(sm.TransitionTo(p::PresentationState::Compiled).ok());
    CHECK(sm.TransitionTo(p::PresentationState::Prepared).ok());
    CHECK(sm.TransitionTo(p::PresentationState::Ready).ok());
    CHECK(sm.TransitionTo(p::PresentationState::Live).ok());
    CHECK(sm.TransitionTo(p::PresentationState::Paused).ok());
    CHECK(sm.TransitionTo(p::PresentationState::Live).ok());   // resume
    CHECK(sm.TransitionTo(p::PresentationState::Finished).ok());
    CHECK(!sm.TransitionTo(p::PresentationState::Live).ok());  // Finished -> Live invalid
    // Direct Created -> Live must be rejected.
    p::PresentationStateMachine sm2;
    CHECK(!sm2.TransitionTo(p::PresentationState::Live).ok());
    // Closed reachable from anywhere.
    CHECK(sm.TransitionTo(p::PresentationState::Closed).ok());
}
void TestPresentationNavigator() {
    p::Presentation pres;
    pres.id = "pres-1";
    pres.name = "Sunday";
    p::Slide s1;
    s1.id = "s1";
    s1.title = "Intro";
    s1.tags = {"opening"};
    s1.sections = {"A"};
    p::Slide s2;
    s2.id = "s2";
    s2.title = "Amazing Grace";
    s2.text = "Amazing grace how sweet the sound";
    s2.tags = {"hymn", "grace"};
    p::Slide hidden;
    hidden.id = "hidden";
    hidden.title = "Hidden";
    hidden.hidden = true;
    pres.slides = {s1, hidden, s2};
    p::PresentationNavigator nav;
    nav.Open(pres);
    CHECK(nav.Count() == 2);   // hidden skipped
    CHECK(nav.Current() == 0);
    CHECK(nav.Next().ok());
    CHECK(nav.CurrentSlide() && nav.CurrentSlide()->id == "s2");
    CHECK(!nav.Next().ok());   // already last
    CHECK(nav.Previous().ok());
    CHECK(nav.CurrentSlide()->id == "s1");
    CHECK(nav.JumpById("s2").ok());
    CHECK(nav.JumpByTag("hymn").ok());
    CHECK(nav.JumpBySection("A").ok());
    auto found = nav.Search("grace");
    CHECK(found.ok() && found.value() == 2);
    CHECK(nav.Back().ok());
    CHECK(nav.HistoryDepth() > 0);
}
void TestPresentationTimeline() {
    p::PresentationTimeline timeline;
    int fired = 0;
    auto cue = std::make_shared<p::ScriptCue>(
        "cue1", 1.5,
        [&](p::CueContext&) { ++fired; return Ok(); });
    CHECK(timeline.AddCue(cue).ok());
    CHECK(!timeline.AddCue(cue).ok());   // duplicate id rejected
    CHECK(timeline.Size() == 1);
    p::CueContext ctx;
    CHECK(timeline.Tick(1.0, ctx) == 0);       // not due yet
    CHECK(timeline.Tick(2.0, ctx) == 1);       // fires once
    CHECK(timeline.Tick(3.0, ctx) == 0);       // never fires again
    CHECK(fired == 1);
    timeline.Reset();
    CHECK(timeline.Tick(2.0, ctx) == 1);       // rewind
    CHECK(fired == 2);
}
void TestPresentationEngine() {
    auto& eng = p::PresentationEngine::Instance();
    CHECK(eng.Initialize().ok());
    CHECK(eng.Start().ok());
    auto created = eng.CreatePresentation("Sunday Service");
    CHECK(created.ok() && !created.value().empty());
    const std::string id = created.value();
    p::Slide s1;
    s1.id = "slide-1";
    s1.title = "Welcome";
    s1.text = "Good morning!";
    CHECK(eng.AddSlide(id, s1).ok());
    p::Slide s2;
    s2.id = "slide-2";
    s2.title = "Amazing Grace";
    s2.text = "Amazing grace how sweet the sound";
    s2.durationMs = 3000;
    CHECK(eng.AddSlide(id, s2).ok());
    CHECK(eng.PresentationCount() == 1);
    // Full pipeline: validate -> compile -> prepare -> go live.
    auto issues = eng.Validate(id);
    CHECK(issues.ok() && !p::PresentationValidator::HasErrors(issues.value()));
    CHECK(eng.Compile(id).ok());
    CHECK(eng.Prepare(id).ok());
    CHECK(eng.State() == p::PresentationState::Ready);
    CHECK(eng.GoLive().ok());
    CHECK(eng.State() == p::PresentationState::Live);
    // Navigation publishes SlideChanged.
    int slideEvents = 0;
    Subscription sub = EventBus::Instance().Subscribe<events::SlideChanged>(
        [&](const events::SlideChanged&) { ++slideEvents; }, 0);
    CHECK(eng.Next().ok());
    CHECK(eng.CurrentIndex() == 1);
    CHECK(eng.CurrentSlide() && eng.CurrentSlide()->id == "slide-2");
    CHECK(slideEvents >= 1);
    (void)EventBus::Instance().Unsubscribe(sub);
    // Timeline cue + playback tick.
    auto cue = std::make_shared<p::SlideCue>("jump", 0.5, "slide-1");
    CHECK(eng.AddCue(cue).ok());
    auto fired = eng.Tick(1.0);
    CHECK(fired.ok() && fired.value() == 1);
    CHECK(eng.CurrentIndex() == 0);   // slide cue jumped back
    // Pause/resume/stop.
    CHECK(eng.Pause().ok());
    CHECK(eng.State() == p::PresentationState::Paused);
    CHECK(eng.Resume().ok());
    CHECK(eng.StopPlayback().ok());
    CHECK(eng.State() == p::PresentationState::Stopped);
    // Session + recovery.
    CHECK(eng.JumpTo(1).ok());
    CHECK(eng.SaveSession().ok());
    auto snap = eng.Recover();
    CHECK(snap.ok() && snap.value().presentationId == id);
    CHECK(snap.ok() && snap.value().slideIndex == 1);
    // Duplicate + delete.
    CHECK(eng.Duplicate(id).ok());
    CHECK(eng.PresentationCount() == 2);
    CHECK(eng.Delete(id).ok());
    CHECK(eng.PresentationCount() == 1);
    CHECK(eng.Stop().ok());
    CHECK(eng.Shutdown().ok());
}

// ===========================================================================
// Phase 9 — Search & Indexing Engine (docs/specs/20)
// ===========================================================================

// ===========================================================================
// PresentationSerializer + PresentationDocument (.vgr Show open/save) —
// docs/specs/19, docs/specs/22, architecture/ProjectSystem.md §Document lifecycle
// ===========================================================================
#include "modules/presentation/PresentationDocument.hpp"
#include "modules/presentation/PresentationSerializer.hpp"
#include "modules/project/DocumentManager.hpp"

namespace {

p::Presentation MakeShow() {
    p::Presentation pres;
    pres.id = "show-abc";
    pres.name = "Sunday Service";
    pres.mode = p::PlaybackMode::Timed;
    pres.defaultTransitionMs = 750;
    p::Slide s1;
    s1.id = "s1";
    s1.title = "Welcome";
    s1.text = "Line one\nLine two — ünïcode \"quoted\"";
    s1.tags = {"intro", "welcome"};
    s1.assetIds = {"asset-1", "asset-2"};
    s1.background = "#ff112233";
    s1.durationMs = 4000;
    s1.metaJson = R"({"tagColor":"#3574f0","ref":"John 3:16"})";
    p::ContentBlock b;
    b.id = "item-1";
    b.kind = "text";
    b.text = "Hello world";
    b.x = 120.5;
    b.y = 64;
    b.width = 640;
    b.height = 90;
    b.style.padding = 8;
    b.style.backgroundColor = "#80ff0000";
    b.style.cornerRadius = 6;
    b.style.borderEnabled = true;
    b.style.borderWidth = 3;
    b.style.borderColor = "#ffffff00";
    s1.blocks.push_back(b);
    p::ContentBlock clock;
    clock.id = "item-2";
    clock.kind = "clock";
    clock.metaJson = R"({"format":"24h","showSeconds":true})";
    s1.blocks.push_back(clock);
    pres.slides.push_back(s1);
    p::Slide s2;
    s2.id = "s2";
    s2.title = "Empty";
    s2.hidden = true;
    pres.slides.push_back(s2);
    return pres;
}

} // namespace

void TestPresentationSerializer() {
    p::PresentationSerializer ser;
    const p::Presentation src = MakeShow();

    auto json = ser.Serialize(src);
    CHECK(json.ok());
    auto back = ser.Deserialize(json.value());
    CHECK(back.ok());
    if (back.ok()) {
        const auto& q = back.value();
        CHECK(q.id == "show-abc" && q.name == "Sunday Service");
        CHECK(q.mode == p::PlaybackMode::Timed);
        CHECK(q.defaultTransitionMs == 750);
        CHECK(q.slides.size() == 2);
        if (q.slides.size() == 2) {
            const auto& s = q.slides[0];
            CHECK(s.id == "s1" && s.title == "Welcome");
            CHECK(s.text == src.slides[0].text);              // newlines, unicode, quotes survive
            CHECK(s.tags == src.slides[0].tags);
            CHECK(s.assetIds == src.slides[0].assetIds);
            CHECK(s.background == "#ff112233");
            CHECK(s.durationMs == 4000);
            auto smeta = bps::json::Parse(s.metaJson);
            CHECK(smeta.ok() && smeta.value().Find("ref") &&
                  smeta.value().Find("ref")->asString() == "John 3:16");
            CHECK(s.blocks.size() == 2);
            if (s.blocks.size() == 2) {
                const auto& b = s.blocks[0];
                CHECK(b.id == "item-1" && b.kind == "text" && b.text == "Hello world");
                CHECK(b.x == 120.5 && b.y == 64 && b.width == 640 && b.height == 90);
                CHECK(b.style.padding == 8 && b.style.cornerRadius == 6);
                CHECK(b.style.backgroundColor == "#80ff0000");
                CHECK(b.style.borderEnabled && b.style.borderWidth == 3);
                CHECK(b.style.borderColor == "#ffffff00");
                // Per-kind meta survives as real JSON.
                auto meta = bps::json::Parse(s.blocks[1].metaJson);
                CHECK(meta.ok() && meta.value().Find("format") &&
                      meta.value().Find("format")->asString() == "24h");
                CHECK(meta.ok() && meta.value().Find("showSeconds") &&
                      meta.value().Find("showSeconds")->asBool());
            }
            CHECK(q.slides[1].hidden && q.slides[1].blocks.empty());
        }
    }

    // Forward compatibility: unknown fields are ignored; missing ids are generated.
    auto lenient = ser.Deserialize(
        R"({"schemaVersion":1,"futureField":{"a":1},"slides":[{"title":"x","blocks":[{"kind":"image"}]}]})");
    CHECK(lenient.ok());
    if (lenient.ok() && lenient.value().slides.size() == 1) {
        CHECK(lenient.value().slides[0].id == "slide-1");
        CHECK(lenient.value().slides[0].blocks.size() == 1);
        CHECK(lenient.value().slides[0].blocks[0].id == "block-1");
    }

    // Rejections.
    CHECK(!ser.Deserialize("not json").ok());
    CHECK(!ser.Deserialize("[]").ok());                                   // not an object
    CHECK(!ser.Deserialize(R"({"slides":[]})").ok());                     // no schemaVersion
    CHECK(!ser.Deserialize(R"({"schemaVersion":99,"slides":[]})").ok());  // newer than us
    CHECK(!ser.Deserialize(R"({"schemaVersion":1,"slides":{}})").ok());   // slides not an array
    CHECK(!ser.Deserialize(R"({"schemaVersion":1,"slides":[5]})").ok());  // slide not an object
}

void TestPresentationDocument() {
    auto& eng = p::PresentationEngine::Instance();
    CHECK(eng.Initialize().ok());
    auto doc = eng.Document();
    CHECK(doc != nullptr);
    if (!doc) return;

    // The engine registered the handler with the DocumentManager.
    auto types = bps::project::DocumentManager::Instance().HandlerTypes();
    CHECK(std::find(types.begin(), types.end(), "presentation") != types.end());

    const std::string path = "/tmp/bps_show_test.vgr";
    auto& fs = bps::platform::PlatformAccessor::Get().Filesystem();
    (void)fs.Remove(path);
    (void)fs.Remove(path + ".tmp");
    (void)fs.Remove(path + ".bak");

    // New show: dirty until saved, no path, refuses a path-less Save.
    doc->New("Sunday Service");
    CHECK(doc->HasDocument() && doc->IsDirty() && doc->Path().empty());
    CHECK(!doc->Save("").ok());

    // Edit + Save writes a real, loadable .vgr and clears dirty.
    doc->Replace(MakeShow());
    CHECK(doc->IsDirty());
    CHECK(doc->Save(path).ok());
    CHECK(!doc->IsDirty());
    CHECK(doc->Path() == path);
    CHECK(fs.Exists(path));
    CHECK(!fs.Exists(path + ".tmp") && !fs.Exists(path + ".bak"));   // no leftovers

    // The file is a genuine .vgr container: header + CRC verified by the reader.
    auto bytes = fs.ReadBinary(path);
    CHECK(bytes.ok());
    if (bytes.ok()) {
        auto vdoc = v::VgrSerializer::Read(bytes.value());
        CHECK(vdoc.ok());
        if (vdoc.ok()) {
            CHECK(vdoc.value().header.type == v::DocumentType::Show);
            CHECK(vdoc.value().header.uuid == "show-abc");
            CHECK(vdoc.value().header.name == "Sunday Service");
            // Dependency manifest lists the referenced assets.
            CHECK(vdoc.value().header.dependencyIds.size() == 2);
        }
    }

    // Overwrite an existing file (the swap path) — still one clean file.
    auto edited = doc->Snapshot();
    edited.slides[0].title = "Welcome (edited)";
    doc->Replace(edited);
    CHECK(doc->Save("").ok());   // empty path = same file
    CHECK(!fs.Exists(path + ".tmp") && !fs.Exists(path + ".bak"));

    // Open into a fresh handler reproduces the saved show.
    p::PresentationDocument reopened;
    CHECK(reopened.Open(path).ok());
    CHECK(!reopened.IsDirty() && reopened.Path() == path);
    auto model = reopened.Snapshot();
    CHECK(model.slides.size() == 2);
    if (model.slides.size() == 2) {
        CHECK(model.slides[0].title == "Welcome (edited)");
        CHECK(model.slides[0].blocks.size() == 2);
        CHECK(model.slides[0].blocks[0].text == "Hello world");
    }

    // Through the DocumentManager (how a frontend/IPC client reaches it).
    auto id = bps::project::DocumentManager::Instance().Open("presentation", path);
    CHECK(id.ok());
    if (id.ok()) CHECK(bps::project::DocumentManager::Instance().Close(id.value()).ok());

    // A damaged file is rejected and the open document is left untouched.
    if (bytes.ok()) {
        auto broken = bytes.value();
        broken[broken.size() / 2] ^= 0xFF;
        const std::string badPath = "/tmp/bps_show_broken.vgr";
        CHECK(fs.WriteBinary(badPath, broken).ok());
        p::PresentationDocument guard;
        CHECK(guard.Open(path).ok());
        CHECK(!guard.Open(badPath).ok());
        CHECK(guard.Snapshot().slides.size() == 2);   // unchanged
        // Truncated file too.
        auto truncated = bytes.value();
        truncated.resize(truncated.size() / 2);
        CHECK(fs.WriteBinary(badPath, truncated).ok());
        CHECK(!guard.Open(badPath).ok());
        (void)fs.Remove(badPath);
    }
    CHECK(!reopened.Open("/tmp/definitely_missing_show.vgr").ok());

    // A .vgr that is not a show (a Theme) is refused with a clear error.
    {
        v::VgrDocument theme;
        theme.header.type = v::DocumentType::Theme;
        theme.header.uuid = "theme-1";
        theme.header.name = "Dark";
        theme.documentJson = "{}";
        auto tb = v::VgrSerializer::Write(theme);
        CHECK(tb.ok());
        const std::string themePath = "/tmp/bps_theme_test.vgr";
        if (tb.ok()) CHECK(fs.WriteBinary(themePath, tb.value()).ok());
        p::PresentationDocument other;
        CHECK(!other.Open(themePath).ok());
        (void)fs.Remove(themePath);
    }

    (void)fs.Remove(path);
    CHECK(eng.Shutdown().ok());
    // Shutdown unregisters the handler again.
    auto after = bps::project::DocumentManager::Instance().HandlerTypes();
    CHECK(std::find(after.begin(), after.end(), "presentation") == after.end());
}
// ===========================================================================
// Show structure: categories (songs / notes / pastor notes) with assigned
// templates, overlays, standalone .vgr templates, and the show library.
// ===========================================================================
#include "modules/presentation/PresentationTemplates.hpp"
#include "modules/presentation/PresentationValidator.hpp"
#include "modules/presentation/ShowLibrary.hpp"
#include "modules/vgr/VgrFile.hpp"

namespace {

p::ContentBlock TBlock(const std::string& id, const std::string& bind, double x, double y,
                       double w, double h) {
    p::ContentBlock b;
    b.id = id;
    b.kind = "text";
    b.bind = bind;
    b.text = "placeholder";
    b.x = x; b.y = y; b.width = w; b.height = h;
    return b;
}

// A service: a song, plain notes, and pastor notes — each category with its own
// template.
p::Presentation MakeStructuredShow() {
    p::Presentation show;
    show.id = "show-structured";
    show.name = "Sunday";

    p::SlideTemplate songT;
    songT.id = "t-song";
    songT.name = "Song";
    songT.contentType = "song";
    songT.background = "#ff000000";
    songT.blocks = {TBlock("lyrics", "text", 100, 200, 1720, 600), TBlock("ref", "ref", 100, 900, 800, 60)};

    p::SlideTemplate notesT;
    notesT.id = "t-notes";
    notesT.name = "Notes";
    notesT.contentType = "notes";
    notesT.background = "#ff102030";
    notesT.blocks = {TBlock("body", "text", 200, 150, 1500, 700)};

    p::SlideTemplate pastorT;
    pastorT.id = "t-pastor";
    pastorT.name = "Pastor Notes";
    pastorT.contentType = "pastor-notes";
    pastorT.blocks = {TBlock("body", "notes", 50, 50, 900, 900)};

    show.templates = {songT, notesT, pastorT};

    show.categories = {
        {"c-songs", "Songs", "song", "t-song", {}, "{}"},
        {"c-notes", "Notes", "notes", "t-notes", {}, "{}"},
        {"c-pastor", "Pastor Notes", "pastor-notes", "t-pastor", {"stage"}, "{}"},
    };

    p::Slide s1;   // a song slide
    s1.id = "s1"; s1.title = "Amazing Grace"; s1.text = "Amazing grace how sweet the sound";
    s1.categoryId = "c-songs";
    s1.metaJson = R"({"ref":"Hymn 12"})";
    p::Slide s2;   // notes with a per-slide override of the body block
    s2.id = "s2"; s2.title = "Announcements"; s2.text = "Youth camp signup";
    s2.categoryId = "c-notes";
    p::ContentBlock over = TBlock("body", "", 0, 0, 500, 300);
    over.text = "custom body";
    s2.blocks.push_back(over);
    p::ContentBlock extra;
    extra.id = "sticker"; extra.kind = "image";
    s2.blocks.push_back(extra);
    p::Slide s3;   // pastor notes
    s3.id = "s3"; s3.title = "Sermon"; s3.notes = "Point one: grace";
    s3.categoryId = "c-pastor";
    p::Slide s4;   // uncategorised
    s4.id = "s4"; s4.title = "Loose"; s4.text = "no category"; s4.background = "#ff445566";
    p::ContentBlock own = TBlock("free", "", 10, 10, 100, 40);
    s4.blocks.push_back(own);
    show.slides = {s1, s2, s3, s4};

    // Overlays: a show-wide logo, a songs-only lyric-safe strip, a one-slide badge,
    // a stage-only clock, and a disabled one.
    auto overlay = [](const std::string& id, p::OverlayScope scope, const std::string& target,
                      std::vector<std::string> outputs, bool enabled) {
        p::Overlay o;
        o.id = id; o.name = id; o.scope = scope; o.targetId = target;
        o.outputs = std::move(outputs); o.enabled = enabled;
        p::ContentBlock b;
        b.id = id + "-block"; b.kind = "image";
        o.blocks.push_back(b);
        return o;
    };
    show.overlays = {
        overlay("o-logo", p::OverlayScope::All, "", {}, true),
        overlay("o-songs", p::OverlayScope::Category, "c-songs", {}, true),
        overlay("o-badge", p::OverlayScope::Slide, "s2", {}, true),
        overlay("o-clock", p::OverlayScope::All, "", {"stage"}, true),
        overlay("o-off", p::OverlayScope::All, "", {}, false),
    };
    return show;
}

std::vector<std::string> BlockIds(const std::vector<p::ContentBlock>& blocks) {
    std::vector<std::string> ids;
    for (const auto& b : blocks) ids.push_back(b.id);
    return ids;
}

} // namespace

void TestShowStructure() {
    const p::Presentation show = MakeStructuredShow();

    // --- Round trip: categories, templates, overlays, categoryId, bind survive ---
    p::PresentationSerializer ser;
    auto json = ser.Serialize(show);
    CHECK(json.ok());
    auto back = ser.Deserialize(json.ok() ? json.value() : std::string());
    CHECK(back.ok());
    if (back.ok()) {
        const auto& q = back.value();
        CHECK(q.categories.size() == 3 && q.templates.size() == 3 && q.overlays.size() == 5);
        if (q.categories.size() == 3) {
            CHECK(q.categories[2].id == "c-pastor" && q.categories[2].templateId == "t-pastor");
            CHECK(q.categories[2].outputs == std::vector<std::string>{"stage"});
            CHECK(q.categories[0].contentType == "song");
        }
        if (q.templates.size() == 3) {
            CHECK(q.templates[0].blocks.size() == 2 && q.templates[0].blocks[0].bind == "text");
            CHECK(q.templates[0].background == "#ff000000");
        }
        if (q.overlays.size() == 5) {
            CHECK(q.overlays[1].scope == p::OverlayScope::Category && q.overlays[1].targetId == "c-songs");
            CHECK(q.overlays[3].outputs == std::vector<std::string>{"stage"});
            CHECK(!q.overlays[4].enabled);
        }
        CHECK(q.slides.size() == 4 && q.slides[2].categoryId == "c-pastor");
    }
    // A show saved before these existed still opens (all three are optional).
    auto legacy = ser.Deserialize(R"({"schemaVersion":1,"slides":[{"id":"a"}]})");
    CHECK(legacy.ok() && legacy.value().categories.empty() && legacy.value().overlays.empty());
    CHECK(!ser.Deserialize(R"({"schemaVersion":1,"categories":{}})").ok());
    CHECK(!ser.Deserialize(R"({"schemaVersion":1,"overlays":[3]})").ok());

    // --- Each category resolves through ITS OWN template ---
    auto song = p::SlideResolver::Resolve(show, "s1");
    CHECK(song.ok());
    if (song.ok()) {
        CHECK(song.value().templateId == "t-song");
        CHECK(song.value().background == "#ff000000");            // template's (slide unset)
        CHECK(BlockIds(song.value().blocks) == std::vector<std::string>({"lyrics", "ref"}));
        CHECK(song.value().blocks[0].text == "Amazing grace how sweet the sound");   // bound: text
        CHECK(song.value().blocks[1].text == "Hymn 12");                              // bound: meta ref
        CHECK(song.value().blocks[0].x == 100 && song.value().blocks[0].width == 1720);
    }
    auto notes = p::SlideResolver::Resolve(show, "s2");
    CHECK(notes.ok());
    if (notes.ok()) {
        CHECK(notes.value().templateId == "t-notes");
        CHECK(notes.value().background == "#ff102030");
        // Same-id slide block REPLACES the template block; unknown ids are extras.
        CHECK(BlockIds(notes.value().blocks) == std::vector<std::string>({"body", "sticker"}));
        CHECK(notes.value().blocks[0].text == "custom body" && notes.value().blocks[0].width == 500);
    }
    auto pastor = p::SlideResolver::Resolve(show, "s3");
    CHECK(pastor.ok());
    if (pastor.ok()) {
        CHECK(pastor.value().templateId == "t-pastor");
        CHECK(pastor.value().blocks.size() == 1 && pastor.value().blocks[0].text == "Point one: grace");
    }
    auto loose = p::SlideResolver::Resolve(show, "s4");   // no category -> no template
    CHECK(loose.ok());
    if (loose.ok()) {
        CHECK(loose.value().templateId.empty());
        CHECK(loose.value().background == "#ff445566");
        CHECK(BlockIds(loose.value().blocks) == std::vector<std::string>({"free"}));
    }
    CHECK(!p::SlideResolver::Resolve(show, "nope").ok());

    // Restyling a template restyles every slide in its category.
    p::Presentation restyled = show;
    restyled.templates[0].blocks[0].x = 999;
    auto song2 = p::SlideResolver::Resolve(restyled, "s1");
    CHECK(song2.ok() && song2.value().blocks[0].x == 999);

    // --- Output filtering: pastor notes are stage-only ---
    auto onAudience = p::SlideResolver::Resolve(show, "s3", "audience");
    CHECK(onAudience.ok() && !onAudience.value().visible && onAudience.value().blocks.empty());
    auto onStage = p::SlideResolver::Resolve(show, "s3", "stage");
    CHECK(onStage.ok() && onStage.value().visible && onStage.value().blocks.size() == 1);
    auto songAudience = p::SlideResolver::Resolve(show, "s1", "audience");
    CHECK(songAudience.ok() && songAudience.value().visible);   // no filter on songs

    // --- Overlays: scope, output filter, enabled ---
    auto ov = [&](const char* slide, const char* output) {
        auto r = p::SlideResolver::Resolve(show, slide, output);
        return r.ok() ? BlockIds(r.value().overlayBlocks) : std::vector<std::string>{};
    };
    CHECK(ov("s1", "") == std::vector<std::string>({"o-logo-block", "o-songs-block", "o-clock-block"}));
    CHECK(ov("s1", "audience") == std::vector<std::string>({"o-logo-block", "o-songs-block"}));   // clock is stage-only
    CHECK(ov("s2", "audience") == std::vector<std::string>({"o-logo-block", "o-badge-block"}));   // one-slide badge
    CHECK(ov("s4", "audience") == std::vector<std::string>({"o-logo-block"}));                    // uncategorised: only All
    // (the disabled overlay never appears anywhere)
    for (const char* s : {"s1", "s2", "s3", "s4"})
        for (const auto& id : ov(s, ""))
            CHECK(id != "o-off-block");

    // --- Integrity: dangling references are reported ---
    CHECK(p::SlideResolver::DanglingReferences(show).empty());
    p::Presentation broken = show;
    broken.categories[0].templateId = "t-gone";
    broken.slides[3].categoryId = "c-gone";
    broken.overlays[1].targetId = "c-gone";
    broken.overlays[2].targetId = "s-gone";
    CHECK(p::SlideResolver::DanglingReferences(broken).size() == 4);
    p::PresentationValidator validator;
    auto issues = validator.Validate(broken, nullptr);
    size_t refWarnings = 0;
    for (const auto& i : issues)
        if (i.message.find("missing") != std::string::npos) ++refWarnings;
    CHECK(refWarnings >= 4);
    // A dangling category simply renders unstyled — it must not crash or throw.
    auto unstyled = p::SlideResolver::Resolve(broken, "s1");
    CHECK(unstyled.ok() && unstyled.value().templateId.empty());
}

void TestTemplateFile() {
    const p::Presentation show = MakeStructuredShow();
    auto& fs = bps::platform::PlatformAccessor::Get().Filesystem();
    const std::string path = "/tmp/bps_template_test.vgr";
    (void)fs.Remove(path);

    CHECK(p::TemplateFile::Save(show.templates[0], path).ok());
    CHECK(fs.Exists(path));
    auto loaded = p::TemplateFile::Load(path);
    CHECK(loaded.ok());
    if (loaded.ok()) {
        CHECK(loaded.value().id == "t-song" && loaded.value().name == "Song");
        CHECK(loaded.value().contentType == "song");
        CHECK(loaded.value().background == "#ff000000");
        CHECK(loaded.value().blocks.size() == 2 && loaded.value().blocks[1].bind == "ref");
    }
    // It is a real .vgr Template document.
    auto raw = bps::vgr::VgrFile::Read(path);
    CHECK(raw.ok() && raw.value().header.type == v::DocumentType::Template);

    // Overwrite is safe; a Show file is not a template; a missing file fails.
    CHECK(p::TemplateFile::Save(show.templates[1], path).ok());
    auto second = p::TemplateFile::Load(path);
    CHECK(second.ok() && second.value().id == "t-notes");
    CHECK(!fs.Exists(path + ".tmp") && !fs.Exists(path + ".bak"));

    const std::string showPath = "/tmp/bps_template_notashow.vgr";
    p::PresentationDocument doc;
    doc.Replace(show);
    CHECK(doc.Save(showPath).ok());
    CHECK(!p::TemplateFile::Load(showPath).ok());
    CHECK(!p::TemplateFile::Load("/tmp/bps_no_such_template.vgr").ok());
    (void)fs.Remove(path);
    (void)fs.Remove(showPath);
}

void TestShowLibrary() {
    auto& fs = bps::platform::PlatformAccessor::Get().Filesystem();
    const std::string root = "/tmp/bps_library_test";
    (void)fs.RemoveAll(root);

    p::ShowLibrary lib(root);
    CHECK(lib.Refresh().ok());                    // creates the root
    CHECK(fs.IsDirectory(root));
    CHECK(lib.Categories().empty() && lib.Shows().empty());

    // Categories: valid names only, no duplicates.
    CHECK(lib.CreateCategory("Youth").ok());
    CHECK(lib.CreateCategory("conferences").ok());
    CHECK(!lib.CreateCategory("Youth").ok());       // exists
    CHECK(!lib.CreateCategory("").ok());
    CHECK(!lib.CreateCategory("a/b").ok());
    CHECK(!lib.CreateCategory("..").ok());
    CHECK(!lib.CreateCategory("trailing ").ok());
    CHECK(lib.Categories() == std::vector<std::string>({"conferences", "Youth"}));   // case-insensitive order

    // Save shows into the library (NewShowPath never overwrites).
    auto saveShow = [&](const std::string& category, const std::string& name) {
        auto path = lib.NewShowPath(category, name);
        CHECK(path.ok());
        if (!path.ok()) return std::string();
        p::PresentationDocument doc;
        doc.New(name);
        p::Presentation m = doc.Snapshot();
        p::Slide s;
        s.id = "s1";
        m.slides.push_back(s);
        m.slides.push_back(s);
        m.categories.push_back({"c1", "Songs", "song", "", {}, "{}"});
        doc.Replace(m);
        CHECK(doc.Save(path.value()).ok());
        return path.value();
    };
    const std::string a = saveShow("", "Sunday Service");
    const std::string b = saveShow("Youth", "Friday Night");
    const std::string c = saveShow("conferences", "Easter: 2027?");     // unsafe chars sanitised
    const std::string a2 = saveShow("", "Sunday Service");              // same name -> "(2)"
    CHECK(a != a2 && a2.find("(2)") != std::string::npos);
    CHECK(c.find('?') == std::string::npos && c.find("Easter") != std::string::npos);

    CHECK(lib.Refresh().ok());
    CHECK(lib.Shows().size() == 4);
    CHECK(lib.ShowsIn("").size() == 2);
    CHECK(lib.ShowsIn("Youth").size() == 1);
    CHECK(lib.ShowsIn("Youth")[0].name == "Friday Night");
    CHECK(lib.ShowsIn("Youth")[0].slideCount == 2);

    // Search by show name or category, case-insensitively.
    CHECK(lib.Search("sunday").size() == 2);
    CHECK(lib.Search("YOUTH").size() == 1);              // by category
    CHECK(lib.Search("easter").size() == 1);
    CHECK(lib.Search("").size() == 4);
    CHECK(lib.Search("zzz").empty());

    // Move between categories; refuses to overwrite.
    auto moved = lib.MoveShow(a, "Youth");
    CHECK(moved.ok());
    CHECK(lib.ShowsIn("Youth").size() == 2 && lib.ShowsIn("").size() == 1);
    CHECK(!fs.Exists(a));
    // Moving onto an existing file name is refused and leaves the file where it was.
    const std::string dup = saveShow("", "Friday Night");   // same file name as the one in Youth
    CHECK(!lib.MoveShow(dup, "Youth").ok());
    CHECK(fs.Exists(dup));
    (void)fs.Remove(dup);
    CHECK(lib.Refresh().ok());
    CHECK(!lib.MoveShow(b, "NoSuchCategory").ok());
    CHECK(!lib.MoveShow("/tmp/bps_library_test/missing.vgr", "Youth").ok());
    auto out = lib.MoveShow(moved.ok() ? moved.value() : std::string(), "");   // back out of every category
    CHECK(out.ok());

    // Rename / remove categories.
    CHECK(lib.RenameCategory("Youth", "Youth Ministry").ok());
    CHECK(lib.ShowsIn("Youth Ministry").size() >= 1 && lib.ShowsIn("Youth").empty());
    CHECK(!lib.RenameCategory("Youth", "X").ok());        // no longer exists
    CHECK(!lib.RemoveCategory("Youth Ministry").ok());    // not empty
    CHECK(lib.CreateCategory("Empty").ok());
    CHECK(lib.RemoveCategory("Empty").ok());
    CHECK(!lib.RemoveCategory("Empty").ok());

    // A damaged or non-show .vgr is reported, never silently dropped.
    auto raw = fs.ReadBinary(c);
    CHECK(raw.ok());
    if (raw.ok()) {
        auto damaged = raw.value();
        damaged[damaged.size() / 2] ^= 0xFF;
        CHECK(fs.WriteBinary(fs.Join(root, "damaged.vgr"), damaged).ok());
    }
    CHECK(lib.Refresh().ok());
    bool reported = false;
    for (const auto& pr : lib.Problems()) reported = reported || pr.find("damaged.vgr") != std::string::npos;
    CHECK(reported);
    CHECK(lib.Shows().size() == 4);                       // the damaged one is not listed as a show

    // Safe names.
    CHECK(p::ShowLibrary::SafeName("a/b:c*d") == "a-b-c-d");
    CHECK(p::ShowLibrary::SafeName("  ...  ") == "Untitled");
    CHECK(p::ShowLibrary::SafeName("Normal Name") == "Normal Name");

    (void)fs.RemoveAll(root);
}

// ===========================================================================
// ShowEditor — the engine owns CRUD for categories, templates, overlays, slides;
// PresentationDocument::Edit makes edits atomic; ShowLibrary CRUD for shows.
// ===========================================================================
#include "modules/presentation/ShowEditor.hpp"

namespace {

std::string Snap(const p::Presentation& show) {
    p::PresentationSerializer ser;
    auto s = ser.Serialize(show);
    return s.ok() ? s.value() : std::string("<serialize failed>");
}

p::SlideTemplate NamedTemplate(const std::string& name) {
    p::SlideTemplate t;
    t.name = name;
    t.contentType = "song";
    p::ContentBlock b;
    b.id = "body";
    b.bind = "text";
    t.blocks.push_back(b);
    return t;
}

} // namespace

void TestShowEditor() {
    using E = p::ShowEditor;
    p::Presentation show;
    show.id = "s";
    show.name = "Sunday";

    // ---- Templates ----
    p::SlideTemplate unnamed;
    CHECK(!E::AddTemplate(show, unnamed).ok());                       // needs a name
    auto tSong = E::AddTemplate(show, NamedTemplate("Song"));
    CHECK(tSong.ok() && tSong.value() == "template-1");
    auto tNotes = E::AddTemplate(show, NamedTemplate("Notes"));
    CHECK(tNotes.ok() && tNotes.value() == "template-2");
    p::SlideTemplate clash = NamedTemplate("Clash");
    clash.id = "template-1";
    CHECK(!E::AddTemplate(show, clash).ok());                         // explicit id must be free
    CHECK(show.templates.size() == 2);

    // ---- Categories ----
    p::Category noName;
    CHECK(!E::AddCategory(show, noName).ok());
    p::Category badTemplate;
    badTemplate.name = "X";
    badTemplate.templateId = "template-99";
    CHECK(!E::AddCategory(show, badTemplate).ok());                   // template must exist
    p::Category songs;
    songs.name = "Songs";
    songs.contentType = "song";
    songs.templateId = tSong.value();
    auto cSongs = E::AddCategory(show, songs);
    CHECK(cSongs.ok() && cSongs.value() == "category-1");
    p::Category pastor;
    pastor.name = "Pastor Notes";
    pastor.contentType = "pastor-notes";
    auto cPastor = E::AddCategory(show, pastor);
    CHECK(cPastor.ok() && cPastor.value() == "category-2");
    CHECK(show.categories[1].templateId.empty());

    // Update: partial, validated, and a refused update changes NOTHING.
    const std::string before = Snap(show);
    p::CategoryPatch bad;
    bad.name = "";
    CHECK(!E::UpdateCategory(show, cPastor.value(), bad).ok());
    p::CategoryPatch badT;
    badT.name = "Renamed";                       // valid part…
    badT.templateId = "template-404";            // …with an invalid part
    CHECK(!E::UpdateCategory(show, cPastor.value(), badT).ok());
    CHECK(Snap(show) == before);                 // atomic: the valid half was NOT applied
    CHECK(!E::UpdateCategory(show, "category-404", p::CategoryPatch{}).ok());
    p::CategoryPatch good;
    good.name = "Pastor's Notes";
    good.outputs = std::vector<std::string>{"stage"};
    CHECK(E::UpdateCategory(show, cPastor.value(), good).ok());
    CHECK(show.categories[1].name == "Pastor's Notes" && show.categories[1].outputs.size() == 1);
    CHECK(show.categories[1].contentType == "pastor-notes");          // untouched field kept

    CHECK(E::AssignTemplate(show, cPastor.value(), tNotes.value()).ok());
    CHECK(show.categories[1].templateId == "template-2");
    CHECK(!E::AssignTemplate(show, cPastor.value(), "template-404").ok());
    CHECK(E::AssignTemplate(show, cPastor.value(), "").ok());         // clear
    CHECK(show.categories[1].templateId.empty());
    CHECK(E::AssignTemplate(show, cPastor.value(), tNotes.value()).ok());

    // ---- Slides ----
    p::Slide bad1;
    bad1.categoryId = "category-404";
    CHECK(!E::AddSlide(show, bad1).ok());                             // unknown category refused
    p::Slide sl;
    sl.title = "Amazing Grace";
    sl.categoryId = cSongs.value();
    auto s1 = E::AddSlide(show, sl);
    CHECK(s1.ok() && s1.value() == "slide-1");
    sl.title = "Sermon";
    sl.categoryId = cPastor.value();
    auto s2 = E::AddSlide(show, sl);
    sl.title = "Loose";
    sl.categoryId.clear();
    auto s0 = E::AddSlide(show, sl, 0);                               // insert at the front
    CHECK(s0.ok() && show.slides.front().title == "Loose" && show.slides.size() == 3);
    CHECK(E::SetSlideCategory(show, s0.value(), cSongs.value()).ok());
    CHECK(show.slides[0].categoryId == cSongs.value());
    CHECK(!E::SetSlideCategory(show, s0.value(), "category-404").ok());
    CHECK(!E::SetSlideCategory(show, "slide-404", cSongs.value()).ok());
    CHECK(E::SetSlideCategory(show, s0.value(), "").ok());            // back to uncategorised

    // Move (with clamping) and duplicate.
    CHECK(E::MoveSlide(show, s0.value(), 99).ok() && show.slides.back().id == s0.value());
    CHECK(E::MoveSlide(show, s0.value(), 0).ok() && show.slides.front().id == s0.value());
    CHECK(E::MoveCategory(show, cPastor.value(), 0).ok() && show.categories[0].id == cPastor.value());
    CHECK(E::MoveCategory(show, cPastor.value(), 1).ok() && show.categories[1].id == cPastor.value());
    show.slides[1].blocks.push_back(p::ContentBlock{});
    auto dup = E::DuplicateSlide(show, s1.value());
    CHECK(dup.ok() && dup.value() != s1.value());
    CHECK(show.slides[2].id == dup.value());                          // right after the original
    CHECK(show.slides[2].title == "Amazing Grace" && show.slides[2].blocks.size() == 1);
    auto dupT = E::DuplicateTemplate(show, tSong.value());
    CHECK(dupT.ok() && show.templates.back().name == "Song copy" && show.templates.back().blocks.size() == 1);

    // Update keeps identity/position.
    p::Slide replacement;
    replacement.id = "something-else";
    replacement.title = "Retitled";
    CHECK(E::UpdateSlide(show, s1.value(), replacement).ok());
    CHECK(show.slides[1].id == s1.value() && show.slides[1].title == "Retitled");
    p::SlideTemplate retemplate = NamedTemplate("Song v2");
    retemplate.id = "other";
    CHECK(E::UpdateTemplate(show, tSong.value(), retemplate).ok());
    CHECK(show.templates[0].id == tSong.value() && show.templates[0].name == "Song v2");
    CHECK(!E::UpdateTemplate(show, tSong.value(), unnamed).ok());

    // ---- Overlays ----
    p::Overlay ov;
    ov.name = "Logo";
    auto oAll = E::AddOverlay(show, ov);
    CHECK(oAll.ok() && oAll.value() == "overlay-1");
    ov.name = "Songs strip";
    ov.scope = p::OverlayScope::Category;
    ov.targetId = "category-404";
    CHECK(!E::AddOverlay(show, ov).ok());                             // target must exist
    ov.targetId = cSongs.value();
    auto oCat = E::AddOverlay(show, ov);
    CHECK(oCat.ok());
    ov.name = "Badge";
    ov.scope = p::OverlayScope::Slide;
    ov.targetId = s2.value();
    auto oSlide = E::AddOverlay(show, ov);
    CHECK(oSlide.ok());
    CHECK(E::SetOverlayEnabled(show, oAll.value(), false).ok() && !show.overlays[0].enabled);
    p::Overlay moved = show.overlays[2];
    moved.targetId = "slide-404";
    CHECK(!E::UpdateOverlay(show, oSlide.value(), moved).ok());
    moved.targetId = s1.value();
    CHECK(E::UpdateOverlay(show, oSlide.value(), moved).ok() && show.overlays[2].targetId == s1.value());
    CHECK(!E::SetOverlayEnabled(show, "overlay-404", true).ok());
    CHECK(!E::RemoveOverlay(show, "overlay-404").ok());

    // ---- Removal rules ----
    CHECK(E::AssignTemplate(show, cSongs.value(), tSong.value()).ok());
    auto inUse = E::RemoveTemplate(show, tSong.value());              // still used by "Songs"
    CHECK(!inUse.ok() && inUse.error().message.find("Songs") != std::string::npos);
    CHECK(show.templates.size() == 3);
    CHECK(E::RemoveTemplate(show, tSong.value(), /*force=*/true).ok());
    for (const auto& c : show.categories)
        if (c.id == cSongs.value()) CHECK(c.templateId.empty());       // force cleared the reference

    // ---- Content blocks (what a canvas edits) ----
    p::ContentBlock blk;
    blk.kind = "text";
    blk.text = "hello";
    blk.x = 100;
    blk.y = 50;
    auto b1 = E::AddBlock(show, s1.value(), blk);
    CHECK(b1.ok() && b1.value() == "item-1");
    blk.text = "second";
    auto b2 = E::AddBlock(show, s1.value(), blk);
    CHECK(b2.ok() && b2.value() == "item-2");
    p::ContentBlock clashBlock;
    clashBlock.id = "item-1";
    CHECK(!E::AddBlock(show, s1.value(), clashBlock).ok());           // id unique per slide
    CHECK(!E::AddBlock(show, "slide-404", blk).ok());
    p::ContentBlock edited = blk;
    edited.id = "ignored";
    edited.text = "edited";
    edited.width = 999;
    CHECK(E::UpdateBlock(show, s1.value(), b1.value(), edited).ok());
    CHECK(show.slides[1].blocks[0].id == "item-1" && show.slides[1].blocks[0].text == "edited");
    CHECK(!E::UpdateBlock(show, s1.value(), "item-404", edited).ok());
    auto bDup = E::DuplicateBlock(show, s1.value(), b1.value());
    CHECK(bDup.ok() && show.slides[1].blocks[1].id == bDup.value());  // right above the original
    CHECK(show.slides[1].blocks[1].x == 120 && show.slides[1].blocks[1].y == 70);
    CHECK(show.slides[1].blocks[1].text == "edited");
    CHECK(E::MoveBlock(show, s1.value(), b1.value(), 99).ok() && show.slides[1].blocks.back().id == b1.value());
    CHECK(E::RemoveBlock(show, s1.value(), bDup.value()).ok());
    CHECK(!E::RemoveBlock(show, s1.value(), bDup.value()).ok());
    CHECK(show.slides[1].blocks.size() == 2);                         // item-1 and item-2 remain (UpdateSlide replaced the slide earlier)

    // Removing a category: its slides become uncategorised, its overlays go with it.
    const size_t slidesBefore = show.slides.size();
    CHECK(E::RemoveCategory(show, cSongs.value()).ok());
    CHECK(show.slides.size() == slidesBefore);                        // no slide is deleted
    for (const auto& s : show.slides) CHECK(s.categoryId != cSongs.value());
    for (const auto& o : show.overlays) CHECK(!(o.scope == p::OverlayScope::Category && o.targetId == cSongs.value()));
    CHECK(!E::RemoveCategory(show, cSongs.value()).ok());             // already gone

    // Removing a slide removes the overlays scoped to it.
    p::Overlay slideOverlay;
    slideOverlay.name = "S";
    slideOverlay.scope = p::OverlayScope::Slide;
    slideOverlay.targetId = s2.value();
    CHECK(E::AddOverlay(show, slideOverlay).ok());
    CHECK(E::RemoveSlide(show, s2.value()).ok());
    for (const auto& o : show.overlays) CHECK(!(o.scope == p::OverlayScope::Slide && o.targetId == s2.value()));
    CHECK(!E::RemoveSlide(show, s2.value()).ok());

    // After every kind of edit the show still has no dangling reference.
    CHECK(p::SlideResolver::DanglingReferences(show).empty());

    // Ids are not reused while their neighbours exist, and never follow position.
    auto again = E::AddCategory(show, songs);   // songs.templateId was removed with its template
    CHECK(!again.ok());                          // (template-1 is gone — refused, show unchanged)
    songs.templateId.clear();
    auto again2 = E::AddCategory(show, songs);
    CHECK(again2.ok() && again2.value() != cPastor.value());
}

void TestDocumentEdit() {
    p::PresentationDocument doc;
    CHECK(!doc.Edit([](p::Presentation&) { return bps::Ok(); }).ok());   // nothing open

    doc.New("Sunday");
    auto& fs = bps::platform::PlatformAccessor::Get().Filesystem();
    const std::string path = "/tmp/bps_edit_test.vgr";
    (void)fs.Remove(path);
    CHECK(doc.Save(path).ok());
    CHECK(!doc.IsDirty());

    // A successful edit applies and marks the document modified.
    std::string catId;
    CHECK(doc.Edit([&](p::Presentation& m) {
        p::Category c;
        c.name = "Songs";
        auto id = p::ShowEditor::AddCategory(m, c);
        if (!id.ok()) return bps::Result<void>(id.error());
        catId = id.value();
        return bps::Ok();
    }).ok());
    CHECK(doc.IsDirty());
    CHECK(doc.Snapshot().categories.size() == 1 && catId == "category-1");

    CHECK(doc.Save("").ok());
    CHECK(!doc.IsDirty());

    // A refused edit changes nothing — the model AND the clean state stay as they were.
    const std::string before = Snap(doc.Snapshot());
    auto refused = doc.Edit([&](p::Presentation& m) {
        return p::ShowEditor::RemoveCategory(m, "category-404");
    });
    CHECK(!refused.ok());
    CHECK(Snap(doc.Snapshot()) == before);
    CHECK(!doc.IsDirty());

    // A multi-step edit that fails part-way rolls the earlier steps back.
    auto partial = doc.Edit([&](p::Presentation& m) {
        p::Category c;
        c.name = "Added then rolled back";
        auto id = p::ShowEditor::AddCategory(m, c);           // step 1 succeeds…
        if (!id.ok()) return bps::Result<void>(id.error());
        return p::ShowEditor::RemoveTemplate(m, "template-404");   // …step 2 fails
    });
    CHECK(!partial.ok());
    CHECK(doc.Snapshot().categories.size() == 1);             // step 1 was not kept
    CHECK(!doc.IsDirty());

    // The saved file carries the structure.
    p::PresentationDocument reopened;
    CHECK(reopened.Open(path).ok());
    CHECK(reopened.Snapshot().categories.size() == 1 && reopened.Snapshot().categories[0].name == "Songs");
    (void)fs.Remove(path);
}

void TestShowLibraryCrud() {
    auto& fs = bps::platform::PlatformAccessor::Get().Filesystem();
    const std::string root = "/tmp/bps_library_crud";
    (void)fs.RemoveAll(root);
    p::ShowLibrary lib(root);
    CHECK(lib.Refresh().ok());
    CHECK(lib.CreateCategory("Youth").ok());

    auto make = [&](const std::string& category, const std::string& name) {
        auto path = lib.NewShowPath(category, name);
        CHECK(path.ok());
        p::PresentationDocument doc;
        doc.New(name);
        p::Presentation m = doc.Snapshot();
        p::Slide s;
        s.id = "s1";
        m.slides.push_back(s);
        doc.Replace(m);
        CHECK(doc.Save(path.ok() ? path.value() : std::string()).ok());
        return path.ok() ? path.value() : std::string();
    };
    const std::string root1 = make("", "Sunday");
    const std::string youth1 = make("Youth", "Friday");
    CHECK(lib.Refresh().ok() && lib.Shows().size() == 2);

    // Rename: file name AND stored name change; the show is still found.
    auto renamed = lib.RenameShow(root1, "Sunday Morning");
    CHECK(renamed.ok());
    if (renamed.ok()) {
        CHECK(renamed.value().find("Sunday Morning") != std::string::npos);
        CHECK(!fs.Exists(root1) && fs.Exists(renamed.value()));
        p::PresentationDocument d;
        CHECK(d.Open(renamed.value()).ok() && d.Snapshot().name == "Sunday Morning");
    }
    CHECK(lib.Search("morning").size() == 1);
    CHECK(!lib.RenameShow(renamed.ok() ? renamed.value() : std::string(), "   ").ok());   // needs a name
    CHECK(!lib.RenameShow("/tmp/bps_library_crud/none.vgr", "X").ok());
    // Rename into an existing name keeps both (uniquified).
    const std::string other = make("", "Sunday Morning");
    CHECK(other.find("(2)") != std::string::npos);

    // Duplicate: new identity, same category, "<name> copy".
    auto copy = lib.DuplicateShow(youth1);
    CHECK(copy.ok());
    if (copy.ok()) {
        CHECK(copy.value().find("Youth") != std::string::npos);      // stays in its category
        p::PresentationDocument a, b;
        CHECK(a.Open(youth1).ok() && b.Open(copy.value()).ok());
        CHECK(a.Snapshot().id != b.Snapshot().id);
        CHECK(b.Snapshot().name == "Friday copy");
        CHECK(b.Snapshot().slides.size() == 1);
    }
    auto rootCopy = lib.DuplicateShow(renamed.ok() ? renamed.value() : std::string(), "Custom Name");
    CHECK(rootCopy.ok() && rootCopy.value().find("Youth") == std::string::npos);   // root show stays in root

    // Delete is recoverable, and the recycle folder is not a category.
    const size_t beforeDelete = lib.Shows().size();
    auto deleted = lib.DeleteShow(youth1);
    CHECK(deleted.ok());
    CHECK(!fs.Exists(youth1) && deleted.ok() && fs.Exists(deleted.value()));
    CHECK(lib.Shows().size() == beforeDelete - 1);
    for (const auto& c : lib.Categories()) CHECK(c != ".deleted");
    CHECK(!lib.DeleteShow(youth1).ok());                              // already gone
    CHECK(lib.EmptyDeleted().ok());
    CHECK(deleted.ok() && !fs.Exists(deleted.value()));

    (void)fs.RemoveAll(root);
}
