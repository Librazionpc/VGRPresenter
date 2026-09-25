// Unit tests: Output styles (Settings · Styles).
// Persistence through the kernel's StyleStore + the style-aware SceneBuilder.
// Split so a single phase can run alone:
//   ./bps_unit_tests styles
#include "TestHarness.hpp"
#include "modules/presentation/SceneBuilder.hpp"
#include "modules/project/StyleStore.hpp"

#include <algorithm>

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
