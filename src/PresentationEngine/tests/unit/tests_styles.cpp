// Unit tests: Output styles (Settings · Styles).
// Persistence through the kernel's StyleStore + the style-aware SceneBuilder.
// Split so a single phase can run alone:
//   ./bps_unit_tests styles
#include "TestHarness.hpp"
#include "modules/presentation/LiveOutputController.hpp"
#include "modules/presentation/PresentationEngine.hpp"
#include "modules/presentation/PresentationRuntime.hpp"
#include "modules/presentation/SceneBuilder.hpp"
#include "modules/project/StyleStore.hpp"
#include "modules/rendering/PngCodec.hpp"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <thread>

namespace l = bps::live;

// ---------------------------------------------------------------------------
// StyleStore — the roster document in the kernel's DatabaseManager
// ---------------------------------------------------------------------------
void TestStyleStore() {
    // An empty store reads as an empty roster — not an error (first run).
    CHECK(DatabaseManager::Instance().Open("").ok());   // in-memory for the test
    auto fresh = proj::StyleStore::Instance().Get();
    CHECK(fresh.empty());

    // Round trip: save a roster, read it back field by field.
    std::vector<proj::StoredStyle> roster;
    proj::StoredStyle a;
    a.id = "s1";
    a.name = "Primary";
    a.res = "1920x1080";
    a.contentType = "scripture";
    a.templateKey = "lowerThird";
    a.backgroundColor = "#ff112233";
    a.clearBackgroundOnText = true;
    roster.push_back(a);
    proj::StoredStyle b;
    b.id = "s2";
    b.name = "Stage";
    roster.push_back(b);   // defaults on the remaining fields
    CHECK(proj::StyleStore::Instance().Save(roster).ok());

    auto back = proj::StyleStore::Instance().Get();
    CHECK(back.size() == 2);
    if (back.size() == 2) {
        CHECK(back[0].id == "s1" && back[0].name == "Primary");
        CHECK(back[0].contentType == "scripture" && back[0].templateKey == "lowerThird");
        CHECK(back[0].backgroundColor == "#ff112233" && back[0].clearBackgroundOnText);
        CHECK(back[1].id == "s2" && back[1].backgroundColor == "transparent");
        CHECK(!back[1].clearBackgroundOnText);
    }

    // Overwrite: the second save replaces the first (the roster is one doc).
    std::vector<proj::StoredStyle> smaller;
    proj::StoredStyle c;
    c.id = "s9";
    c.name = "Only";
    smaller.push_back(c);
    CHECK(proj::StyleStore::Instance().Save(smaller).ok());
    auto after = proj::StyleStore::Instance().Get();
    CHECK(after.size() == 1 && after[0].id == "s9");
}

// ---------------------------------------------------------------------------
// StyleBuilder — layout presets + colour parsing
// ---------------------------------------------------------------------------
void TestStyleBuilder() {
    // Colour parsing: #rrggbb, #aarrggbb, transparent, junk, and empty.
    const r::Color opaque = p::StyleBuilder::ParseColor("#336699");
    CHECK(std::abs(opaque.r - 0x33 / 255.0f) < 0.01f);
    CHECK(std::abs(opaque.g - 0x66 / 255.0f) < 0.01f);
    CHECK(std::abs(opaque.b - 0x99 / 255.0f) < 0.01f);
    CHECK(opaque.a > 0.99f);

    const r::Color withAlpha = p::StyleBuilder::ParseColor("#80ff0000");
    CHECK(std::abs(withAlpha.a - 0x80 / 255.0f) < 0.01f);   // alpha channel first
    CHECK(withAlpha.r > 0.99f);

    CHECK(p::StyleBuilder::ParseColor("transparent").a == 0.0f);
    CHECK(p::StyleBuilder::ParseColor("not a colour").a == 0.0f);
    CHECK(p::StyleBuilder::ParseColor("").a == 0.0f);       // "" = "no colour given"
    CHECK(p::StyleBuilder::ParseColor("#12345").a == 0.0f);  // wrong length

    // Layout presets: the lower third hugs the bottom edge and shows no
    // title block; fullscreen covers the stage; an unknown key falls back
    // to the safe default (fullscreen-like).
    const p::StyleBuilder::Layout lower = p::StyleBuilder::LayoutFor("lowerThird");
    CHECK(!lower.showTitle);
    CHECK(lower.body.y + lower.body.h > 0.9f);   // bottom-anchored

    const p::StyleBuilder::Layout full = p::StyleBuilder::LayoutFor("fullscreen");
    CHECK(full.showTitle);
    CHECK(full.title.w > 0.8f && full.body.h > 0.5f);

    const p::StyleBuilder::Layout unknown = p::StyleBuilder::LayoutFor("some-engine-design");
    CHECK(unknown.showTitle && unknown.body.w > 0.8f);
}

// ---------------------------------------------------------------------------
// SceneBuilder — style-aware scene building
// ---------------------------------------------------------------------------
void TestSceneBuilderStyles() {
    p::Presentation pres;
    pres.id = "pres-style";
    p::Slide slide;
    slide.id = "slide-1";
    slide.title = "Welcome";
    slide.text = "Good morning everyone";

    p::SceneBuilder builder;
    r::RenderEngine& engine = r::RenderEngine::Instance();

    const auto checkTextBounds = [&](const std::string& sceneId,
                                     bool expectTitle,
                                     const r::Rect& bodyRect) {
        auto scene = engine.GetScene(sceneId);
        CHECK(scene.ok());
        if (!scene.ok())
            return;
        bool sawTitle = false;
        for (const r::RenderObject* obj : engine.CollectObjects(sceneId)) {
            const auto* text = dynamic_cast<const r::TextObject*>(obj);
            if (!text)
                continue;
            if (text->Id() == "title") {
                sawTitle = true;
            } else if (text->Id() == "body") {
                // Layout lands where the preset says (within a pixel).
                CHECK(std::abs(text->Bounds().x - bodyRect.x) <= 1.0f);
                CHECK(std::abs(text->Bounds().y - bodyRect.y) <= 1.0f);
            }
        }
        CHECK(sawTitle == expectTitle);
    };

    // 1. Unstyled build: engine default background, default layout. Even the
    //    empty spec fingerprints the id ("@s:__0") — one id scheme for every
    //    build, so a style landing later can never collide with an old scene.
    p::OutputStyleSpec none;
    auto plain = builder.BuildSlideScene(pres, slide, none, engine);
    CHECK(plain.ok());
    CHECK(plain.ok() && plain.value() == p::SceneBuilder::StyledSceneIdFor(pres, slide, none));
    CHECK(plain.ok() && plain.value() != p::SceneBuilder::SceneIdFor(pres, slide));
    if (plain.ok()) {
        auto scene = engine.GetScene(plain.value());
        CHECK(scene.ok());
        if (scene.ok()) {
            const auto* bg = engine.GetObject(plain.value(), "bg").ok()
                ? dynamic_cast<const r::BackgroundObject*>(engine.GetObject(plain.value(), "bg").value())
                : nullptr;
            CHECK(bg != nullptr);
            if (bg)
                CHECK(std::abs(bg->BackgroundColor().r - 0.06f) < 0.01f);   // engine default
        }
    }

    // 2. Styled build: the style's background and lowerThird layout apply,
    //    and the scene id carries the style fingerprint.
    p::OutputStyleSpec lowerStyle;
    lowerStyle.name = "Primary";
    lowerStyle.templateKey = "lowerThird";
    lowerStyle.backgroundColor = "#ff204060";
    lowerStyle.clearBackgroundOnText = false;

    auto styled = builder.BuildSlideScene(pres, slide, lowerStyle, engine);
    CHECK(styled.ok());
    CHECK(styled.ok() && styled.value() == p::SceneBuilder::StyledSceneIdFor(pres, slide, lowerStyle));
    CHECK(styled.value() != plain.value());
    if (styled.ok()) {
        const auto* bg = dynamic_cast<const r::BackgroundObject*>(
            engine.GetObject(styled.value(), "bg").value());
        CHECK(bg != nullptr);
        if (bg) {
            CHECK(std::abs(bg->BackgroundColor().r - 0x20 / 255.0f) < 0.01f);
            CHECK(std::abs(bg->BackgroundColor().b - 0x60 / 255.0f) < 0.01f);
        }
        // lowerThird hides the title and parks the body at the bottom.
        const p::StyleBuilder::Layout layout = p::StyleBuilder::LayoutFor("lowerThird");
        r::Rect expectedBody(int(layout.body.x * 1920), int(layout.body.y * 1080),
                             int(layout.body.w * 1920), int(layout.body.h * 1080));
        checkTextBounds(styled.value(), false, expectedBody);
    }

    // 3. clearBackgroundOnText + a slide with its own background: the slide
    //    wins (FreeShow's clearStyleBackgroundOnText). (Distinct slide id —
    //    see test 4.)
    p::Slide owned = slide;
    owned.id = "slide-owned";
    owned.background = "#ff00ff00";
    p::OutputStyleSpec clear = lowerStyle;
    clear.clearBackgroundOnText = true;
    auto slideKeeps = builder.BuildSlideScene(pres, owned, clear, engine);
    CHECK(slideKeeps.ok());
    if (slideKeeps.ok()) {
        const auto* bg = dynamic_cast<const r::BackgroundObject*>(
            engine.GetObject(slideKeeps.value(), "bg").value());
        CHECK(bg != nullptr);
        if (bg)
            CHECK(bg->BackgroundColor().g > 0.99f);   // the slide's green
    }

    // 4. clearBackgroundOnText + a plain slide: the style's background shows.
    //    (Distinct slide id — a copy of `slide` would share test 3's
    //    fingerprinted id and get its cached scene back.)
    p::Slide plain2 = slide;
    plain2.id = "slide-plain2";
    auto styleWins = builder.BuildSlideScene(pres, plain2, clear, engine);
    CHECK(styleWins.ok());
    if (styleWins.ok()) {
        const auto* bg = dynamic_cast<const r::BackgroundObject*>(
            engine.GetObject(styleWins.value(), "bg").value());
        CHECK(bg != nullptr);
        if (bg)
            CHECK(std::abs(bg->BackgroundColor().r - 0x20 / 255.0f) < 0.01f);
    }

    // 5. An EMPTY style background leaves the slide's own (spec carries none).
    p::OutputStyleSpec silent;
    silent.templateKey = "fullscreen";
    p::Slide green;
    green.id = "slide-green";
    green.text = "body only";
    green.background = "#ff101010";
    auto untouched = builder.BuildSlideScene(pres, green, silent, engine);
    CHECK(untouched.ok());
    if (untouched.ok()) {
        const auto* bg = dynamic_cast<const r::BackgroundObject*>(
            engine.GetObject(untouched.value(), "bg").value());
        CHECK(bg != nullptr);
        if (bg)
            CHECK(std::abs(bg->BackgroundColor().r - 0x10 / 255.0f) < 0.01f);
    }

    // 6. Fingerprint: editing a styled field changes the id (so the cached
    //    scene can never be served under a changed style).
    p::OutputStyleSpec edited = lowerStyle;
    edited.backgroundColor = "#ff204061";
    CHECK(p::SceneBuilder::StyledSceneIdFor(pres, slide, edited)
          != p::SceneBuilder::StyledSceneIdFor(pres, slide, lowerStyle));
    p::OutputStyleSpec rekeyed = lowerStyle;
    rekeyed.templateKey = "sidebar";
    CHECK(p::SceneBuilder::StyledSceneIdFor(pres, slide, rekeyed)
          != p::SceneBuilder::StyledSceneIdFor(pres, slide, lowerStyle));
    p::OutputStyleSpec renamed = lowerStyle;
    renamed.name = "Renamed";   // diagnostics only — same fingerprint
    CHECK(p::SceneBuilder::StyledSceneIdFor(pres, slide, renamed)
          == p::SceneBuilder::StyledSceneIdFor(pres, slide, lowerStyle));

    // Cleanup for the next run of the suite in-process.
    (void)engine.DestroyScene(plain.value());
    if (styled.ok()) (void)engine.DestroyScene(styled.value());
    if (slideKeeps.ok()) (void)engine.DestroyScene(slideKeeps.value());
    if (styleWins.ok()) (void)engine.DestroyScene(styleWins.value());
    if (untouched.ok()) (void)engine.DestroyScene(untouched.value());
}

// ---------------------------------------------------------------------------
// PresentationEngine — SetActiveOutputStyle round trip + live-path wiring
// ---------------------------------------------------------------------------
void TestSetActiveOutputStyle() {
    auto& eng = p::PresentationEngine::Instance();
    CHECK(eng.Initialize().ok());

    p::OutputStyleSpec spec;
    spec.name = "Primary";
    spec.templateKey = "lowerThird";
    spec.backgroundColor = "#ff204060";

    // A REAL change bumps the style revision — the live loop's signal to
    // rebuild its scenes under the new spec.
    const uint64_t before = eng.Runtime().StyleRevision();
    CHECK(eng.SetActiveOutputStyle(spec).ok());
    CHECK(eng.Runtime().StyleRevision() == before + 1);
    const p::OutputStyleSpec back = eng.ActiveOutputStyle();
    CHECK(back.name == "Primary" && back.templateKey == "lowerThird");
    CHECK(back.backgroundColor == "#ff204060");

    // An identical push is a no-op: no revision bump, no scene thrash.
    const uint64_t after = eng.Runtime().StyleRevision();
    CHECK(eng.SetActiveOutputStyle(spec).ok());
    CHECK(eng.Runtime().StyleRevision() == after);

    // Clearing (empty spec) round trips too — and is a real change.
    CHECK(eng.SetActiveOutputStyle(p::OutputStyleSpec{}).ok());
    CHECK(eng.Runtime().StyleRevision() == after + 1);
    CHECK(eng.ActiveOutputStyle().name.empty()
          && eng.ActiveOutputStyle().backgroundColor.empty());

    // ---- The live path: compile + prepare under a style, then verify the
    // COMPILED scene id is the fingerprinted one (RenderOnce reads
    // Compiled().slides[idx].sceneId — a plain id here would render a scene
    // that was never built). Then RebuildScenes (what the live loop calls on
    // a style change, legal while LIVE) moves every compiled scene id.
    auto created = eng.CreatePresentation("Style Live");
    CHECK(created.ok());
    const std::string id = created.value();
    p::Slide s;
    s.id = "style-live-1";
    s.title = "Welcome";
    s.text = "Body";
    CHECK(eng.AddSlide(id, s).ok());
    // Full pipeline (the state machine requires Loaded → Validated → Compiled).
    auto issues = eng.Validate(id);
    CHECK(issues.ok() && !p::PresentationValidator::HasErrors(issues.value()));
    CHECK(eng.Compile(id).ok());

    p::OutputStyleSpec live;
    live.name = "Primary";
    live.templateKey = "sidebar";
    live.backgroundColor = "#ff102030";
    CHECK(eng.SetActiveOutputStyle(live).ok());
    CHECK(eng.Prepare(id).ok());   // builds scenes under the pushed spec
    const auto& compiled = eng.Runtime().Compiled();
    CHECK(compiled.slides.size() == 1);
    if (compiled.slides.size() == 1) {
        CHECK(compiled.slides[0].sceneId
              == p::SceneBuilder::StyledSceneIdFor(
                     *eng.Get(id).value(), s, live));
    }

    // A live style change (RebuildScenes — no state transition) re-points the
    // compiled ids at the new fingerprint.
    p::OutputStyleSpec live2;
    live2.name = "Other";
    live2.templateKey = "fullscreen";
    live2.backgroundColor = "#ff000000";
    CHECK(eng.Runtime().RebuildScenes(eng.Builder(),
                                      r::RenderEngine::Instance(), live2).ok());
    CHECK(eng.Runtime().Compiled().slides.size() == 1);
    if (eng.Runtime().Compiled().slides.size() == 1) {
        CHECK(eng.Runtime().Compiled().slides[0].sceneId
              == p::SceneBuilder::StyledSceneIdFor(
                     *eng.Get(id).value(), s, live2));
    }

    CHECK(eng.Delete(id).ok());
    CHECK(eng.SetActiveOutputStyle(p::OutputStyleSpec{}).ok());   // leave unstyled
}

// ---------------------------------------------------------------------------
// Go-live paths — the ORIGINAL BUG: StartFromOpenShow opened the working show
// by a registry id it was never registered under, so goLive failed with
// "presentation not found" and nothing ever rendered.
// ---------------------------------------------------------------------------
void TestGoLivePaths() {
    auto& eng = p::PresentationEngine::Instance();
    CHECK(eng.Initialize().ok());
    // The loop renders REAL frames: the render backend must be live (the
    // rendering phase boots it in its own suites; this phase may run alone).
    // The config default is "null" unless a config file says otherwise —
    // force software for this phase.
    (void)bps::ConfigurationManager::Instance().Set(
        "render.backend", bps::json::Value::String("software"));
    auto& render = r::RenderEngine::Instance();
    CHECK(render.Initialize().ok());
    CHECK(render.Start().ok());
    CHECK(render.SetBackend("software").ok());
    auto& live = l::LiveOutputController::Instance();
    CHECK(live.Initialize().ok());
    CHECK(live.Start().ok());

    // 1. The working-show path THROUGH THE CONTROLLER: the document gets a
    //    show, StartFromOpenShow runs the whole pipeline, frames flow.
    {
        p::Presentation pres;
        pres.id = "pres-golive-doc";
        pres.name = "GoLive Doc";
        p::Slide s;
        s.id = "gl-1";
        s.title = "On Air";
        s.text = "hello live world";
        pres.slides.push_back(s);

        auto doc = eng.Document();
        CHECK(doc != nullptr);
        if (doc) {
            doc->Replace(std::move(pres));
            CHECK(doc->HasDocument());
            CHECK(live.StartFromOpenShow().ok());
            CHECK(live.IsLive());
            CHECK(eng.State() == p::PresentationState::Live);
            CHECK(eng.Runtime().Compiled().slides.size() == 1);
            CHECK(!eng.Runtime().Compiled().slides[0].sceneId.empty());
            // The loop thread renders at 60Hz — give it a moment and require
            // REAL frames (the whole point: something shows on the monitor).
            bool sawFrame = false;
            for (int i = 0; i < 100 && !sawFrame; ++i) {
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
                sawFrame = live.FramesSent() >= 1;
            }
            CHECK(sawFrame);

            // LIVE REPLACE through the controller: new slides, still live.
            p::Presentation second;
            second.id = "pres-golive-doc";
            second.name = "GoLive Doc";
            p::Slide s2;
            s2.id = "gl-2";
            s2.title = "Replaced";
            second.slides.push_back(s2);
            CHECK(live.StartFromSlides("GoLive Doc", second.slides).ok());
            CHECK(live.IsLive());
            CHECK(eng.State() == p::PresentationState::Live);
            CHECK(eng.Runtime().ActivePresentation() != nullptr);
            if (eng.Runtime().ActivePresentation())
                CHECK(eng.Runtime().ActivePresentation()->slides.size() == 1);

            CHECK(live.StopLive().ok());
            CHECK(!live.IsLive());
        }

        // PresentLive direct (the engine-side contract StartFromOpenShow wraps).
        p::Presentation again;
        again.id = "pres-golive-again";
        again.name = "Again";
        again.slides.push_back(s);
        CHECK(eng.PresentLive(again).ok());
        CHECK(eng.State() == p::PresentationState::Live);
        CHECK(eng.Runtime().Compiled().slides.size() == 1);
        CHECK(!eng.Runtime().Compiled().slides[0].sceneId.empty());
        CHECK(eng.StopPlayback().ok());
        CHECK(eng.Runtime().Close().ok());

        // REGISTRY HYGIENE: StartFromOpenShow/StartFromSlides mirror their
        // content into the engine registry (PutPresentation); PresentLive does
        // NOT (the caller owns its copy). Delete what was mirrored — a later
        // phase counts presentations.
        CHECK(eng.Delete("pres-golive-doc").ok());
        // (The live-replace swap does NOT re-put — "pres-golive-again" was
        // never mirrored either, PresentLive leaves the registry alone. The
        // "temp:..." mirror is made by section 3's StartFromSlides below and
        // is deleted there.)
    }

    // 2. Empty content is refused, not crashed on.
    {
        p::Presentation empty;
        empty.id = "pres-empty";
        CHECK(!eng.PresentLive(empty).ok());
    }

    // 3. Controller StartFromSlides: content ownership + empty refusal.
    {
        std::vector<p::Slide> slides;
        p::Slide s;
        s.title = "John 3:16";
        s.text = "For God so loved the world...";
        slides.push_back(s);
        CHECK(live.StartFromSlides("John 3:16", slides).ok());
        CHECK(live.IsLive());
        // The runtime's active presentation IS the temp content now.
        CHECK(eng.Runtime().ActivePresentation() != nullptr);
        CHECK(eng.Runtime().ActivePresentation()->name == "John 3:16");
        CHECK(live.RenderOnce().ok());
        CHECK(live.StopLive().ok());
        CHECK(!live.IsLive());
        // REGISTRY HYGIENE: StartFromSlides mirrored "temp:John 3:16" into the
        // engine registry — delete it here (not before it exists), or the
        // later presentation phase sees a phantom: TestPresentationEngine
        // asserts a fresh-engine presentation count.
        CHECK(eng.Delete("temp:John 3:16").ok());

        std::vector<p::Slide> none;
        CHECK(!live.StartFromSlides("nothing", none).ok());
    }

    CHECK(live.Stop().ok());

    // REGISTRY HYGIENE (render): going live registers the controller's preview
    // feed output with the shared RenderEngine and the design KEEPS it across
    // stop/start cycles. This test booted the render engine for the phase, so
    // it removes the feed again — the rendering phase's TestRenderEngine
    // asserts exact OutputCount() around its own add/remove.
    (void)render.RemoveOutput(l::LiveOutputController::kPreviewName);
}

// ---------------------------------------------------------------------------
// Style background images — fingerprint + render object placement
// ---------------------------------------------------------------------------
void TestStyleBackgroundImage() {
    // A real 3x2 PNG on disk (EncodePngRgba8 writes one; no hand-rolled bytes).
    const std::filesystem::path img = std::filesystem::temp_directory_path() / "bps_style_bg_test.png";
    {
        r::RgbaImage px;
        px.width = 3;
        px.height = 2;
        px.pixels = {0xFF0000FF, 0xFF00FF00, 0xFFFF0000,
                     0xFFFFFFFF, 0xFF808080, 0xFF000000};
        auto encoded = r::EncodePngRgba8(reinterpret_cast<const uint8_t*>(px.pixels.data()),
                                         px.width, px.height);
        CHECK(encoded.ok());
        std::ofstream file(img, std::ios::binary);
        file.write(reinterpret_cast<const char*>(encoded.value().data()),
                   static_cast<std::streamsize>(encoded.value().size()));
        CHECK(file.good());
    }

    p::Presentation pres;
    pres.id = "pres-bgimg";
    p::Slide slide;
    slide.id = "bgimg-1";
    slide.text = "styled image";

    p::SceneBuilder builder;
    r::RenderEngine& engine = r::RenderEngine::Instance();

    // The image path fingerprints the scene id: a new image always rebuilds.
    p::OutputStyleSpec withImage;
    withImage.templateKey = "fullscreen";
    withImage.backgroundColor = "#ff000000";
    withImage.backgroundImage = img.string();
    auto built = builder.BuildSlideScene(pres, slide, withImage, engine);
    CHECK(built.ok());
    CHECK(built.ok() && built.value() == p::SceneBuilder::StyledSceneIdFor(pres, slide, withImage));

    p::OutputStyleSpec otherImage = withImage;
    otherImage.backgroundImage = (std::filesystem::temp_directory_path() / "other.png").string();
    CHECK(p::SceneBuilder::StyledSceneIdFor(pres, slide, otherImage)
          != p::SceneBuilder::StyledSceneIdFor(pres, slide, withImage));
    p::OutputStyleSpec noImage = withImage;
    noImage.backgroundImage.clear();
    CHECK(p::SceneBuilder::StyledSceneIdFor(pres, slide, noImage)
          != p::SceneBuilder::StyledSceneIdFor(pres, slide, withImage));

    // The scene carries the image object, cover-fit over the whole stage.
    if (built.ok()) {
        const r::RenderObject* found = nullptr;
        for (const r::RenderObject* obj : engine.CollectObjects(built.value()))
            if (obj->Id() == "stylebg") found = obj;
        CHECK(found != nullptr);
        if (found) {
            const r::Rect& b = found->Bounds();
            CHECK(b.x <= 0.5f && b.y <= 0.5f);                       // centered
            CHECK(b.x + b.width >= 1919.5f && b.y + b.height >= 1079.5f);   // covers
        }
        (void)engine.DestroyScene(built.value());
    }

    std::error_code ec;
    (void)std::filesystem::remove(img, ec);
}

// ---------------------------------------------------------------------------
// Block-aware scenes — positioned template blocks render as objects
// ---------------------------------------------------------------------------
void TestBlockScenes() {
    p::Presentation pres;
    pres.id = "pres-blocks";
    p::Slide slide;
    slide.id = "blk-1";
    slide.title = "ignored when blocks exist";

    // The scripture template's shape: a translucent box + verse text +
    // reference line (DesignCatalogs geometry, 754x428 stage fractions).
    p::ContentBlock box;
    box.id = "b1";
    box.kind = "box";
    box.x = 30; box.y = 30; box.width = 694; box.height = 368;
    box.style.backgroundColor = "#66000000";
    p::ContentBlock verse;
    verse.id = "b2";
    verse.kind = "text";
    verse.x = 55; verse.y = 45; verse.width = 644; verse.height = 338;
    verse.text = "For God so loved the world";
    verse.metaJson = R"({"color":"#ffffff","fontSize":80,"bold":true,"align":"left","verticalAlign":"center"})";
    p::ContentBlock ref;
    ref.id = "b3";
    ref.kind = "text";
    ref.x = 30; ref.y = 900 / 2.52; ref.width = 694; ref.height = 40;
    ref.text = "John 3:16";
    ref.metaJson = R"({"color":"#cccccc","fontSize":55,"align":"left"})";
    slide.blocks = {box, verse, ref};

    p::SceneBuilder builder;
    r::RenderEngine& engine = r::RenderEngine::Instance();
    p::OutputStyleSpec style;
    style.templateKey = "fullscreen";
    style.backgroundColor = "#ff101020";

    auto built = builder.BuildSlideScene(pres, slide, style, engine);
    CHECK(built.ok());
    if (!built.ok())
        return;

    // Two texts + one box shape, in block order, at block geometry.
    int texts = 0, shapes = 0;
    bool verseFound = false, refFound = false, boxFound = false;
    for (const r::RenderObject* obj : engine.CollectObjects(built.value())) {
        if (const auto* t = dynamic_cast<const r::TextObject*>(obj)) {
            if (t->Id() == "blk1" || t->Id() == "blk2" || t->Id() == "blk3") {
                texts++;
                if (t->Text() == "For God so loved the world") {
                    verseFound = true;
                    // x=55/754 of 1920, y=45/428 of 1080.
                    CHECK(std::abs(t->Bounds().x - 55.0 / 754.0 * 1920.0) < 2.0f);
                    CHECK(std::abs(t->Bounds().y - 45.0 / 428.0 * 1080.0) < 2.0f);
                }
                if (t->Text() == "John 3:16") refFound = true;
            }
        } else if (dynamic_cast<const r::ShapeObject*>(obj) != nullptr) {
            if (obj->Id() == "blk1") { shapes++; boxFound = true; }
        }
    }
    CHECK(texts == 2);
    CHECK(shapes == 1);
    CHECK(verseFound && refFound && boxFound);

    // The slide's TITLE does not join the render when blocks own the slide.
    bool titleLeaked = false;
    for (const r::RenderObject* obj : engine.CollectObjects(built.value()))
        if (const auto* t = dynamic_cast<const r::TextObject*>(obj); t && t->Text() == slide.title)
            titleLeaked = true;
    CHECK(!titleLeaked);

    (void)engine.DestroyScene(built.value());
}

// ---------------------------------------------------------------------------
// Engine-template styles — a style wearing a template design (templateKey
// "tpl-…", blocks baked into the spec) renders THE TEMPLATE with the slide's
// content bound in; the fingerprint hashes the blocks.
// ---------------------------------------------------------------------------
void TestTemplateStyleScenes() {
    p::Presentation pres;
    pres.id = "pres-tplstyle";

    // The plain slide ShapeBuilder's legacy path serves (no blocks of its
    // own): a verse + its reference as title/text.
    p::Slide slide;
    slide.id = "tplstyle-1";
    slide.title = "John 3:16";
    slide.text = "For God so loved the world...";

    // A baked template: a full-stage bound text + a reference chip (the
    // shipped "Scripture" design's shape, condensed).
    p::ContentBlock verse;
    verse.id = "t1";
    verse.kind = "text";
    verse.x = 55; verse.y = 45; verse.width = 644; verse.height = 338;
    verse.bind = "text";
    verse.text = "{scripture_number} {scripture_text}";
    verse.metaJson = R"({"color":"#ffffff","fontSize":80,"align":"left","verticalAlign":"center"})";
    p::ContentBlock chip;
    chip.id = "t2";
    chip.kind = "box";
    chip.x = 30; chip.y = 330; chip.width = 694; chip.height = 68;
    chip.style.backgroundColor = "#ff851b";
    p::ContentBlock ref;
    ref.id = "t3";
    ref.kind = "text";
    ref.x = 40; ref.y = 335; ref.width = 674; ref.height = 58;
    ref.bind = "ref";
    ref.text = "{scripture_reference}";
    ref.metaJson = R"({"color":"#000000","fontSize":40,"align":"left","verticalAlign":"center"})";

    p::SceneBuilder builder;
    r::RenderEngine& engine = r::RenderEngine::Instance();
    p::OutputStyleSpec style;
    style.templateKey = "tpl-scripture";
    style.backgroundColor = "#ff101020";
    style.templateBlocks = {verse, chip, ref};

    // 1. The template renders (scene id = the block-hashed fingerprint), the
    //    slide's content lands in the bound blocks, and the legacy title/body
    //    layout does NOT join.
    auto built = builder.BuildSlideScene(pres, slide, style, engine);
    CHECK(built.ok());
    CHECK(built.ok() && built.value() == p::SceneBuilder::StyledSceneIdFor(pres, slide, style));
    if (built.ok()) {
        bool sawVerse = false, sawRef = false, sawChip = false, sawLegacy = false;
        for (const r::RenderObject* obj : engine.CollectObjects(built.value())) {
            if (const auto* t = dynamic_cast<const r::TextObject*>(obj)) {
                if (t->Id() == "tpl1" && t->Text() == slide.text) sawVerse = true;
                if (t->Id() == "tpl3" && t->Text() == slide.title) sawRef = true;
                if (t->Id() == "title" || t->Id() == "body") sawLegacy = true;
            } else if (obj->Id() == "tpl2") {
                sawChip = true;
            }
        }
        CHECK(sawVerse && sawRef && sawChip);
        CHECK(!sawLegacy);
    }

    // 2. The style's template OWNS the composition: a slide that arrives
    //    with its OWN blocks (the content tab's template already laid them
    //    out for a different design) must NOT paint them over the style's
    //    template — the output style wins (FreeShow: output style > slide).
    p::Slide dressed = slide;
    dressed.id = "tplstyle-dressed";
    dressed.blocks = {chip};   // a stray box from the tab's own template
    auto dressedScene = builder.BuildSlideScene(pres, dressed, style, engine);
    CHECK(dressedScene.ok());
    if (dressedScene.ok()) {
        bool sawStray = false;
        for (const r::RenderObject* obj : engine.CollectObjects(dressedScene.value()))
            if (obj->Id() == "blk1") sawStray = true;   // the slide's own block
        CHECK(!sawStray);
        bool sawTemplateVerse = false;
        for (const r::RenderObject* obj : engine.CollectObjects(dressedScene.value()))
            if (const auto* t = dynamic_cast<const r::TextObject*>(obj); t && t->Id() == "tpl1")
                sawTemplateVerse = t->Text() == dressed.text;
        CHECK(sawTemplateVerse);
    }

    // 3. Leftover scripture placeholders on a PLAIN slide render as blank
    //    lines, not raw "{scripture_name}" text.
    p::OutputStyleSpec placeholderStyle = style;
    p::ContentBlock name;
    name.id = "t4";
    name.kind = "text";
    name.x = 40; name.y = 400; name.width = 674; name.height = 20;
    name.text = "{scripture_name}";   // unbound: a decorative literal in the shipped design
    placeholderStyle.templateBlocks.push_back(name);
    auto withPlaceholder = builder.BuildSlideScene(pres, slide, placeholderStyle, engine);
    CHECK(withPlaceholder.ok());
    if (withPlaceholder.ok()) {
        bool rawPlaceholder = false;
        for (const r::RenderObject* obj : engine.CollectObjects(withPlaceholder.value()))
            if (const auto* t = dynamic_cast<const r::TextObject*>(obj); t && t->Text() == "{scripture_name}")
                rawPlaceholder = true;
        CHECK(!rawPlaceholder);
    }

    // 4. Fingerprint: a block-only edit (moved/retyped text) changes the scene
    //    id — an edited template re-pushes the same key and MUST rebuild, not
    //    serve the cached scene.
    p::OutputStyleSpec edited = style;
    edited.templateBlocks[0].y = 65.0;
    CHECK(p::SceneBuilder::StyledSceneIdFor(pres, slide, edited)
          != p::SceneBuilder::StyledSceneIdFor(pres, slide, style));
    p::OutputStyleSpec renamed = style;
    renamed.name = "Renamed";   // diagnostics only — same fingerprint
    CHECK(p::SceneBuilder::StyledSceneIdFor(pres, slide, renamed)
          == p::SceneBuilder::StyledSceneIdFor(pres, slide, style));

    // 5. Engine-level: pushing a spec whose ONLY delta is templateBlocks is a
    //    real change (revision bumps) — the edited-template relay depends on
    //    it; and the equal-blocks re-push stays a no-op.
    auto& eng = p::PresentationEngine::Instance();
    CHECK(eng.Initialize().ok());
    CHECK(eng.SetActiveOutputStyle(style).ok());
    const uint64_t rev = eng.Runtime().StyleRevision();
    CHECK(eng.SetActiveOutputStyle(style).ok());
    CHECK(eng.Runtime().StyleRevision() == rev);
    CHECK(eng.SetActiveOutputStyle(edited).ok());
    CHECK(eng.Runtime().StyleRevision() == rev + 1);
    CHECK(eng.ActiveOutputStyle().templateBlocks.size() == 3);
    CHECK(eng.SetActiveOutputStyle(p::OutputStyleSpec{}).ok());   // leave unstyled

    (void)engine.DestroyScene(built.value());
}
