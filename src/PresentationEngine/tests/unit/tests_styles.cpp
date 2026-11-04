#include "TestHarness.hpp"
#include "modules/presentation/BookFill.hpp"
#include "modules/presentation/CompositorState.hpp"
#include "modules/presentation/LiveOutputController.hpp"
#include "modules/presentation/PresentationEngine.hpp"
#include "modules/presentation/PresentationRuntime.hpp"
#include "modules/presentation/SceneBuilder.hpp"
#include "modules/presentation/SlideRules.hpp"
#include "modules/presentation/TemplateTextMetrics.hpp"
#include "modules/project/OutputStore.hpp"
#include "modules/project/ShippedConfig.hpp"
#include "modules/project/StyleStore.hpp"
#include "modules/rendering/FlowScrollMath.hpp"
#include "modules/rendering/PngCodec.hpp"
#include "modules/settings/Telemetry.hpp"
#include "platform/PlatformAccessor.hpp"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <thread>

#include <cstdint>
#include <cstdio>
#include <vector>

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
// ---------------------------------------------------------------------------
void TestStylePortableImagePaths() {
    auto& platform = bps::platform::PlatformAccessor::Get();
    const std::string userDir = platform.Paths().UserDataDir();
    CHECK(!userDir.empty());
    if (userDir.empty())
        return;   // platform not usable in this environment

    // Probe file in the seeded location (unique name; removed afterwards).
    const std::string name = "vgr-style-heal-probe.png";
    const std::string probe = platform.Filesystem().Join(userDir, name);
    const std::vector<uint8_t> bytes{'p', 'r', 'o', 'b', 'e'};
    CHECK(platform.Filesystem().WriteBinary(probe, bytes).ok());
    CHECK(platform.Filesystem().Exists(probe));

    // 1. A path from another machine re-attaches to the local seeded copy.
    const std::string healed = proj::StyleStore::ReattachBackgroundImage(
        "Z:/from another machine/" + name);
    CHECK(healed == probe);

    // 2. A path that exists on this machine is untouched (the authoring PC).
    CHECK(proj::StyleStore::ReattachBackgroundImage(probe) == probe);

    // 3. No local copy with that name — kept as-is (renders without image).
    const std::string absent = "Z:/nowhere/definitely-absent-heal-probe-7f3d.png";
    CHECK(proj::StyleStore::ReattachBackgroundImage(absent) == absent);

    // 4. Empty stays empty.
    CHECK(proj::StyleStore::ReattachBackgroundImage("").empty());

    (void)platform.Filesystem().Remove(probe);
    CHECK(!platform.Filesystem().Exists(probe));
}

// ---------------------------------------------------------------------------
// OutputStore — the OUTPUT ROSTER document (styles' sibling): the style
// assignment must survive a restart or the boot-time style push is empty.
// ---------------------------------------------------------------------------
void TestOutputStore() {
    CHECK(DatabaseManager::Instance().Open("").ok());   // in-memory for the test
    auto fresh = proj::OutputStore::Instance().Get();
    CHECK(fresh.empty());

    std::vector<proj::StoredOutput> roster;
    proj::StoredOutput a;
    a.id = "out-1";
    a.name = "Main Output";
    a.badge = "LIVE 1";
    a.kind = "HDMI";
    a.res = "1920x1080";
    a.refresh = "60 Hz";
    a.screenName = "\\\\.\\DISPLAY1";
    a.active = true;
    a.styleId = "s1";                     // THE assignment
    a.contentToggles = { true, false, true, true, true, false };
    roster.push_back(a);
    proj::StoredOutput b;
    b.id = "out-2";
    b.name = "Stage";
    roster.push_back(b);   // defaults on the remaining fields
    CHECK(proj::OutputStore::Instance().Save(roster).ok());

    auto back = proj::OutputStore::Instance().Get();
    CHECK(back.size() == 2);
    if (back.size() == 2) {
        CHECK(back[0].id == "out-1" && back[0].name == "Main Output");
        CHECK(back[0].styleId == "s1" && back[0].active);
        CHECK(back[0].screenName == "\\\\.\\DISPLAY1");
        CHECK(!back[0].contentToggles[1] && back[0].contentToggles[2]);
        CHECK(back[1].styleId.empty() && !back[1].active && back[1].enabled);
    }

    // Overwrite: the second save replaces the first (one roster document).
    std::vector<proj::StoredOutput> smaller;
    proj::StoredOutput c;
    c.id = "out-9";
    c.name = "Only";
    smaller.push_back(c);
    CHECK(proj::OutputStore::Instance().Save(smaller).ok());
    CHECK(proj::OutputStore::Instance().Get().size() == 1
          && proj::OutputStore::Instance().Get()[0].id == "out-9");
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
            CHECK(!engine.GetObject(plain.value(), "bg").ok());
            CHECK(scene.value()->TransparentBackground());
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
void TestStyleBackgroundImagePixels() {
    const std::filesystem::path img = std::filesystem::temp_directory_path() / "bps_style_bg_px_test.png";
    {
        r::RgbaImage px;
        px.width = 4;
        px.height = 4;
        px.pixels.assign(16, 0xFF0000FFu);
        auto encoded = r::EncodePngRgba8(reinterpret_cast<const uint8_t*>(px.pixels.data()),
                                         px.width, px.height);
        CHECK(encoded.ok());
        std::ofstream file(img, std::ios::binary);
        file.write(reinterpret_cast<const char*>(encoded.value().data()),
                   static_cast<std::streamsize>(encoded.value().size()));
        CHECK(file.good());
    }

    // The software backend, forced: a phase may run alone and the default is
    // not guaranteed to be a live backend (see TestGoLivePaths above).
    (void)bps::ConfigurationManager::Instance().Set(
        "render.backend", bps::json::Value::String("software"));
    auto& render = r::RenderEngine::Instance();
    CHECK(render.Initialize().ok());
    CHECK(render.Start().ok());
    CHECK(render.SetBackend("software").ok());

    p::Presentation pres;
    pres.id = "pres-bgimg-px";
    p::Slide slide;
    slide.id = "bgimg-px-1";
    slide.text = "pixels";

    p::OutputStyleSpec style;
    style.templateKey = "fullscreen";
    style.backgroundColor = "#ff000000";   // black — the red image is the only colour
    style.backgroundImage = img.string();

    struct SizeCase { int w; int h; };
    const SizeCase cases[] = { { 320, 180 }, { 1920, 1080 } };
    for (const SizeCase& c : cases) {
        p::SceneBuilder builder;
        auto built = builder.BuildSlideScene(pres, slide, style, render, r::Size(c.w, c.h));
        CHECK(built.ok());
        if (!built.ok())
            continue;
        r::RenderOptions opts;
        opts.capture = true;
        opts.distribute = false;   // this is about the pixels, not the outputs
        auto frame = render.Render(built.value(), opts);
        CHECK(frame.ok());
        if (frame.ok()) {
            CHECK(frame.value().width == c.w && frame.value().height == c.h);
            long long nonBlack = 0;
            long long red = 0;
            for (uint32_t px : frame.value().pixels) {
                if ((px & 0x00FFFFFFu) != 0) ++nonBlack;
                // R in the low byte, G/B above: a red pixel has R high, G/B low.
                if ((px & 0xFFu) > 0x80u && ((px >> 8) & 0xFFu) < 0x40u
                    && ((px >> 16) & 0xFFu) < 0x40u)
                    ++red;
            }
            // nothing drawn at all (only the clear) — the app's black frame
            CHECK(nonBlack > 0);
            // the image is cover-fit over the whole stage, so it must own most
            // of the picture at BOTH sizes
            CHECK(red > static_cast<long long>(frame.value().pixels.size()) / 2);
        }
        (void)render.DestroyScene(built.value());
    }

    std::error_code ec;
    (void)std::filesystem::remove(img, ec);
}

// ---------------------------------------------------------------------------
void TestBlankCarrierCompositorPixels() {
    // A real 4x4 solid-red PNG on disk (EncodePngRgba8; no hand-rolled bytes).
    // Colour packing is the engine's own 0xAABBGGRR (R in the LOW byte).
    const std::filesystem::path img = std::filesystem::temp_directory_path() / "bps_blank_carrier_bg.png";
    {
        r::RgbaImage px;
        px.width = 4;
        px.height = 4;
        px.pixels.assign(16, 0xFF0000FFu);
        auto encoded = r::EncodePngRgba8(reinterpret_cast<const uint8_t*>(px.pixels.data()),
                                         px.width, px.height);
        CHECK(encoded.ok());
        std::ofstream file(img, std::ios::binary);
        file.write(reinterpret_cast<const char*>(encoded.value().data()),
                   static_cast<std::streamsize>(encoded.value().size()));
        CHECK(file.good());
    }

    (void)bps::ConfigurationManager::Instance().Set(
        "render.backend", bps::json::Value::String("software"));
    auto& render = r::RenderEngine::Instance();
    CHECK(render.Initialize().ok());
    CHECK(render.Start().ok());
    CHECK(render.SetBackend("software").ok());

    p::Presentation pres;
    pres.id = "temp:";
    p::Slide carrier;
    carrier.id = "temp:-1-blank-carrier";
    carrier.title.clear();
    carrier.text.clear();
    carrier.blocks.clear();
    carrier.background = "transparent";

    p::OutputStyleSpec style;
    style.name = "Blank Carrier";
    style.templateKey = "fullscreen";
    style.backgroundColor = "#ff000000";      // opaque black, as the operator's style
    style.clearBackgroundOnText = true;        // ...which is why content hides the image
    style.backgroundImage = img.string();      // the style's own background image

    // The compositor's media layer (what a taken camera/screen input becomes):
    // 8x8 opaque GREEN, in the byte order the QML hand-off delivers (R,G,B,A).
    auto setMedia = [](bool on) {
        if (!on) {
            p::CompositorState::Instance().ClearMedia();
            return;
        }
        p::CompositorState::MediaFrame frame;
        frame.width = 192;
        frame.height = 108;
        frame.rgba.assign(static_cast<size_t>(frame.width) * frame.height * 4, 0);
        for (size_t i = 0; i < frame.rgba.size(); i += 4) {
            frame.rgba[i + 0] = 0x00;   // R
            frame.rgba[i + 1] = 0xFF;   // G
            frame.rgba[i + 2] = 0x00;   // B
            frame.rgba[i + 3] = 0xFF;   // A
        }
        p::CompositorState::Instance().SetMediaFrame(std::move(frame));
    };

    auto slabTemplate = []() {
        p::ContentBlock slab;
        slab.id = "tpl-slab";
        slab.kind = "box";
        slab.x = 0; slab.y = 0; slab.width = 754; slab.height = 428;
        slab.style.backgroundColor = "#ff000000";
        p::ContentBlock headline;
        headline.id = "tpl-head";
        headline.kind = "text";
        headline.x = 50; headline.y = 88; headline.width = 654; headline.height = 252;
        headline.bind = "text";
        headline.metaJson = R"({"color":"#ffffff","fontSize":90,"align":"center"})";
        return std::vector<p::ContentBlock>{ slab, headline };
    };
    auto bandTemplate = [](std::string meta = "{}") {
        p::ContentBlock band;
        band.id = "tpl-band";
        band.kind = "box";
        band.x = 30; band.y = 330; band.width = 694; band.height = 68;
        band.style.backgroundColor = "#ff851b";
        band.metaJson = std::move(meta);
        p::ContentBlock line;
        line.id = "tpl-line";
        line.kind = "text";
        line.x = 40; line.y = 335; line.width = 674; line.height = 58;
        line.bind = "text";
        line.metaJson = R"({"color":"#000000","fontSize":40,"align":"left"})";
        return std::vector<p::ContentBlock>{ band, line };
    };
    // The same slab, but with the design's own statement about where it belongs.
    auto slabWithMeta = [&slabTemplate](std::string meta) {
        std::vector<p::ContentBlock> blocks = slabTemplate();
        blocks.front().metaJson = std::move(meta);
        return blocks;
    };
    auto panelPairTemplate = [](bool asText = false) {
        auto panel = [asText](const char* id, double y, double h) {
            p::ContentBlock b;
            b.id = id;
            b.kind = asText ? "text" : "box";
            b.x = 0; b.y = y; b.width = 754; b.height = h;
            b.style.backgroundColor = "#ff204060";
            return b;
        };
        p::ContentBlock line;
        line.id = "tpl-pairline";
        line.kind = "text";
        line.x = 50; line.y = 80; line.width = 654; line.height = 220;
        line.bind = "text";
        line.metaJson = R"({"color":"#ffffff","fontSize":80,"align":"center"})";
        return std::vector<p::ContentBlock>{ panel("tpl-panel", 0, 365),
                                             panel("tpl-bar", 365, 63),
                                             line };
    };

    struct Case {
        const char* name;
        bool withContent;
        int templateKind;      // 0 none | 1 slab | 2 band | 3 slab over-feed | 4 band under-feed | 5 panel pair
        bool mediaVisible;
        bool bandOnTop;
        bool bandHidden;
        bool pairHidden;
    };
    const Case cases[] = {
        { "blank carrier", false, 0, true, false, false, false },
        { "blank carrier + slab template", false, 1, true, false, false, false },
        { "carrier + content + slab template", true, 1, true, false, false, false },
        { "carrier + content + panel pair", true, 5, true, false, false, true },
        { "carrier + content + text-item pair", true, 6, true, false, false, true },
        { "carrier + content + band template", true, 2, true, true, false, false },
        { "carrier + content + slab over-feed", true, 3, false, false, false, false },
        { "carrier + content + band under-feed", true, 4, true, false, true, false },
    };
    for (const Case& c : cases) {
        p::Slide slide = carrier;
        p::OutputStyleSpec caseStyle = style;
        if (c.withContent) {
            slide.id = "temp:-1-with-content";
            slide.title = "On Air";
            slide.text = "hello blank carrier";
        }
        if (c.templateKind == 1)
            caseStyle.templateBlocks = slabTemplate();
        else if (c.templateKind == 2)
            caseStyle.templateBlocks = bandTemplate();
        else if (c.templateKind == 3)
            caseStyle.templateBlocks = slabWithMeta("{\"underVideo\":false}");
        else if (c.templateKind == 4)
            caseStyle.templateBlocks = bandTemplate("{\"underVideo\":true}");
        else if (c.templateKind == 5)
            caseStyle.templateBlocks = panelPairTemplate(false);
        else if (c.templateKind == 6)
            caseStyle.templateBlocks = panelPairTemplate(true);

        p::SceneBuilder builder;
        setMedia(true);
        auto built = builder.BuildSlideScene(pres, slide, caseStyle, render,
                                             r::Size(1920, 1080));
        CHECK(built.ok());
        if (!built.ok()) {
            setMedia(false);
            continue;
        }

        r::RenderOptions opts;
        opts.capture = true;
        opts.distribute = false;
        auto frame = render.Render(built.value(), opts);
        CHECK(frame.ok());
        if (frame.ok() && !frame.value().empty()) {
            long long green = 0;
            long long red = 0;
            long long band = 0;   // the orange lower-third band (#ff851b)
            long long pair = 0;   // the dark blue panel pair (#204060)
            for (uint32_t px : frame.value().pixels) {
                const int r8 = static_cast<int>(px & 0xFFu);
                const int g8 = static_cast<int>((px >> 8) & 0xFFu);
                const int b8 = static_cast<int>((px >> 16) & 0xFFu);
                if (g8 > 0x80 && r8 < 0x40 && b8 < 0x40) ++green;
                if (r8 > 0x80 && g8 < 0x40 && b8 < 0x40) ++red;
                if (r8 > 0x70 && g8 > 0x50 && g8 < 0xb0 && b8 < 0x40) ++band;
                if (r8 > 0x18 && r8 < 0x28 && g8 > 0x38 && g8 < 0x48
                    && b8 > 0x58 && b8 < 0x68)
                    ++pair;
            }
            CHECK((green > 0) == c.mediaVisible);
            if ((green > 0) != c.mediaVisible)
                std::cout << "[blank-carrier] " << c.name << ": media "
                          << (green > 0 ? "visible" : "INVISIBLE")
                          << " (green=" << green << ", red=" << red << ")\n";
            if (c.bandOnTop) {
                CHECK(band > 0);
                if (band == 0)
                    std::cout << "[blank-carrier] " << c.name
                              << ": the band went missing under the video layer\n";
            }
            if (c.bandHidden) {
                CHECK(band == 0);
                if (band != 0)
                    std::cout << "[blank-carrier] " << c.name
                              << ": an underVideo band was painted over the feed ("
                              << band << " band pixels)\n";
            }
            // A covering leading run is the design's background WHOLE — none of
            // its panels may end up drawn over the live feed.
            if (c.pairHidden) {
                CHECK(pair == 0);
                if (pair != 0)
                    std::cout << "[blank-carrier] " << c.name
                              << ": a background panel is painted over the feed ("
                              << pair << " panel pixels)\n";
            }
            (void)red;
        }
        if (c.templateKind == 1 && !c.withContent) {   // the geometric slab case
            setMedia(false);
            auto plainScene = builder.BuildSlideScene(pres, slide, caseStyle, render, r::Size(1920, 1080));
            auto plain = plainScene.ok() ? render.Render(plainScene.value(), opts)
                                         : render.Render(built.value(), opts);
            CHECK(plain.ok());
            if (plain.ok() && !plain.value().empty()) {
                long long red2 = 0;
                for (uint32_t px : plain.value().pixels) {
                    const int r8 = static_cast<int>(px & 0xFFu);
                    const int g8 = static_cast<int>((px >> 8) & 0xFFu);
                    const int b8 = static_cast<int>((px >> 16) & 0xFFu);
                    if (r8 > 0x80 && g8 < 0x40 && b8 < 0x40) ++red2;
                }
                CHECK(red2 > 0);
            }
        }
        (void)render.DestroyScene(built.value());
        setMedia(false);
    }

    std::error_code ec;
    (void)std::filesystem::remove(img, ec);
}

// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
void TestGoLivePaths() {
    auto& eng = p::PresentationEngine::Instance();
    CHECK(eng.Initialize().ok());
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

        CHECK(eng.Delete("pres-golive-doc").ok());
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

        {
            p::OutputStyleSpec outStyle;
            outStyle.backgroundColor = "#ff000000";
            const std::string outBuffer = "__out_style_test__";
            CHECK(eng.SetLiveOutputStyles({outStyle}, {outBuffer}, {{1280, 720}}).ok());
            // The loop re-reads the set every frame, but a fully static scene is
            // only rasterized on its heartbeat — poll instead of assuming a slot.
            std::shared_ptr<const r::Frame> frame;
            r::FrameBufferOutput *fb = nullptr;
            for (int i = 0; i < 150 && !fb; ++i) {
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
                auto registered = render.GetOutput(outBuffer);
                if (!registered.ok()) continue;
                auto *candidate = dynamic_cast<r::FrameBufferOutput *>(registered.value().get());
                if (!candidate) continue;
                auto snapshot = candidate->LastFrameSnapshot();
                if (snapshot && !snapshot->empty()) {
                    fb = candidate;
                    frame = std::move(snapshot);
                }
            }
            CHECK(fb != nullptr);
            if (fb) {
                // Registered at THIS output's configured resolution...
                CHECK(fb->TargetSize().width == 1280.0f);
                CHECK(fb->TargetSize().height == 720.0f);
                // ...owned by its own pass (the program pass leaves it alone)...
                CHECK(!fb->AcceptsDistributedFrames());
                CHECK(frame && frame->width == 1280 && frame->height == 720);
                // ...and rasterized as its OWN size-tagged scene, not the
                // 1920x1080 program scene resampled to this size.
                CHECK(frame && frame->sceneId.find("@size:1280x720") != std::string::npos);
            }
            CHECK(eng.SetLiveOutputStyles({}, {}).ok());   // leave the set empty again
            CHECK(render.RemoveOutput(outBuffer).ok());
        }

        {
            const std::string sharedBuf = "__out_shared_test__";
            CHECK(eng.SetLiveOutputStyles({eng.ActiveOutputStyle()}, {sharedBuf}).ok());

            r::FrameBufferOutput *fb = nullptr;
            for (int i = 0; i < 200 && !fb; ++i) {
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
                auto registered = render.GetOutput(sharedBuf);
                if (!registered.ok()) continue;
                auto *candidate = dynamic_cast<r::FrameBufferOutput *>(registered.value().get());
                if (!candidate) continue;
                auto snapshot = candidate->LastFrameSnapshot();
                if (candidate->AcceptsDistributedFrames() && snapshot && !snapshot->empty())
                    fb = candidate;
            }
            CHECK(fb != nullptr);
            // A per-output raster was replaced by the copy (the count only moves
            // on the sharing path — see RenderPerOutputPasses).
            CHECK(live.SharedOutputCopies() >= 1);
            if (fb) {
                auto previewReg = render.GetOutput(l::LiveOutputController::kPreviewName);
                CHECK(previewReg.ok());
                auto *pv = previewReg.ok()
                               ? dynamic_cast<r::FrameBufferOutput *>(previewReg.value().get())
                               : nullptr;
                auto shot = fb->LastFrameSnapshot();
                auto feed = pv ? pv->LastFrameSnapshot() : nullptr;
                CHECK(shot && feed);
                if (shot && feed) {
                    CHECK(shot->width == feed->width);
                    CHECK(shot->height == feed->height);
                    CHECK(shot->pixels == feed->pixels);
                }

                const uint64_t rastersThen = render.Stats().frames;
                const uint64_t representedThen = live.RepresentedFrames();
                const uint64_t receivedThen = fb->FramesReceived();
                bool represented = false;
                for (int i = 0; i < 120 && !represented; ++i) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(20));
                    represented = live.RepresentedFrames() > representedThen
                                  && fb->FramesReceived() > receivedThen;
                }
                CHECK(represented);
                CHECK(render.Stats().frames == rastersThen);   // a copy, not a raster

                bps::settings::LiveSample quiet;
                bool measured = false;
                for (int i = 0; i < 300 && !measured; ++i) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(20));
                    const std::vector<bps::settings::LiveSample> curve =
                        bps::settings::Telemetry::Instance().LiveHistory();
                    if (curve.empty())
                        continue;
                    const bps::settings::LiveSample& last = curve.back();
                    if (last.composedFps == 0.0 && last.presentFps > 0.0 && last.sharedCopies >= 1) {
                        quiet = last;
                        measured = true;
                    }
                }
                const bps::settings::LiveStats totals =
                    bps::settings::Telemetry::Instance().LiveSnapshot();
                std::fprintf(stderr,
                             "[live] static second: composedFps=%.2f presentFps=%.2f "
                             "sharedCopies=%llu | raster(report) frameMs=%.1f ms vs "
                             "copy(report) representMs=%.1f ms | session composed=%llu "
                             "represented=%llu\n",
                             quiet.composedFps, quiet.presentFps,
                             static_cast<unsigned long long>(quiet.sharedCopies),
                             totals.frameMs, totals.representMs,
                             static_cast<unsigned long long>(totals.composed),
                             static_cast<unsigned long long>(totals.represented));
                CHECK(measured);
                CHECK(quiet.composedFps == 0.0);
                CHECK(quiet.presentFps > 0.0);
                CHECK(totals.frameMs > totals.representMs);
                CHECK(quiet.sharedCopies >= 1);

                std::fprintf(stderr,
                             "[live] hotspot: %s — %.0f%% of the frame (%.1f ms), background %.0f%%\n",
                             quiet.costElement.c_str(), quiet.costSharePct, quiet.costMs,
                             quiet.costBackgroundPct);
                CHECK(!quiet.costElement.empty());
                CHECK(quiet.costSharePct > 0.0 && quiet.costSharePct <= 100.0);
                CHECK(quiet.costMs > 0.0);
                CHECK(quiet.costElement.find("Background clear") == std::string::npos);
                CHECK(quiet.costBackgroundPct > 0.0);
            }
            CHECK(eng.SetLiveOutputStyles({}, {}).ok());
            CHECK(render.RemoveOutput(sharedBuf).ok());
        }

        CHECK(live.StopLive().ok());
        CHECK(!live.IsLive());
        CHECK(eng.Delete("temp:John 3:16").ok());

        std::vector<p::Slide> none;
        CHECK(!live.StartFromSlides("nothing", none).ok());
    }

    CHECK(live.Stop().ok());

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
// ---------------------------------------------------------------------------
void TestPlacedImageBlock() {
    r::RenderEngine& engine = r::RenderEngine::Instance();
    CHECK(engine.Initialize().ok());
    CHECK(engine.SetBackend("software").ok());

    const std::filesystem::path img =
        std::filesystem::temp_directory_path() / "bps_placed_media_test.png";
    {
        r::RgbaImage px;
        px.width = 4;
        px.height = 2;
        px.pixels = {0xFF0000FF, 0xFF0000FF, 0xFFFF0000, 0xFFFF0000,
                     0xFF0000FF, 0xFF0000FF, 0xFFFF0000, 0xFFFF0000};
        auto encoded = r::EncodePngRgba8(reinterpret_cast<const uint8_t*>(px.pixels.data()),
                                         px.width, px.height);
        CHECK(encoded.ok());
        std::ofstream file(img, std::ios::binary);
        file.write(reinterpret_cast<const char*>(encoded.value().data()),
                   static_cast<std::streamsize>(encoded.value().size()));
        CHECK(file.good());
    }

    p::Presentation pres;
    pres.id = "pres-placed-media";
    p::Slide slide;
    slide.id = "pm-1";
    p::ContentBlock picture;
    picture.id = "m1";
    picture.kind = "image";
    // 300x150 of the 754x428 stage grid — 2:1, the picture's own shape.
    picture.x = 200; picture.y = 140; picture.width = 300; picture.height = 150;
    picture.metaJson = std::string(R"({"src":")") + img.generic_string() + R"("})";
    slide.blocks = { picture };

    p::SceneBuilder builder;
    p::OutputStyleSpec style;
    style.templateKey = "fullscreen";
    style.backgroundColor = "#ff000000";

    auto built = builder.BuildSlideScene(pres, slide, style, engine);
    CHECK(built.ok());
    if (!built.ok()) {
        std::error_code ec;
        (void)std::filesystem::remove(img, ec);
        return;
    }

    const float bx = 200.0f / 754.0f * 1920.0f;
    const float by = 140.0f / 428.0f * 1080.0f;
    const float bw = 300.0f / 754.0f * 1920.0f;
    const float bh = 150.0f / 428.0f * 1080.0f;

    const r::ImageObject* found = nullptr;
    for (const r::RenderObject* obj : engine.CollectObjects(built.value()))
        if (const auto* image = dynamic_cast<const r::ImageObject*>(obj); image && obj->Id() == "blk1")
            found = image;
    CHECK(found != nullptr);
    if (found) {
        const r::Rect& b = found->Bounds();
        CHECK(b.x >= bx - 1.0f);
        CHECK(b.y >= by - 1.0f);
        CHECK(b.Right() <= bx + bw + 1.0f);
        CHECK(b.Bottom() <= by + bh + 1.0f);
        CHECK(std::abs((b.x + b.width / 2.0f) - (bx + bw / 2.0f)) < 2.0f);
        CHECK(std::abs(b.height - bh) < 2.0f);
        // The file's own pixels came through (the cache the style background
        // uses — one decoder, one answer).
        CHECK(found->Source() != nullptr);
        CHECK(found->Source() && found->Source()->source.image.width == 4);
        CHECK(found->Source() && found->Source()->source.image.height == 2);
    }
    (void)engine.DestroyScene(built.value());

    // An item with NO file draws NOTHING — a guessed box would be a lie about
    // content that is not there.
    p::Slide empty = slide;
    empty.id = "pm-2";
    empty.blocks[0].metaJson = "{}";
    auto bare = builder.BuildSlideScene(pres, empty, style, engine);
    CHECK(bare.ok());
    if (bare.ok()) {
        for (const r::RenderObject* obj : engine.CollectObjects(bare.value()))
            CHECK(obj->Id() != "blk1");
        (void)engine.DestroyScene(bare.value());
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
// ---------------------------------------------------------------------------
void TestBlockTextBox() {
    r::RenderEngine& engine = r::RenderEngine::Instance();
    CHECK(engine.Initialize().ok());
    CHECK(engine.SetBackend("software").ok());

    p::Presentation pres;
    pres.id = "pres-textbox";
    p::Slide slide;
    slide.id = "tb-1";
    slide.title = "47-0412";
    slide.text = "I trust that sometime";

    // The stage-block geometry below is in the 754x428 Edit grid: x 100/754 …
    // 654/754 of 1920, y 100/428 … 328/428 of 1080.
    p::ContentBlock verse;
    verse.id = "v1";
    verse.kind = "text";
    verse.x = 100; verse.y = 100; verse.width = 554; verse.height = 228;
    verse.text = "I trust that sometime";
    verse.style.backgroundColor = "#66000000";   // translucent black, the canvas's own tint
    verse.metaJson = R"({"color":"#f1c40f","fontSize":40,"align":"left","verticalAlign":"center"})";
    slide.blocks = { verse };

    p::SceneBuilder builder;
    p::OutputStyleSpec style;
    style.templateKey = "fullscreen";
    style.backgroundColor = "#ffffff";   // opaque white: a box over it MUST darken it

    auto built = builder.BuildSlideScene(pres, slide, style, engine);
    CHECK(built.ok());
    if (!built.ok())
        return;

    const float bx = 100.0f / 754.0f * 1920.0f;
    const float bw = 554.0f / 754.0f * 1920.0f;
    const float by = 100.0f / 428.0f * 1080.0f;
    const float bh = 228.0f / 428.0f * 1080.0f;

    bool textFound = false, boxFound = false;
    for (const r::RenderObject* obj : engine.CollectObjects(built.value())) {
        if (const auto* t = dynamic_cast<const r::TextObject*>(obj); t && obj->Id() == "blk1") {
            textFound = true;
            CHECK(std::abs(t->Bounds().x - bx) < 2.0f);
        }
        if (const auto* s = dynamic_cast<const r::ShapeObject*>(obj); s && obj->Id() == "blk1box") {
            boxFound = true;
            CHECK(std::abs(obj->Bounds().x - bx) < 2.0f);
            CHECK(std::abs(obj->Bounds().y - by) < 2.0f);
            CHECK(std::abs(obj->Bounds().width - bw) < 2.0f);
            CHECK(std::abs(obj->Bounds().height - bh) < 2.0f);
            CHECK(obj->LayerId() == "text");       // under its text, above the background
            const auto* shp = s->Shape();
            CHECK(shp != nullptr);
            if (shp) {
                CHECK(std::abs(shp->fill.a - 0.4f) < 0.02f);          // #66 of the alpha pair
                CHECK(shp->fill.r < 0.02f && shp->fill.g < 0.02f && shp->fill.b < 0.02f);
            }
        }
    }
    CHECK(textFound);
    CHECK(boxFound);

    r::RenderOptions opts;
    opts.capture = true;
    opts.distribute = false;
    auto img = engine.Render(built.value(), opts);
    CHECK(img.ok());
    if (img.ok() && !img.value().empty()) {
        const r::RgbaImage& f = img.value();
        size_t dimmed = 0, brightInside = 0, brightOutside = 0, outside = 0;
        for (int y = 0; y < f.height; ++y) {
            for (int x = 0; x < f.width; ++x) {
                const r::Color px = r::Color::Unpack(
                    f.pixels[static_cast<size_t>(y) * f.width + x]);
                const bool inside = x > bx + 4 && x < bx + bw - 4 && y > by + 4 && y < by + bh - 4;
                const bool right = static_cast<float>(x) > bx + bw + 8;
                if (inside) {
                    if (px.r < 0.7f && px.g < 0.7f) ++dimmed;
                    else ++brightInside;
                } else if (right) {
                    ++outside;
                    if (px.r > 0.9f && px.g > 0.9f && px.b > 0.9f) ++brightOutside;
                }
            }
        }
        CHECK(dimmed > 10000);                        // the box really painted
        CHECK(brightOutside == outside);              // and it stopped at its own rect
        CHECK(brightInside > 0);                      // the text is still drawn ON the box
    }

    // An OPAQUE text fill (#000000 — the shipped Table/Table copy templates'
    // verse text) paints too: the box must not be gated on alpha < 1.
    p::ContentBlock solid = verse;
    solid.id = "v3";
    solid.style.backgroundColor = "#000000";
    p::Slide solidSlide;
    solidSlide.id = "tb-3";
    solidSlide.text = "solid";
    solidSlide.blocks = { solid };
    auto solidScene = builder.BuildSlideScene(pres, solidSlide, style, engine);
    CHECK(solidScene.ok());
    if (solidScene.ok()) {
        bool opaqueBox = false;
        for (const r::RenderObject* obj : engine.CollectObjects(solidScene.value()))
            if (const auto* s = dynamic_cast<const r::ShapeObject*>(obj);
                s && obj->Id() == "blk1box") {
                const auto* shp = s->Shape();
                opaqueBox = shp != nullptr && shp->fill.a > 0.99f;
            }
        CHECK(opaqueBox);
        (void)engine.DestroyScene(solidScene.value());
    }

    // A text block with NO fill adds no box — the containment TestBlockScenes
    // relies on (two texts + their own box block, exactly one shape object).
    p::ContentBlock bare = verse;
    bare.id = "v2";
    bare.style.backgroundColor = "transparent";
    p::Slide bareSlide;
    bareSlide.id = "tb-2";
    bareSlide.text = "bare";
    bareSlide.blocks = { bare };
    auto bareScene = builder.BuildSlideScene(pres, bareSlide, style, engine);
    CHECK(bareScene.ok());
    if (bareScene.ok()) {
        int shapes = 0;
        for (const r::RenderObject* obj : engine.CollectObjects(bareScene.value()))
            if (dynamic_cast<const r::ShapeObject*>(obj) != nullptr) ++shapes;
        CHECK(shapes == 0);
        (void)engine.DestroyScene(bareScene.value());
    }

    (void)engine.DestroyScene(built.value());
}

// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
void TestBlockCornerRadius() {
    r::RenderEngine& engine = r::RenderEngine::Instance();
    CHECK(engine.Initialize().ok());
    CHECK(engine.SetBackend("software").ok());

    p::Presentation pres;
    pres.id = "pres-radius";
    p::Slide slide;
    slide.id = "cr-1";
    slide.text = "rounded";

    // A shape block and a text block with its own fill, same rounded geometry.
    p::ContentBlock band;
    band.id = "band";
    band.kind = "shape";
    band.x = 100; band.y = 40; band.width = 554; band.height = 140;
    band.style.backgroundColor = "#000000";
    band.style.cornerRadius = 20;              // stage px
    p::ContentBlock boxed;
    boxed.id = "text";
    boxed.kind = "text";
    boxed.x = 100; boxed.y = 220; boxed.width = 554; boxed.height = 140;
    boxed.text = "rounded";
    boxed.style.backgroundColor = "#000000";
    boxed.style.cornerRadius = 20;             // stage px
    boxed.metaJson = R"({"color":"#ffffff","fontSize":30,"align":"left","verticalAlign":"center"})";
    slide.blocks = { band, boxed };

    p::SceneBuilder builder;
    p::OutputStyleSpec style;
    style.templateKey = "fullscreen";
    style.backgroundColor = "#ffffff";         // white: a corner shows as white, a fill as black

    auto built = builder.BuildSlideScene(pres, slide, style, engine);
    CHECK(built.ok());
    if (!built.ok())
        return;

    constexpr float kExpectedRadius = 20.0f / 754.0f * 1920.0f;   // ~50.9 px
    int roundedShapes = 0;
    for (const r::RenderObject* obj : engine.CollectObjects(built.value())) {
        const auto* s = dynamic_cast<const r::ShapeObject*>(obj);
        if (!s) continue;
        // blk1 = the shape block, blk2box = the text block's own box.
        if (obj->Id() != "blk1" && obj->Id() != "blk2box") continue;
        ++roundedShapes;
        const auto* shp = s->Shape();
        CHECK(shp != nullptr);
        if (shp)
            CHECK(std::abs(shp->radius - kExpectedRadius) < 0.5f);
    }
    CHECK(roundedShapes == 2);

    r::RenderOptions opts;
    opts.capture = true;
    opts.distribute = false;
    auto img = engine.Render(built.value(), opts);
    CHECK(img.ok());
    if (!img.ok() || img.value().empty()) {
        (void)engine.DestroyScene(built.value());
        return;
    }
    const r::RgbaImage& f = img.value();
    const auto at = [&f](int x, int y) {
        return r::Color::Unpack(f.pixels[static_cast<size_t>(y) * f.width + x]);
    };
    const auto isWhite = [](const r::Color& c) {
        return c.r > 0.9f && c.g > 0.9f && c.b > 0.9f;
    };
    const auto isBlack = [](const r::Color& c) {
        return c.r < 0.1f && c.g < 0.1f && c.b < 0.1f;
    };
    // Stage -> output for both bands.
    const int bandX = static_cast<int>(100.0f / 754.0f * 1920.0f);
    const int bandY = static_cast<int>(40.0f / 428.0f * 1080.0f);
    const int bandW = static_cast<int>(554.0f / 754.0f * 1920.0f);
    const int bandH = static_cast<int>(140.0f / 428.0f * 1080.0f);
    const int boxY = static_cast<int>(220.0f / 428.0f * 1080.0f);

    // The corner of the bounding rect is CUT by the radius: background shows.
    CHECK(isWhite(at(bandX + 3, bandY + 3)));
    CHECK(isWhite(at(bandX + 3, boxY + 3)));
    // …while the body of the band is the fill, corner or not.
    CHECK(isBlack(at(bandX + bandW / 2, bandY + bandH / 2)));
    CHECK(isBlack(at(bandX + 3, bandY + bandH / 2)));            // mid-height, hard against the left edge
    // The rounded edge itself is ANTI-ALIASED: partial coverage along the arc.
    int aaPixels = 0;
    for (int y = bandY; y < bandY + 60; ++y) {
        for (int x = bandX; x < bandX + 60; ++x) {
            const r::Color c = at(x, y);
            if (c.r > 0.1f && c.r < 0.9f && c.g > 0.1f && c.g < 0.9f)
                ++aaPixels;
        }
    }
    CHECK(aaPixels > 20);

    (void)engine.DestroyScene(built.value());

    // CONTROL: radius 0 keeps the hard square corner (the pre-radius look, and
    // what a template that never set one must keep).
    p::Slide square = slide;
    square.id = "cr-2";
    square.blocks[0].style.cornerRadius = 0;
    auto squareScene = builder.BuildSlideScene(pres, square, style, engine);
    CHECK(squareScene.ok());
    if (squareScene.ok()) {
        auto squareImg = engine.Render(squareScene.value(), opts);
        CHECK(squareImg.ok());
        if (squareImg.ok() && !squareImg.value().empty()) {
            const r::RgbaImage& sf = squareImg.value();
            const r::Color corner = r::Color::Unpack(
                sf.pixels[static_cast<size_t>(bandY + 3) * sf.width + bandX + 3]);
            CHECK(isBlack(corner));
        }
        (void)engine.DestroyScene(squareScene.value());
    }
}

// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
void TestAutoSizeVocabulary() {
    p::Presentation pres;
    pres.id = "pres-autosize";
    r::RenderEngine& engine = r::RenderEngine::Instance();
    p::SceneBuilder builder;
    p::OutputStyleSpec style;
    style.templateKey = "fullscreen";
    style.backgroundColor = "#ff101020";

    constexpr float kSetPx = 60.0f;                 // template's own size (stage units)
    constexpr float kScale = 1080.0f / 428.0f;      // stage -> output (AddStageBlocks')
    const float setOutputPx = kSetPx * kScale;

    // Effective output-pixel font size of the single blk1 text object.
    const auto builtSize = [&](const p::Slide& slide) -> float {
        auto built = builder.BuildSlideScene(pres, slide, style, engine);
        if (!built.ok()) return -1.0f;
        float size = -1.0f;
        for (const r::RenderObject* obj : engine.CollectObjects(built.value())) {
            if (const auto* t = dynamic_cast<const r::TextObject*>(obj); t && t->Id() == "blk1")
                for (const auto& c : t->Components())
                    if (const auto* sc = dynamic_cast<const r::TextStyleComponent*>(c.get()))
                        size = sc->style.size;
        }
        (void)engine.DestroyScene(built.value());
        return size;
    };

    const auto slideWithMode = [&](const char* mode, const char* text) {
        p::Slide slide;
        slide.id = std::string("as-") + mode;
        p::ContentBlock verse;
        verse.id = "b1";
        verse.kind = "text";
        verse.x = 40; verse.y = 40; verse.width = 674; verse.height = 348;
        verse.text = text;
        verse.metaJson = std::format(
            R"({{"color":"#ffffff","fontSize":{:.0f},"align":"center","verticalAlign":"center","autoSize":"{}"}})",
            kSetPx, mode);
        slide.blocks = {verse};
        return slide;
    };

    // A short verse in a tall box: grow fills the box (well past the set
    // size); shrink has nothing to shrink (short text fits) and stays exact.
    const char* growModes[] = {"growToFit", "grow"};
    for (const char* mode : growModes) {
        const float size = builtSize(slideWithMode(mode, "He reigns"));
        CHECK(size > setOutputPx + 1.0f);
    }
    const char* exactModes[] = {"shrinkToFit", "shrink", "none"};
    for (const char* mode : exactModes) {
        const float size = builtSize(slideWithMode(mode, "He reigns"));
        CHECK(std::abs(size - setOutputPx) < 1.0f);
    }

    // Overflow: the same box, a verse far too long for 60px — both shrink
    // spellings come DOWN, none stays exact (and overflows, honestly).
    const char* longVerse =
        "For God so loved the world that he gave his one and only Son, that "
        "whoever believes in him shall not perish but have eternal life. And "
        "the Spirit of God was hovering over the face of the waters, and "
        "darkness was upon the face of the deep.";
    const float shrunk = builtSize(slideWithMode("shrinkToFit", longVerse));
    CHECK(shrunk > 3.0f * kScale && shrunk < setOutputPx - 1.0f);
    const float shrunkLegacy = builtSize(slideWithMode("shrink", longVerse));
    CHECK(std::abs(shrunkLegacy - shrunk) < 0.5f);   // both spellings agree
    const float grownLong = builtSize(slideWithMode("growToFit", longVerse));
    CHECK(grownLong > 3.0f * kScale && grownLong < setOutputPx - 1.0f);
    CHECK(std::abs(grownLong - shrunk) < 0.5f);
    const float fixed = builtSize(slideWithMode("none", longVerse));
    CHECK(fixed > 3.0f * kScale && fixed < setOutputPx - 1.0f);
    CHECK(std::abs(fixed - shrunk) < 0.5f);   // none and shrinkToFit agree now
}

// ---------------------------------------------------------------------------
//   1. lineSpacing tracks the font: SceneBuilder derives it from the font's
//   2. measured height fits where it solved: laying the fitted size out
// ---------------------------------------------------------------------------
void TestFontMetricsFitParity() {
    auto& fonts = r::RenderEngine::Instance().Fonts();

    const char* famA = "Segoe UI";
    const char* famB = "Georgia";
    r::TextStyle st;
    st.fontId = famA;
    st.size = 100.0f;
    st.lineSpacing = 20.0f;
    st.wrap = true;
    st.wrapWidth = 900.0f;

    const auto spacingFor = [&](const char* family) {
        r::FontManager::LineMetrics lm{};
        if (fonts.SystemLineMetrics(family, st.size, false, false, lm))
            return lm.inkHeight * 1.2f - st.size;
        return (1.2f - 1.0f) * st.size;
    };
    if (fonts.HasSystemFont(famA) && fonts.HasSystemFont(famB))
        CHECK(std::abs(spacingFor(famB) - spacingFor(famA)) > st.size * 0.01f);

    const auto fittedSize = [&](const char* family) {
        r::TextStyle s = st;
        s.fontId = family;
        float lo = 3.0f, hi = 400.0f;
        for (int i = 0; i < 12; ++i) {
            const float mid = (lo + hi) / 2.0f;
            r::TextStyle probe = s;
            probe.size = mid;
            probe.lineSpacing = spacingFor(family) * (mid / st.size);
            const auto m = r::TextLayout::Measure("He reigns", probe, fonts);
            if (m.totalHeight <= 500.0f && m.totalWidth <= 900.0f) lo = mid;
            else hi = mid;
        }
        return lo;
    };
    // Width-bound at ~2× the set size (900px box, 9 glyphs): the point is
    // that grow GROWS — well past the 100px set size the old walker returned.
    CHECK(fittedSize(famA) > st.size * 1.5f);
    const float fitted = fittedSize(famA);
    r::TextStyle verify = st;
    verify.size = fitted;
    verify.lineSpacing = spacingFor(famA) * (fitted / st.size);
    const auto m = r::TextLayout::Measure("He reigns", verify, fonts);
    CHECK(m.totalHeight <= 500.0f);
    CHECK(m.totalWidth <= 900.0f);

    const char* verse =
        "It shall be light in the evening time, the path to glory you will "
        "surely find; thru the water way, it is the light today.";
    r::TextStyle vl = st;
    vl.size = fitted;
    vl.lineSpacing = verify.lineSpacing;
    const auto mv = r::TextLayout::Measure(verse, vl, fonts);
    if (mv.lines.size() > 1)
        CHECK(mv.totalHeight < static_cast<float>(mv.lines.size()) * mv.lineStep + 0.01f);

    // Builtin-font layouts keep the legacy totals exactly (n·step) — the
    // fallback path this change promised not to touch.
    r::TextStyle builtin = st;
    builtin.fontId = "builtin";
    const auto mb = r::TextLayout::Measure("He reigns", builtin, fonts);
    CHECK(std::abs(mb.totalHeight - static_cast<float>(mb.lines.size()) * mb.lineStep) < 0.01f);
}

// ---------------------------------------------------------------------------
// 1.2. The value the operator could see was never the value on air. This pins
// the shared rule and its engine integration:
//   1. absent and 0 render the SAME, natural step;
//   2. that step is the font's own ink height — NOT the old hidden 1.2;
//   3. an explicit 2.0 still scales it, so the knob keeps working.
// ---------------------------------------------------------------------------
void TestLineHeightObeysTheTemplate() {
    namespace metrics = bps::presentation::template_text_metrics;
    CHECK(metrics::lineHeightMultiplier(0.0) == 1.0);
    CHECK(metrics::lineHeightMultiplier(-1.0) == 1.0);
    CHECK(metrics::lineHeightMultiplier(1.5) == 1.5);
    CHECK(metrics::lineSpacing(std::numeric_limits<double>::quiet_NaN()) == 0.0);
    CHECK(metrics::linePitch(20.0, 1.5, 4.0) == 34.0);
    CHECK(std::abs(metrics::qmlLineHeight(20.0, 1.5, 4.0) - 1.7) < 0.0001);
    CHECK(metrics::engineLineSpacing(20.0, 24.0, 1.5) == 16.0);
    p::Presentation pres;
    pres.id = "pres-lineheight";
    r::RenderEngine& engine = r::RenderEngine::Instance();
    p::SceneBuilder builder;
    p::OutputStyleSpec style;
    style.templateKey = "fullscreen";
    style.backgroundColor = "#ff101020";

    const auto builtText = [&](const std::string& lineHeightJson) -> r::TextStyle {
        p::Slide slide;
        slide.id = "lh-" + lineHeightJson;
        p::ContentBlock verse;
        verse.id = "b1";
        verse.kind = "text";
        verse.x = 40; verse.y = 40; verse.width = 674; verse.height = 348;
        verse.text = "He reigns";
        verse.metaJson = std::format(
            R"({{"color":"#ffffff","fontSize":40,"align":"center","verticalAlign":"center",)"
            R"("autoSize":"none","fontFamily":"Segoe UI"{}}})", lineHeightJson);
        slide.blocks = {verse};
        auto built = builder.BuildSlideScene(pres, slide, style, engine);
        r::TextStyle out;
        if (!built.ok()) return out;
        for (const r::RenderObject* obj : engine.CollectObjects(built.value()))
            if (const auto* t = dynamic_cast<const r::TextObject*>(obj); t && t->Id() == "blk1")
                for (const auto& c : t->Components())
                    if (const auto* sc = dynamic_cast<const r::TextStyleComponent*>(c.get()))
                        out = sc->style;
        (void)engine.DestroyScene(built.value());
        return out;
    };

    const auto expectedStep = [&](float sizePx, double lh) {
        r::FontManager::LineMetrics lm{};
        if (engine.Fonts().SystemLineMetrics("Segoe UI", sizePx, false, false, lm))
            return lm.inkHeight * static_cast<float>(lh);
        return static_cast<float>(lh) * sizePx;
    };
    const auto stepOf = [](const r::TextStyle& s) { return s.size + s.lineSpacing; };

    const r::TextStyle absent = builtText("");                                    // no lineHeight key
    const r::TextStyle zero = builtText(R"(,"lineHeight":0)");
    const r::TextStyle wide = builtText(R"(,"lineHeight":2)");
    CHECK(absent.size > 1.0f && zero.size > 1.0f && wide.size > 1.0f);

    // 1. "Set to zero" and "never set" are the same layout — not one of them
    //    quietly inflated to a default the panel never shows.
    CHECK(std::abs(stepOf(zero) - stepOf(absent)) < 0.5f);
    // 2. That shared layout is the font's NATURAL step (proportional 1.0), and
    //    it is tighter than the 1.2 this used to hardcode for a missing field.
    CHECK(std::abs(stepOf(absent) - expectedStep(absent.size, 1.0)) < 0.5f);
    CHECK(stepOf(absent) < expectedStep(absent.size, 1.2) - 1.0f);
    // 3. An explicit value still scales the step — the knob is not a constant.
    CHECK(std::abs(stepOf(wide) - expectedStep(wide.size, 2.0)) < 0.5f);

    // 4. And the layout never collapses: a style whose lineSpacing cancels its
    //    size (the zero step a "lineHeight":0 used to derive) still gets one
    //    line's worth of space instead of stacking every line on one baseline.
    r::TextStyle collapsed;
    collapsed.fontId = "Segoe UI";
    collapsed.size = 40.0f;
    collapsed.lineSpacing = -40.0f;
    const auto measured = r::TextLayout::Measure("one\ntwo", collapsed, engine.Fonts());
    CHECK(measured.lines.size() == 2);
    CHECK(measured.lineStep >= 39.9f);
}

// ---------------------------------------------------------------------------
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
    p::Slide dressed = slide;
    dressed.id = "tplstyle-dressed";
    dressed.metaJson = R"({"contentType":"scripture"})";
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

    // 6. A Table slide rendered through a Lower Third family preset uses the
    //    slide body, wraps it, and shrinks long paragraphs to the preset bounds.
    p::Slide tableSlide;
    tableSlide.id = "tplstyle-table-lower-third";
    tableSlide.title = "47-0412 - Faith Is The Substance";
    tableSlide.metaJson = R"({"contentType":"table"})";
    tableSlide.text =
        "My, that was very fine, Brother Bosworth. We certainly appreciated that. "
        "And I wonder what you think of it. I am sure we will enjoy hearing more.";
    p::OutputStyleSpec tableLowerThird;
    tableLowerThird.name = "Lower Third";
    tableLowerThird.familyTemplateKeys[p::SlideRules::FamilyTemplateSlot("table")] = "lowerThird";
    const auto tableScene = builder.BuildSlideScene(pres, tableSlide, tableLowerThird, engine);
    CHECK(tableScene.ok());
    if (tableScene.ok()) {
        const r::TextObject* body = nullptr;
        for (const r::RenderObject* object : engine.CollectObjects(tableScene.value()))
            if (const auto* text = dynamic_cast<const r::TextObject*>(object);
                text && text->Text() == tableSlide.text)
                body = text;
        CHECK(body != nullptr);
        if (body) {
            const auto* bodyStyle = body->FindComponent<r::TextStyleComponent>();
            CHECK(bodyStyle != nullptr);
            if (bodyStyle) {
                CHECK(bodyStyle->style.wrap);
                CHECK(bodyStyle->style.fontId == "Cambria");
                CHECK(bodyStyle->style.size > 24.0f);
                const auto measured = r::TextLayout::Measure(
                    body->Text(), bodyStyle->style, engine.Fonts());
                CHECK(measured.lines.size() > 1);
                CHECK(measured.totalWidth <= body->Bounds().width + 0.1f);
                CHECK(measured.totalHeight <= body->Bounds().height + 0.1f);
            }
        }
        (void)engine.DestroyScene(tableScene.value());
    }

    p::Slide tableFlow = tableSlide;
    tableFlow.id = "tplstyle-table-family-flow";
    tableFlow.flowText = tableFlow.text;
    p::ContentBlock tableBody;
    tableBody.id = "table-body";
    tableBody.kind = "text";
    tableBody.x = 0;
    tableBody.y = 303.6428572;
    tableBody.width = 754;
    tableBody.height = 124.3571428;
    tableBody.text = "{table_text}";
    tableBody.bind = "text";
    tableBody.metaJson =
        R"({"autoSize":"growToFit","fontFamily":"Segoe UI",)"
        R"("align":"justify","verticalAlign":"bottom",)"
        R"("lineHeight":1.5,"lineSpacing":6})";
    tableBody.style.padding = 12.0;
    p::OutputStyleSpec tableFamilyStyle;
    tableFamilyStyle.name = "Lower Third";
    const std::size_t tableSlot = p::SlideRules::FamilyTemplateSlot("table");
    tableFamilyStyle.templateKey = "lowerThird";
    tableFamilyStyle.familyTemplateKeys[tableSlot] = "tpl-3";
    tableFamilyStyle.familyTemplateBlocks[tableSlot] = {tableBody};
    const auto tableFlowScene =
        builder.BuildSlideScene(pres, tableFlow, tableFamilyStyle, engine, r::Size(1920, 1080));
    CHECK(tableFlowScene.ok());
    float longFlowSize = 0.0f;
    if (tableFlowScene.ok()) {
        const r::TextObject* body = nullptr;
        for (const r::RenderObject* object : engine.CollectObjects(tableFlowScene.value()))
            if (const auto* text = dynamic_cast<const r::TextObject*>(object);
                text && text->Text() == tableFlow.text)
                body = text;
        CHECK(body != nullptr);
        if (body) {
            const auto* bodyStyle = body->FindComponent<r::TextStyleComponent>();
            CHECK(bodyStyle != nullptr);
            if (bodyStyle) {
                longFlowSize = bodyStyle->style.size;
                CHECK(bodyStyle->style.size > 16.0f * 1080.0f / 428.0f + 1.0f);
                CHECK(bodyStyle->style.align == r::TextAlign::Justify);
                CHECK(bodyStyle->style.valign == r::TextVAlign::Bottom);
                CHECK(bodyStyle->style.lineSpacing > 0.0f);
                CHECK(std::abs(bodyStyle->style.lineGap - 6.0f * 1080.0f / 428.0f) < 0.1f);
                const float expectedTextWidth =
                    static_cast<float>((tableBody.width - 2.0 * tableBody.style.padding)
                                       * 1920.0 / 754.0);
                CHECK(std::abs(body->Bounds().width - expectedTextWidth) < 0.1f);
                const float expectedTextY =
                    static_cast<float>((tableBody.y + tableBody.style.padding)
                                       * 1080.0 / 428.0);
                CHECK(std::abs(body->Bounds().y - expectedTextY) < 0.1f);
                const auto measured = r::TextLayout::Measure(
                    body->Text(), bodyStyle->style, engine.Fonts());
                CHECK(measured.totalWidth <= body->Bounds().width + 0.1f);
                CHECK(measured.totalHeight <= body->Bounds().height + 0.1f);
            }
        }
        (void)engine.DestroyScene(tableFlowScene.value());
    }
    p::Slide shortTableFlow = tableFlow;
    shortTableFlow.id = "tplstyle-table-family-short-flow";
    shortTableFlow.text = "A short paragraph.";
    shortTableFlow.flowText = shortTableFlow.text;
    tableBody.text = "{table_text}";
    tableFamilyStyle.familyTemplateBlocks[tableSlot] = {tableBody};
    const auto shortTableFlowScene =
        builder.BuildSlideScene(pres, shortTableFlow, tableFamilyStyle, engine,
                                r::Size(1920, 1080));
    CHECK(shortTableFlowScene.ok());
    if (shortTableFlowScene.ok()) {
        const r::TextObject* body = nullptr;
        for (const r::RenderObject* object : engine.CollectObjects(shortTableFlowScene.value()))
            if (const auto* text = dynamic_cast<const r::TextObject*>(object);
                text && text->Text() == shortTableFlow.text)
                body = text;
        CHECK(body != nullptr);
        if (body) {
            const auto* bodyStyle = body->FindComponent<r::TextStyleComponent>();
            CHECK(bodyStyle != nullptr);
            if (bodyStyle) {
                CHECK(bodyStyle->style.size > longFlowSize);
                const auto measured = r::TextLayout::Measure(
                    body->Text(), bodyStyle->style, engine.Fonts());
                CHECK(measured.totalWidth <= body->Bounds().width + 0.1f);
                CHECK(measured.totalHeight <= body->Bounds().height + 0.1f);
            }
        }
        (void)engine.DestroyScene(shortTableFlowScene.value());
    }

    // 8. RESOLUTION: an output's scene is built AT that output's own size. The
    CHECK(p::SceneBuilder::StyledSceneIdFor(pres, slide, style)
          == p::SceneBuilder::StyledSceneIdFor(pres, slide, style, r::Size(1920, 1080)));
    const std::string idBig = p::SceneBuilder::StyledSceneIdFor(pres, slide, style, r::Size(3840, 2160));
    CHECK(idBig != p::SceneBuilder::StyledSceneIdFor(pres, slide, style));
    CHECK(idBig.find("@size:3840x2160") != std::string::npos);
    auto big = builder.BuildSlideScene(pres, slide, style, engine, r::Size(3840, 2160));
    CHECK(big.ok());
    CHECK(big.ok() && big.value() != built.value());   // its own scene, not the 1080p one
    if (big.ok()) {
        auto bigScene = engine.GetScene(big.value());
        CHECK(bigScene.ok());
        if (bigScene.ok()) {
            CHECK(bigScene.value()->SceneSize().width == 3840.0f);
            CHECK(bigScene.value()->SceneSize().height == 2160.0f);
        }
        // The 1080p scene the earlier sections built still reports ITS size:
        // the size tag kept them apart instead of re-keying one into the other.
        auto defScene = engine.GetScene(built.value());
        CHECK(defScene.ok());
        if (defScene.ok())
            CHECK(defScene.value()->SceneSize().height == 1080.0f);
        (void)engine.DestroyScene(big.value());
    }

    (void)engine.DestroyScene(built.value());
}

// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
void TestTransparentBackground() {
    r::RenderEngine &engine = r::RenderEngine::Instance();
    CHECK(engine.SetBackend("software").ok());

    p::Presentation pres;
    pres.id = "pres-transparent";
    p::Slide slide;
    slide.id = "tr-1";
    slide.title = "On Air";
    slide.text = "hello transparent world";

    p::SceneBuilder builder;
    auto rasterAlpha = [&engine](const std::string &sceneId, uint8_t atCorner) {
        r::RenderOptions opts;
        opts.distribute = false;
        opts.capture = true;
        auto img = engine.Render(sceneId, opts);
        CHECK(img.ok());
        if (!img.ok() || img.value().empty())
            return std::pair<uint8_t, bool>{ 0, false };
        // 1x1 top-left corner: the area a full-frame background would cover and
        // the legacy text layout never reaches.
        const uint8_t corner = static_cast<uint8_t>((img.value().pixels.front() >> 24) & 0xFF);
        bool sawOpaque = false;
        for (const uint32_t px : img.value().pixels)
            if (((px >> 24) & 0xFF) == 255) { sawOpaque = true; break; }
        (void)atCorner;
        return std::pair<uint8_t, bool>{ corner, sawOpaque };
    };

    // 1. EXPLICIT "transparent": no background object, scene flagged, corner
    //    fully transparent — and the CONTENT is still there (some pixel opaque),
    //    so this is a keyable overlay, not an empty frame.
    {
        p::OutputStyleSpec clearStyle;
        clearStyle.backgroundColor = "transparent";
        auto clearScene = builder.BuildSlideScene(pres, slide, clearStyle, engine);
        CHECK(clearScene.ok());
        if (clearScene.ok()) {
            auto scene = engine.GetScene(clearScene.value());
            CHECK(scene.ok());
            if (scene.ok())
                CHECK(scene.value()->TransparentBackground());
            bool sawBg = false;
            for (const r::RenderObject *obj : engine.CollectObjects(clearScene.value()))
                if (obj->Id() == "bg") sawBg = true;
            CHECK(!sawBg);
            const auto raster = rasterAlpha(clearScene.value(), 0);
            CHECK(raster.first == 0);      // alpha 0 at the corner: transparent
            CHECK(raster.second);          // text still rasterized
            (void)engine.DestroyScene(clearScene.value());
        }
    }

    // 2. UNSET background ("" — the style carries no colour): NO background
    {
        p::OutputStyleSpec unsetStyle;
        auto unsetScene = builder.BuildSlideScene(pres, slide, unsetStyle, engine);
        CHECK(unsetScene.ok());
        if (unsetScene.ok()) {
            auto scene = engine.GetScene(unsetScene.value());
            CHECK(scene.ok());
            if (scene.ok())
                CHECK(scene.value()->TransparentBackground());
            bool sawBg = false;
            for (const r::RenderObject *obj : engine.CollectObjects(unsetScene.value()))
                if (obj->Id() == "bg") sawBg = true;
            CHECK(!sawBg);
            const auto raster = rasterAlpha(unsetScene.value(), 0);
            CHECK(raster.first == 0);      // alpha 0 at the corner: transparent
            CHECK(raster.second);          // the slide's text still rasterized
            (void)engine.DestroyScene(unsetScene.value());
        }
    }

    // 3. A solid colour is opaque in both senses (a background is painted and
    //    the scene is not flagged).
    {
        p::OutputStyleSpec solidStyle;
        solidStyle.backgroundColor = "#ff0000";
        auto solidScene = builder.BuildSlideScene(pres, slide, solidStyle, engine);
        CHECK(solidScene.ok());
        if (solidScene.ok()) {
            auto scene = engine.GetScene(solidScene.value());
            if (scene.ok())
                CHECK(!scene.value()->TransparentBackground());
            const auto raster = rasterAlpha(solidScene.value(), 255);
            CHECK(raster.first == 255);
            (void)engine.DestroyScene(solidScene.value());
        }
    }

    // 4. The SLIDE'S OWN OPAQUE BACKGROUND MUST NOT DEFEAT AN EXPLICITLY
    {
        p::Slide owned = slide;
        owned.id = "tr-owned";
        owned.background = "#000000";   // what SlideBuilder bakes from tpl-table
        p::OutputStyleSpec clearStyle;
        clearStyle.backgroundColor = "transparent";
        auto ownedScene = builder.BuildSlideScene(pres, owned, clearStyle, engine);
        CHECK(ownedScene.ok());
        if (ownedScene.ok()) {
            auto scene = engine.GetScene(ownedScene.value());
            CHECK(scene.ok());
            if (scene.ok())
                CHECK(scene.value()->TransparentBackground());
            bool sawBg = false;
            for (const r::RenderObject *obj : engine.CollectObjects(ownedScene.value()))
                if (obj->Id() == "bg") sawBg = true;
            CHECK(!sawBg);
            const auto raster = rasterAlpha(ownedScene.value(), 0);
            CHECK(raster.first == 0);   // the template-baked black did not win
            (void)engine.DestroyScene(ownedScene.value());
        }
    }

    // 5. An UNSET style with the same slide-owned background still keeps it —
    //    the reordering must not collaterally make an unset style transparent.
    {
        p::Slide owned = slide;
        owned.id = "tr-owned-unset";
        owned.background = "#000000";
        p::OutputStyleSpec unsetStyle;
        auto keepScene = builder.BuildSlideScene(pres, owned, unsetStyle, engine);
        CHECK(keepScene.ok());
        if (keepScene.ok()) {
            auto scene = engine.GetScene(keepScene.value());
            if (scene.ok())
                CHECK(!scene.value()->TransparentBackground());
            const auto raster = rasterAlpha(keepScene.value(), 255);
            CHECK(raster.first == 255);   // the slide's own colour still applies
            (void)engine.DestroyScene(keepScene.value());
        }
    }

    // 6. ALPHA INSPECTION (RenderEngine::SetAlphaInspect — the engine half of
    {
        p::OutputStyleSpec clearStyle;
        clearStyle.backgroundColor = "transparent";
        auto scene = builder.BuildSlideScene(pres, slide, clearStyle, engine);
        CHECK(scene.ok());
        if (scene.ok()) {
            auto cornerPixel = [&engine](const std::string &id) {
                r::RenderOptions opts;
                opts.distribute = false;   // no outputs to feed in a test
                opts.capture = true;       // we want the frame back
                auto img = engine.Render(id, opts);
                return (img.ok() && !img.value().empty()) ? img.value().pixels.front() : 0u;
            };

            engine.SetAlphaInspect(false);
            CHECK((cornerPixel(scene.value()) >> 24) == 0u);    // keyable: alpha 0

            engine.SetAlphaInspect(true);
            const uint32_t inspected = cornerPixel(scene.value());
            CHECK(((inspected >> 24) & 0xFFu) == 255u);         // no longer transparent
            CHECK(inspected == 0xFFCCCCCCu);                    // the light checker tile
            engine.SetAlphaInspect(false);
            CHECK((cornerPixel(scene.value()) >> 24) == 0u);    // restored
            (void)engine.DestroyScene(scene.value());
        }

        // Opaque content is untouched by inspection (a == 255 composites to
        // itself), so the diagnostic cannot silently restyle a normal frame.
        p::OutputStyleSpec solidStyle;
        solidStyle.backgroundColor = "#ff0000";
        auto solidScene = builder.BuildSlideScene(pres, slide, solidStyle, engine);
        CHECK(solidScene.ok());
        if (solidScene.ok()) {
            engine.SetAlphaInspect(true);
            r::RenderOptions opts;
            opts.distribute = false;
            opts.capture = true;
            auto img = engine.Render(solidScene.value(), opts);
            CHECK(img.ok());
            if (img.ok() && !img.value().empty())
                CHECK(img.value().pixels.front() == 0xFF0000FFu);   // opaque red, unchanged
            engine.SetAlphaInspect(false);
            (void)engine.DestroyScene(solidScene.value());
        }
    }
}

void TestFrameRule() {
    p::ContentBlock text;
    text.kind = "text";
    text.text = "verse";
    p::ContentBlock camera;
    camera.kind = "camera";
    p::ContentBlock media;
    media.kind = "media";
    p::ContentBlock audio;
    audio.kind = "audio";
    p::ContentBlock image;
    image.kind = "image";

    // Nothing to stand in for: an empty block list, and the kinds a block
    // renderer draws for real.
    CHECK(!p::SlideRules::NeedsDistributedFrame({}));
    CHECK(!p::SlideRules::NeedsDistributedFrame({ text }));
    p::ContentBlock box;
    box.kind = "box";
    p::ContentBlock clock;
    clock.kind = "clock";
    p::ContentBlock timer;
    timer.kind = "timer";
    CHECK(!p::SlideRules::NeedsDistributedFrame({ box, clock, timer, text }));

    // ONE media-kind block is enough — the slide carries a picture the blocks
    // can only stand in for, so the frame wins for the whole slide.
    CHECK(p::SlideRules::NeedsDistributedFrame({ camera }));
    CHECK(p::SlideRules::NeedsDistributedFrame({ media }));
    CHECK(p::SlideRules::NeedsDistributedFrame({ audio }));
    CHECK(p::SlideRules::NeedsDistributedFrame({ image }));
    CHECK(p::SlideRules::NeedsDistributedFrame({ text, camera }));

    p::Slide slide;
    slide.id = "frame-rule-1";
    slide.blocks = { text, camera };
    p::OutputStyleSpec unset;   // no template key, no family picks
    const auto own = p::SceneBuilder::ComposeSlide(slide, unset, /*applyGates=*/true);
    CHECK(own.source == p::SceneBuilder::SlideComposition::Source::SlideBlocks);
    CHECK(p::SlideRules::NeedsDistributedFrame(slide.blocks));

    p::ContentBlock bound = text;
    bound.bind = "text";
    bound.text = "a line the template paints";
    p::Slide tagged = slide;
    tagged.metaJson = R"({"contentType":"scripture"})";
    p::OutputStyleSpec templated;
    templated.familyTemplateBlocks[p::SlideRules::FamilyTemplateSlot("scripture")] = { bound };
    const auto owned = p::SceneBuilder::ComposeSlide(tagged, templated, /*applyGates=*/true);
    CHECK(owned.source == p::SceneBuilder::SlideComposition::Source::FamilyTemplate);
    CHECK(!p::SlideRules::NeedsDistributedFrame(owned.blocks));
}

// ---------------------------------------------------------------------------
// Placeholder binding — an unbound `{token}` block must render the SAME
// sentence the preview fills, not an empty box
// ---------------------------------------------------------------------------
void TestPlaceholderBinding() {
    p::Slide slide;
    slide.id = "tbl-1";
    slide.text = "3 I trust that sometime when I come back again to Oakland";
    // Deliberately NOT equal to the ref: the reference must come from the meta the
    // services write (ShowConverter's `ref`), not from whatever else the title holds.
    slide.title = "Faith Is The Substance 3";
    slide.metaJson = R"({"ref":"47-0412 - Faith Is The Substance 3","contentType":"table"})";

    auto slot = [](const char* id, const char* text, double y) {
        p::ContentBlock b;
        b.id = id;
        b.kind = "text";
        b.x = 30; b.y = y; b.width = 694; b.height = 60;
        b.text = text;
        return b;
    };
    // The three slots of the reported design, as the palette leaves them.
    p::OutputStyleSpec spec;
    spec.familyTemplateBlocks[p::SlideRules::FamilyTemplateSlot("table")] = {
        slot("t1", "{table_number} {table_text}", 45),
        slot("t2", "{table_reference}", 905),
        slot("t3", "{table_code_paragraphs}", 970),
    };

    const auto comp = p::SceneBuilder::ComposeSlide(slide, spec, /*applyGates=*/true);
    CHECK(comp.source == p::SceneBuilder::SlideComposition::Source::FamilyTemplate);
    CHECK(comp.blocks.size() == 3);
    if (comp.blocks.size() == 3) {
        CHECK(comp.blocks[0].text == slide.text);          // body: the number slot swallowed, the text in
        CHECK(comp.blocks[1].text == std::string("47-0412 - Faith Is The Substance 3"));   // from the meta ref
        CHECK(comp.blocks[1].text.find('{') == std::string::npos);   // never a raw token on air
        CHECK(comp.blocks[2].text.empty());                // nothing to guess → blank, not literal
    }

    // ONE VOCABULARY, TWO CALLERS: the helper the scene builder calls is the fill
    // the preview uses, so the same values must produce the same sentence.
    p::BookValues v;
    v.text = slide.text;
    v.reference = "47-0412 - Faith Is The Substance 1-3";
    v.code = "47-0412";
    v.numbers = "1-3";
    CHECK(p::FillTokens("A {table_code_paragraphs} B {table_reference}", v)
          == "A 47-0412 1-3 B 47-0412 - Faith Is The Substance 1-3");
    // Recognition is by NAME, so an unknown placeholder is not ours to resolve —
    // and the scene builder keeps its old rule for those (blank).
    CHECK(p::HasTokens("{table_text}"));
    CHECK(p::HasTokens("x {scripture_reference} y"));
    CHECK(!p::HasTokens("plain {not_a_token}"));
    CHECK(p::FillTokens("plain {not_a_token}", v) == "plain {not_a_token}");
}

// ---------------------------------------------------------------------------
// The body of a slide that carries SEVERAL slots is not the first one
// ---------------------------------------------------------------------------
void TestPlaceholderBodyNotFirstSlot() {
    const std::string body = "3 I trust that sometime when I come back again to Oakland";

    auto filled = [](const char* id, const char* text, double y, const char* bind) {
        p::ContentBlock b;
        b.id = id;
        b.kind = "text";
        b.x = 30; b.y = y; b.width = 694; b.height = 60;
        b.text = text;              // ALREADY FILLED: this is what BookFill hands over
        b.bind = bind;
        return b;
    };

    p::Slide slide;
    slide.id = "tbl-slot-order";
    // No `text`, exactly as the table path delivers it — the body is in the blocks.
    slide.title = "47-0412 - Faith Is The Substance 3";
    slide.metaJson = R"({"ref":"47-0412 - Faith Is The Substance 3","contentType":"table"})";
    slide.blocks = {
        filled("c1", "47-0412 4", 970, ""),        // the code in the corner: FIRST in the design
        filled("b1", body.c_str(), 45, "text"),      // the body slot, as the palette declares it
        filled("r1", "47-0412 - Faith Is The Substance 3", 905, "ref"),
    };

    // 1. What the slide's body IS. The code must not win by being first.
    CHECK(p::SlideRules::ContentText(slide) == body);

    // 2. What a style's own template renders for the body, by bind and by token.
    auto slot = [](const char* id, const char* text, double y) {
        p::ContentBlock b;
        b.id = id;
        b.kind = "text";
        b.x = 30; b.y = y; b.width = 694; b.height = 60;
        b.text = text;
        return b;
    };
    p::ContentBlock boundBody = slot("s1", "{table_text}", 45);
    boundBody.bind = "text";
    p::OutputStyleSpec spec;
    spec.familyTemplateBlocks[p::SlideRules::FamilyTemplateSlot("table")] = {
        boundBody,
        slot("s2", "{table_text}", 400),        // the same body asked by TOKEN
        slot("s3", "{table_code_paragraphs}", 970),
    };

    const auto comp = p::SceneBuilder::ComposeSlide(slide, spec, /*applyGates=*/true);
    CHECK(comp.source == p::SceneBuilder::SlideComposition::Source::FamilyTemplate);
    CHECK(comp.blocks.size() == 3);
    if (comp.blocks.size() == 3) {
        CHECK(comp.blocks[0].text == body);        // the bound body block
        CHECK(comp.blocks[1].text == body);        // {table_text}
        CHECK(comp.blocks[2].text.find(body) == std::string::npos);   // the code slot is NOT the body
    }

    // 3. And the design keeps drawing its OWN blocks — including the code slot in
    //    its corner, whose filled text this rule must not touch. (SlideBlocks
    //    leaves `blocks` empty on purpose: the caller draws the slide's own.)
    p::OutputStyleSpec unset;
    const auto own = p::SceneBuilder::ComposeSlide(slide, unset, /*applyGates=*/true);
    CHECK(own.source == p::SceneBuilder::SlideComposition::Source::SlideBlocks);
    CHECK(own.blocks.empty());
    CHECK(slide.blocks.size() == 3 && slide.blocks[0].text == "47-0412 4");

    // 4. THE REPORTED DESIGN ITSELF, through the real fill. It declares NO bind
    p::BookSource sermon;
    sermon.family = p::ContentFamily::Table;   // named: the shape of the reference and of the tokens follows it
    sermon.book = "47-0412 - Faith Is The Substance";
    sermon.code = "47-0412";
    sermon.chapter = 0;
    sermon.items = { p::BookItem{ 3, "I trust that sometime when I come back again to Oakland" } };
    const std::vector<p::ContentBlock> design = {
        slot("item-3", "{table_code_paragraphs}", 259.6),   // the code in the corner, FIRST
        slot("item-2", "{table_text}", 303.6),              // the body, UNBOUND
    };
    const auto filledSlides = p::FillBookSlides(design, sermon, p::BookFillOptions{}, /*onlyFirst=*/true);
    CHECK(filledSlides.size() == 1);
    if (filledSlides.size() == 1) {
        CHECK(filledSlides[0].body == "3 I trust that sometime when I come back again to Oakland");
        CHECK(filledSlides[0].blocks[0].text == "47-0412 3");        // the code slot still gets the code
        CHECK(filledSlides[0].blocks[1].text == filledSlides[0].body);   // ...and the body slot the body

        p::Slide carried = slide;
        carried.blocks = filledSlides[0].blocks;
        carried.text = filledSlides[0].body;
        CHECK(p::SlideRules::ContentText(carried) == filledSlides[0].body);

        // 5. THE REST OF THE VOCABULARY rides the same way. "It just remains the
        const p::BookValues &v = filledSlides[0].values;
        CHECK(v.code == "47-0412");
        CHECK(v.numbers == "3");
        CHECK(p::FillTokens("{table_code_paragraphs}", v) == "47-0412 3");
        CHECK(p::FillTokens("{table_reference}", v) == "47-0412 - Faith Is The Substance 3");

        p::Slide onAir = carried;
        onAir.metaJson = R"({"ref":"47-0412 - Faith Is The Substance 3","refFull":"47-0412 - Faith Is The Substance 3","refLast":"","tableCode":"47-0412","tableNumbers":"3","book":"47-0412 - Faith Is The Substance","contentType":"table"})";
        p::OutputStyleSpec corner;
        corner.familyTemplateBlocks[p::SlideRules::FamilyTemplateSlot("table")] = {
            slot("t1", "{table_text}", 45),
            slot("t2", "{table_code_paragraphs}", 970),
            slot("t3", "{table_reference_last}", 905),
        };
        const auto composed = p::SceneBuilder::ComposeSlide(onAir, corner, /*applyGates=*/true);
        CHECK(composed.blocks.size() == 3);
        if (composed.blocks.size() == 3) {
            CHECK(composed.blocks[0].text == onAir.text);            // the body
            CHECK(composed.blocks[1].text == "47-0412 3");           // the code + ITS paragraph numbers
            CHECK(composed.blocks[2].text.empty());
        }
        // The same slide on its LAST one: the fill's whole selection, verbatim.
        p::Slide last = onAir;
        last.metaJson = R"({"ref":"47-0412 - Faith Is The Substance 3","refFull":"47-0412 - Faith Is The Substance 3","refLast":"47-0412 - Faith Is The Substance 3","tableCode":"47-0412","tableNumbers":"3","contentType":"table"})";
        const auto lastScene = p::SceneBuilder::ComposeSlide(last, corner, /*applyGates=*/true);
        CHECK(lastScene.blocks.size() == 3);
        if (lastScene.blocks.size() == 3)
            CHECK(lastScene.blocks[2].text == "47-0412 - Faith Is The Substance 3");
    }
}

// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
void TestBlankTemplateFallsBackToSlideBlocks() {
    r::RenderEngine& engine = r::RenderEngine::Instance();
    CHECK(engine.SetBackend("software").ok());

    p::Presentation pres;
    pres.id = "pres-blank-tpl";
    p::SceneBuilder builder;

    const auto textBlock = [](const char* id, const char* text, double y) {
        p::ContentBlock b;
        b.id = id;
        b.kind = "text";
        b.x = 40; b.y = y; b.width = 674; b.height = 90;
        b.text = text;
        return b;
    };

    p::Slide tableSlide;
    tableSlide.id = "blank-tpl-1";
    tableSlide.metaJson = R"({"contentType":"table"})";
    tableSlide.blocks = {
        textBlock("b1", "Faith is the substance of things hoped for...", 60),
    };

    p::OutputStyleSpec style;
    style.name = "NDI screen";
    style.backgroundColor = "transparent";
    style.clearBackgroundOnText = true;
    style.backgroundImage = "";   // no disk dependency in a unit test
    style.familyTemplateKeys[p::SlideRules::FamilyTemplateSlot("table")] = "tpl-3";
    style.familyTemplateBlocks[p::SlideRules::FamilyTemplateSlot("table")] = {
        textBlock("t1", "{table_code_paragraphs}", 45),
        textBlock("t2", "{table_notes}", 150),
    };

    // 1. The decision itself: the template cannot paint, so the slide's own
    //    blocks own the composition (and `blocks` stays empty, which is what
    //    tells the renderer to draw the slide's own).
    const auto comp = p::SceneBuilder::ComposeSlide(tableSlide, style, /*applyGates=*/true);
    CHECK(comp.source == p::SceneBuilder::SlideComposition::Source::SlideBlocks);
    CHECK(comp.blocks.empty());

    // 2. ...and the real scene the outputs render carries the picture: the
    auto scene = builder.BuildSlideScene(pres, tableSlide, style, engine);
    CHECK(scene.ok());
    if (scene.ok()) {
        bool sawSlideBlock = false;
        for (const r::RenderObject* obj : engine.CollectObjects(scene.value()))
            if (const auto* t = dynamic_cast<const r::TextObject*>(obj);
                t != nullptr && t->Text() == tableSlide.blocks[0].text)
                sawSlideBlock = true;
        CHECK(sawSlideBlock);

        r::RenderOptions opts;
        opts.distribute = false;
        opts.capture = true;
        auto img = engine.Render(scene.value(), opts);
        CHECK(img.ok());
        if (img.ok() && !img.value().empty()) {
            bool painted = false;
            for (const uint32_t px : img.value().pixels)
                if (((px >> 24) & 0xFFu) != 0u) { painted = true; break; }
            CHECK(painted);   // the frame is no longer see-through
        }
        (void)engine.DestroyScene(scene.value());
    }

    // 3. CONTROL: a template that DOES paint keeps the composition, and the
    //    slide's own block stays out of it — the fallback is for a blank layout,
    //    not a licence for the slide to overrule a style that says something.
    p::OutputStyleSpec labelled = style;
    labelled.familyTemplateBlocks[p::SlideRules::FamilyTemplateSlot("table")] = {
        textBlock("t1", "{table_notes}", 45),   // resolves to nothing here too...
        textBlock("t2", "BRANDED", 300),         // ...but this slot really paints
    };
    const auto kept = p::SceneBuilder::ComposeSlide(tableSlide, labelled, /*applyGates=*/true);
    CHECK(kept.source == p::SceneBuilder::SlideComposition::Source::FamilyTemplate);
    CHECK(kept.blocks.size() == 2);

    // 4. The PRESET half of the same rule: a family key with no baked blocks is
    //    rendered as the slide's own title/body, and this slide has neither — so
    //    a preset may not claim a blocks-only slide either.
    p::OutputStyleSpec presetStyle;
    presetStyle.name = "Legacy lower third";
    presetStyle.templateKey = "lowerThird";
    presetStyle.familyTemplateKeys[p::SlideRules::FamilyTemplateSlot("table")] = "lowerThird";
    const auto presetComp = p::SceneBuilder::ComposeSlide(tableSlide, presetStyle, /*applyGates=*/true);
    CHECK(presetComp.source == p::SceneBuilder::SlideComposition::Source::SlideBlocks);

    // 5. ...while a slide that HAS a title/body still gets the preset it names:
    //    the fallback fires on an empty layout, it does not ban presets.
    p::Slide titled = tableSlide;
    titled.id = "blank-tpl-2";
    titled.blocks.clear();
    titled.title = "47-0412 - Faith Is The Substance 2";
    titled.text = "Faith is the substance of things hoped for...";
    const auto presetKept = p::SceneBuilder::ComposeSlide(titled, presetStyle, /*applyGates=*/true);
    CHECK(presetKept.source == p::SceneBuilder::SlideComposition::Source::FamilyPreset);
}

// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
void TestClearCarrierPaintsNothing() {
    // A real red PNG on disk — the style's branded backdrop, the thing the
    // report says comes back.
    const std::filesystem::path img =
        std::filesystem::temp_directory_path() / "bps_clear_carrier_test.png";
    {
        r::RgbaImage px;
        px.width = 4;
        px.height = 4;
        px.pixels.assign(16, 0xFF0000FFu);   // opaque red (R in the low byte)
        auto encoded = r::EncodePngRgba8(reinterpret_cast<const uint8_t *>(px.pixels.data()),
                                         px.width, px.height);
        CHECK(encoded.ok());
        std::ofstream file(img, std::ios::binary);
        file.write(reinterpret_cast<const char *>(encoded.value().data()),
                   static_cast<std::streamsize>(encoded.value().size()));
        CHECK(file.good());
    }

    (void)bps::ConfigurationManager::Instance().Set(
        "render.backend", bps::json::Value::String("software"));
    auto &render = r::RenderEngine::Instance();
    CHECK(render.Initialize().ok());
    CHECK(render.Start().ok());
    CHECK(render.SetBackend("software").ok());

    p::Slide blank;                 // the GO-LIVE-with-nothing carrier
    blank.id = "clear-test-blank";
    p::Slide clear = blank;
    clear.id = "clear-test-clear";
    clear.clearStage = true;        // ...and the CLEAR

    p::OutputStyleSpec style;
    style.name = "Clear Test Style";
    style.templateKey = "fullscreen";
    style.backgroundColor = "#ff000000";    // a colour that really paints
    style.backgroundImage = img.string();   // ...and a background image
    // ...and a template DESIGN as well, so "the layout paints nothing either"
    // is under test, not only the background pair.
    p::ContentBlock slab;
    slab.id = "clear-slab";
    slab.kind = "box";
    slab.x = 0; slab.y = 0; slab.width = 754; slab.height = 428;
    slab.style.backgroundColor = "#ff00ff00";
    style.templateBlocks = { slab };

    // 1. THE COMPOSITION, the two carriers side by side.
    const auto blankComp = p::SceneBuilder::ComposeSlide(blank, style, /*applyGates=*/true);
    const auto clearComp = p::SceneBuilder::ComposeSlide(clear, style, /*applyGates=*/true);
    CHECK(blankComp.source != p::SceneBuilder::SlideComposition::Source::Cleared);
    CHECK(blankComp.paintBackground);          // the style paints...
    CHECK(!blankComp.styleImageSuppressed);    // ...its image included
    CHECK(clearComp.source == p::SceneBuilder::SlideComposition::Source::Cleared);
    CHECK(!clearComp.paintBackground);         // the clear paints NOTHING...
    CHECK(clearComp.explicitTransparent);      // ...as a CHOSEN empty frame
    CHECK(clearComp.styleImageSuppressed);     // ...not even the background image
    CHECK(clearComp.blocks.empty());           // ...and no layout of its own
    CHECK(clearComp.frameTransparent);
    CHECK(!blankComp.frameTransparent);

    // 2. THE PIXELS, at the stage size the outputs actually use.
    p::Presentation pres;
    pres.id = "pres-clear-carrier";
    {
        p::SceneBuilder builder;
        r::RenderOptions opts;
        opts.capture = true;
        opts.distribute = false;
        const auto countNonBlack = [](const r::RgbaImage &f) {
            long long n = 0;
            for (uint32_t px : f.pixels)
                if ((px & 0x00FFFFFFu) != 0)
                    ++n;
            return n;
        };
        const auto countVisible = [](const r::RgbaImage &f) {
            long long n = 0;
            for (uint32_t px : f.pixels)
                if (((px >> 24) & 0xFFu) != 0 && (px & 0x00FFFFFFu) != 0)
                    ++n;
            return n;
        };

        auto builtBlank = builder.BuildSlideScene(pres, blank, style, render, r::Size(1920, 1080));
        CHECK(builtBlank.ok());
        if (builtBlank.ok()) {
            auto frame = render.Render(builtBlank.value(), opts);
            CHECK(frame.ok());
            if (frame.ok()) {
                // The control: an empty slide on a painting style is NOT an empty
                // frame — this is the GO-LIVE-with-nothing promise, untouched.
                CHECK(countNonBlack(frame.value()) > 0);
                CHECK(countVisible(frame.value()) > 0);
            }
            (void)render.DestroyScene(builtBlank.value());
        }

        auto builtClear = builder.BuildSlideScene(pres, clear, style, render, r::Size(1920, 1080));
        CHECK(builtClear.ok());
        if (builtClear.ok()) {
            auto frame = render.Render(builtClear.value(), opts);
            CHECK(frame.ok());
            if (frame.ok() && !frame.value().empty()) {
                CHECK(countNonBlack(frame.value()) == 0);
                CHECK(countVisible(frame.value()) == 0);
            }
            (void)render.DestroyScene(builtClear.value());
        }

        auto &compositor = p::CompositorState::Instance();
        p::CompositorState::MediaFrame onAirFrame;
        onAirFrame.width = 4;
        onAirFrame.height = 4;
        onAirFrame.rgba.assign(4 * 4 * 4, 0xFFu);   // opaque
        compositor.SetMediaFrame(onAirFrame);
        p::CompositorState::ActiveOverlay overlay;
        overlay.id = "clear-test-overlay";
        p::ContentBlock overlayText;
        overlayText.id = "clear-test-overlay-text";
        overlayText.kind = "text";
        overlayText.text = "overlay";
        overlayText.x = 60; overlayText.y = 60;
        overlayText.width = 300; overlayText.height = 80;
        overlay.blocks = { overlayText };
        compositor.SetActiveOverlays({ overlay });

        // THE CONTROL: the blank GO-LIVE carrier DOES carry those layers — the
        // media-only GO LIVE promise, untouched by this rule.
        auto blankWithLayers = builder.BuildSlideScene(pres, blank, style, render, r::Size(1920, 1080));
        CHECK(blankWithLayers.ok());
        if (blankWithLayers.ok()) {
            CHECK(render.GetObject(blankWithLayers.value(), "mediaonair").ok());
            CHECK(render.GetObject(blankWithLayers.value(), "ov1_1").ok());
            (void)render.DestroyScene(blankWithLayers.value());
        }
        // THE BLACKOUT: none of them, and still not one painted pixel.
        auto clearWithLayers = builder.BuildSlideScene(pres, clear, style, render, r::Size(1920, 1080));
        CHECK(clearWithLayers.ok());
        if (clearWithLayers.ok()) {
            CHECK(!render.GetObject(clearWithLayers.value(), "mediaonair").ok());
            CHECK(!render.GetObject(clearWithLayers.value(), "ov1_1").ok());
            CHECK(!render.GetObject(clearWithLayers.value(), "takeninput").ok());
            auto frame = render.Render(clearWithLayers.value(), opts);
            CHECK(frame.ok());
            if (frame.ok() && !frame.value().empty()) {
                CHECK(countNonBlack(frame.value()) == 0);
                CHECK(countVisible(frame.value()) == 0);
            }
            (void)render.DestroyScene(clearWithLayers.value());
        }
        compositor.ClearMedia();
        compositor.SetActiveOverlays({});
    }

    // 3. THE LIVE PATH the button really takes — and the trap beside it.
    auto &eng = p::PresentationEngine::Instance();
    CHECK(eng.Initialize().ok());
    auto &live = l::LiveOutputController::Instance();
    CHECK(live.Initialize().ok());
    CHECK(live.Start().ok());
    {
        CHECK(live.StartFromSlides("Clear Test", { blank }).ok());
        const std::string blankScene = eng.Runtime().Compiled().slides[0].sceneId;
        CHECK(live.StopLive().ok());
        CHECK(live.StartFromSlides("Clear Test", { clear }).ok());
        const std::string clearScene = eng.Runtime().Compiled().slides[0].sceneId;
        CHECK(!blankScene.empty());
        CHECK(!clearScene.empty());
        CHECK(blankScene != clearScene);
        CHECK(live.StopLive().ok());
        CHECK(eng.Delete("temp:Clear Test").ok());
    }

    std::error_code ec;
    (void)std::filesystem::remove(img, ec);
}

// ---------------------------------------------------------------------------
// A SHOW'S SLIDE KEEPS ITS OWN DESIGN ON THE OUTPUTS
// ---------------------------------------------------------------------------
void TestShowSlideKeepsItsOwnDesign() {
    const auto textBlock = [](const char* id, const char* text, double y, double h) {
        p::ContentBlock b;
        b.id = id;
        b.kind = "text";
        b.x = 40; b.y = y; b.width = 674; b.height = h;
        b.text = text;
        return b;
    };

    // The show slide: authored items only — no title, no body, no content family.
    p::Slide showSlide;
    showSlide.id = "show-1";
    showSlide.blocks = {
        textBlock("s1", "VICTORY AHEAD", 20, 60),                     // the title item
        textBlock("s2", "When the host of Israel led by God,", 300, 60),
    };
    // (Untagged is what the shows family IS — SlideRules::FamilyTemplateSlot.)

    // The style a show's output wears: a whole-style template (NOT keyed to any
    // content family), its own layout of literal text.
    p::OutputStyleSpec style;
    style.name = "Main Output";
    style.backgroundColor = "#ff000000";
    style.templateKey = "big-bold";
    style.templateBlocks = { textBlock("t1", "STYLE LAYOUT", 300, 200) };

    // 1. THE SHOW keeps its own composition: both authored items, not the style's
    //    single body slot. This is the assertion the outputs failed.
    const auto showComp = p::SceneBuilder::ComposeSlide(showSlide, style, /*applyGates=*/true);
    CHECK(showComp.source == p::SceneBuilder::SlideComposition::Source::SlideBlocks);
    CHECK(showComp.blocks.empty());      // empty = "draw the slide's own blocks"
    // ...and the style still supplies the background.
    CHECK(showComp.paintBackground);
    CHECK(showComp.background == std::string("#ff000000"));

    // 2. A CONTENT-FAMILY slide of the same shape is untouched: its blocks are
    //    the design's slots, so the style's template still owns it.
    p::Slide scriptureSlide = showSlide;
    scriptureSlide.id = "scripture-1";
    scriptureSlide.metaJson = R"({"contentType":"scripture"})";
    const auto scriptureComp =
        p::SceneBuilder::ComposeSlide(scriptureSlide, style, /*applyGates=*/true);
    CHECK(scriptureComp.source == p::SceneBuilder::SlideComposition::Source::StyleTemplate);
    CHECK(scriptureComp.blocks.size() == 1);

    // 3. A template PICKED IN THE STYLE'S SHOWS SLOT (Settings · Styles →
    p::OutputStyleSpec showsStyle = style;
    showsStyle.familyTemplateKeys[p::SlideRules::FamilyTemplateSlot("shows")] = "shows-tpl";
    showsStyle.familyTemplateBlocks[p::SlideRules::FamilyTemplateSlot("shows")] = {
        textBlock("f1", "SHOWS LAYOUT", 300, 200),
    };
    const auto showsComp = p::SceneBuilder::ComposeSlide(showSlide, showsStyle, /*applyGates=*/true);
    CHECK(showsComp.source == p::SceneBuilder::SlideComposition::Source::FamilyTemplate);
    CHECK(showsComp.blocks.size() == 1);

    // 3b. ...and that template IS the layout for a show slide with none of its
    //     own — the case it exists for.
    p::Slide plainForShows;
    plainForShows.id = "show-3";
    plainForShows.title = "Welcome";
    plainForShows.text = "Glad you are here";
    const auto plainShowsComp =
        p::SceneBuilder::ComposeSlide(plainForShows, showsStyle, /*applyGates=*/true);
    CHECK(plainShowsComp.source == p::SceneBuilder::SlideComposition::Source::FamilyTemplate);
    CHECK(plainShowsComp.blocks.size() == 1);

    p::OutputStyleSpec namedShowsStyle = style;
    namedShowsStyle.contentType = "shows";
    const auto namedShowsComp =
        p::SceneBuilder::ComposeSlide(showSlide, namedShowsStyle, /*applyGates=*/true);
    CHECK(namedShowsComp.source == p::SceneBuilder::SlideComposition::Source::SlideBlocks);
    CHECK(namedShowsComp.blocks.empty());

    p::OutputStyleSpec fallbackShowsStyle = style;
    fallbackShowsStyle.familyTemplateBlocks[p::SlideRules::FamilyTemplateSlot("shows")] = {
        textBlock("b1", "SCRIPTURE FALLBACK", 0, 400),
    };
    const auto fallbackShowsComp =
        p::SceneBuilder::ComposeSlide(showSlide, fallbackShowsStyle, /*applyGates=*/true);
    CHECK(fallbackShowsComp.source == p::SceneBuilder::SlideComposition::Source::SlideBlocks);
    CHECK(fallbackShowsComp.blocks.empty());

    // 4. A show slide with NO authored blocks (a plain title/body show) still
    //    wears the style's template: there is no design of its own to keep.
    p::Slide plainShow;
    plainShow.id = "show-2";
    plainShow.title = "Announcements";
    plainShow.text = "Youth night on Friday";
    const auto plainComp = p::SceneBuilder::ComposeSlide(plainShow, style, /*applyGates=*/true);
    CHECK(plainComp.source == p::SceneBuilder::SlideComposition::Source::StyleTemplate);

    CHECK(p::SlideRules::LayoutScope(style) == p::SlideRules::StyleLayoutScope::ContentFamilies);
    CHECK(p::SlideRules::LayoutScope(showsStyle) == p::SlideRules::StyleLayoutScope::Shows);
    {
        p::OutputStyleSpec bare;
        bare.backgroundColor = "#ff101020";       // a background with no template
        CHECK(p::SlideRules::LayoutScope(bare)
              == p::SlideRules::StyleLayoutScope::None);
        p::OutputStyleSpec keyedOnly;
        keyedOnly.familyTemplateKeys[p::SlideRules::FamilyTemplateSlot("table")] = "tpl-3";
        CHECK(p::SlideRules::LayoutScope(keyedOnly)
              == p::SlideRules::StyleLayoutScope::ContentFamilies);
        p::OutputStyleSpec named;
        named.contentType = "shows";
        CHECK(p::SlideRules::LayoutScope(named) == p::SlideRules::StyleLayoutScope::None);
    }

    // 5. ...and the SCENE every output rasterizes really carries both authored
    //    items. The decision above is the fix; this is the frame the NDI sender,
    //    the HDMI window and the distributed outputs are handed.
    r::RenderEngine& engine = r::RenderEngine::Instance();
    CHECK(engine.SetBackend("software").ok());
    p::Presentation pres;
    pres.id = "pres-show-design";
    p::SceneBuilder builder;
    auto scene = builder.BuildSlideScene(pres, showSlide, style, engine);
    CHECK(scene.ok());
    if (scene.ok()) {
        bool sawTitle = false, sawBand = false, sawTemplate = false;
        for (const r::RenderObject* obj : engine.CollectObjects(scene.value())) {
            const auto* t = dynamic_cast<const r::TextObject*>(obj);
            if (!t) continue;
            if (t->Text() == "VICTORY AHEAD") sawTitle = true;
            if (t->Text().rfind("When the host of Israel", 0) == 0) sawBand = true;
            if (t->Id() == "tpl1") sawTemplate = true;   // the style's own slot
        }
        CHECK(sawTitle);
        CHECK(sawBand);
        CHECK(!sawTemplate);
        (void)engine.DestroyScene(scene.value());
    }
}

// ---------------------------------------------------------------------------
// Bus windows — a "bus" block's live plane on the real output frame
// ---------------------------------------------------------------------------
void TestBusWindowPlane() {
    r::RenderEngine& engine = r::RenderEngine::Instance();
    CHECK(engine.Initialize().ok());
    CHECK(engine.SetBackend("software").ok());

    auto& compositor = p::CompositorState::Instance();
    auto solid = [](int w, int h) {
        p::CompositorState::MediaFrame f;
        f.width = static_cast<uint32_t>(w);
        f.height = static_cast<uint32_t>(h);
        f.rgba.assign(static_cast<size_t>(w) * h * 4, 0);
        for (size_t i = 0; i < f.rgba.size(); i += 4) {
            f.rgba[i + 0] = 0x00;   // R
            f.rgba[i + 1] = 0xFF;   // G
            f.rgba[i + 2] = 0x00;   // B
            f.rgba[i + 3] = 0xFF;   // A
        }
        return f;
    };

    p::Presentation pres;
    pres.id = "pres-bus-window";
    p::Slide slide;
    slide.id = "bus-window-slide";
    slide.title = "Bus 1 live";
    // The block the Bus chip inserts: a 16:9 window carrying the bus's STABLE
    // number (see BusSourceModal / EditScreen's onApplied).
    p::ContentBlock window;
    window.id = "busA";
    window.kind = "bus";
    window.x = 100; window.y = 60; window.width = 400; window.height = 225;
    window.metaJson = R"({"busId":3,"busName":"Camera 1","busType":"video"})";
    p::ContentBlock caption;
    caption.id = "cap";
    caption.kind = "text";
    caption.x = 100; caption.y = 300; caption.width = 400; caption.height = 60;
    caption.text = "Bus 1";
    caption.metaJson = R"({"color":"#ffffff","fontSize":30})";
    slide.blocks = { window, caption };

    p::OutputStyleSpec style;
    style.templateKey = "fullscreen";
    style.backgroundColor = "#ff000000";

    // What LiveOutputService publishes for an on-air bus block: the identity of
    // the bus's routed feed, then its decoded frames.
    compositor.SetBusFeeds({ p::CompositorState::BusFeed{3, "cam-1", "camera"} });
    CHECK(compositor.GetBusFeeds().size() == 1);
    compositor.SetBusFrame(3, solid(8, 8));
    CHECK(compositor.HasBusFrames());

    p::SceneBuilder builder;
    auto built = builder.BuildSlideScene(pres, slide, style, engine);
    CHECK(built.ok());
    if (!built.ok())
        return;

    const float expectX = 100.0f / 754.0f * 1920.0f;
    const float expectY = 60.0f / 428.0f * 1080.0f;
    const float expectW = 400.0f / 754.0f * 1920.0f;
    const float expectH = 225.0f / 428.0f * 1080.0f;

    auto probe = engine.GetObject(built.value(), "bus:busA");
    CHECK(probe.ok());
    if (probe.ok()) {
        auto* video = dynamic_cast<r::VideoObject*>(probe.value());
        CHECK(video != nullptr);
        if (video) {
            CHECK(video->HasFrame());
            CHECK(video->Frame().width == 8 && video->Frame().height == 8);
            // THE BLOCK'S RECTANGLE, scaled onto the frame — a full-frame plane
            // would pass "has a frame" while covering the whole design.
            CHECK(std::abs(video->Bounds().x - expectX) < 0.5f);
            CHECK(std::abs(video->Bounds().y - expectY) < 0.5f);
            CHECK(std::abs(video->Bounds().width - expectW) < 0.5f);
            CHECK(std::abs(video->Bounds().height - expectH) < 0.5f);
            // The compositor's video layer: under the slide's own text.
            CHECK(video->LayerId() == "media");
            const uint64_t firstRev = video->FrameRevision();

            compositor.SetBusFrame(3, solid(4, 4));
            auto again = builder.BuildSlideScene(pres, slide, style, engine);
            CHECK(again.ok() && again.value() == built.value());
            auto updated = engine.GetObject(built.value(), "bus:busA");
            CHECK(updated.ok());
            if (updated.ok()) {
                auto* v = dynamic_cast<r::VideoObject*>(updated.value());
                CHECK(v != nullptr);
                if (v) {
                    CHECK(v->FrameRevision() > firstRev);
                    CHECK(v->Frame().width == 4 && v->Frame().height == 4);
                    // ...and the object KNOWS which frame that was: the bus's own
                    // push counter rode along with the pixels.
                    CHECK(v->SourceSeq() == compositor.GetBusFrameSeq(3));
                    CHECK(v->SourceSeq() != 0);

                    const uint64_t quietRev = v->FrameRevision();
                    auto quietBuild = builder.BuildSlideScene(pres, slide, style, engine);
                    CHECK(quietBuild.ok() && quietBuild.value() == built.value());
                    auto quiet = engine.GetObject(built.value(), "bus:busA");
                    CHECK(quiet.ok());
                    if (quiet.ok()) {
                        auto* q = dynamic_cast<r::VideoObject*>(quiet.value());
                        CHECK(q != nullptr);
                        if (q) {
                            CHECK(q->FrameRevision() == quietRev);
                            CHECK(q->Frame().width == 4 && q->Frame().height == 4);
                        }
                    }
                }
            }

            compositor.ClearBusFrames();
            CHECK(!compositor.HasBusFrames());
            (void)builder.BuildSlideScene(pres, slide, style, engine);
            auto blanked = engine.GetObject(built.value(), "bus:busA");
            CHECK(blanked.ok());
            if (blanked.ok()) {
                auto* v = dynamic_cast<r::VideoObject*>(blanked.value());
                CHECK(v != nullptr);
                if (v) {
                    CHECK(v->SourceSeq() == 0);                              // no feed behind it
                    CHECK(v->Frame().width != 4 || v->Frame().height != 4);   // not the stopped picture
                    CHECK(std::abs(static_cast<float>(v->Frame().width) - expectW) < 1.5f);
                    CHECK(std::abs(static_cast<float>(v->Frame().height) - expectH) < 1.5f);
                }
            }
        }
    }

    // A bus block with no busId has nothing to show and nothing to guess: no
    // plane, and no device opened for a bus nobody named.
    p::Slide unnamed = slide;
    unnamed.id = "bus-window-unnamed";
    unnamed.blocks[0].metaJson = "{}";
    auto noneBuilt = builder.BuildSlideScene(pres, unnamed, style, engine);
    CHECK(noneBuilt.ok());
    if (noneBuilt.ok()) {
        CHECK(!engine.GetObject(noneBuilt.value(), "bus:busA").ok());
        (void)engine.DestroyScene(noneBuilt.value());
    }

    compositor.SetBusFeeds({});
    compositor.ClearBusFrames();
    (void)engine.DestroyScene(built.value());
}

// ---------------------------------------------------------------------------
// A TEMPLATE BUS's WINDOWS — which feed each window reads, and where it draws
// ---------------------------------------------------------------------------
void TestTemplateBusWindows() {
    r::RenderEngine& engine = r::RenderEngine::Instance();
    CHECK(engine.Initialize().ok());
    CHECK(engine.SetBackend("software").ok());

    auto& compositor = p::CompositorState::Instance();
    auto solid = [](int w, int h) {
        p::CompositorState::MediaFrame f;
        f.width = static_cast<uint32_t>(w);
        f.height = static_cast<uint32_t>(h);
        f.rgba.assign(static_cast<size_t>(w) * h * 4, 0);
        for (size_t i = 0; i < f.rgba.size(); i += 4) {
            f.rgba[i + 0] = 0x00;   // R
            f.rgba[i + 1] = 0xFF;   // G
            f.rgba[i + 2] = 0x00;   // B
            f.rgba[i + 3] = 0xFF;   // A
        }
        return f;
    };

    constexpr int kTemplateBus = 5;   // the bus the windows belong to
    constexpr int kNamedBus = 3;      // the bus the FIRST window names

    // namedA — its own assignment (bus 3), NOT a route of the template bus.
    p::ContentBlock namedA;
    namedA.id = "namedA";
    namedA.kind = "bus";
    namedA.x = 20; namedA.y = 20; namedA.width = 300; namedA.height = 169;
    namedA.metaJson = R"({"busId":3,"busName":"Camera 1","busType":"video"})";
    // openB / openC — no assignment: route 0 and route 1, in document order.
    p::ContentBlock openB;
    openB.id = "openB";
    openB.kind = "bus";
    openB.x = 340; openB.y = 20; openB.width = 300; openB.height = 169;
    openB.metaJson = "{}";
    p::ContentBlock openC;
    openC.id = "openC";
    openC.kind = "bus";
    openC.x = 20; openC.y = 200; openC.width = 300; openC.height = 169;
    openC.metaJson = "{}";

    const std::vector<p::ContentBlock> blocks = { namedA, openB, openC };

    const int route0 = p::CompositorState::TemplateWindowPlaneKey(kTemplateBus, 0);
    const int route1 = p::CompositorState::TemplateWindowPlaneKey(kTemplateBus, 1);
    // A window key is no bus number: the two spaces cannot collide, or a window
    // would paint whatever bus happens to carry that number.
    CHECK(route0 > kNamedBus && route1 > route0);

    compositor.ClearBusFrames();
    compositor.SetBusFrame(kNamedBus, solid(9, 9));
    compositor.SetBusFrame(route0, solid(6, 6));
    compositor.SetBusFrame(route1, solid(4, 4));

    p::SceneBuilder builder;
    auto built = builder.BuildTemplateBusScene(kTemplateBus, blocks, "#ff101010", engine);
    CHECK(built.ok());
    if (!built.ok())
        return;

    // The plane a window is drawn with, read the way the renderer reads it
    // (SceneBuilder records it as "w:<block id>").
    auto window = [&](const char *id) -> r::VideoObject* {
        auto obj = engine.GetObject(built.value(), std::string("w:") + id);
        if (!obj.ok())
            return nullptr;
        return dynamic_cast<r::VideoObject*>(obj.value());
    };
    // The rectangle the design names, in output pixels (the 754x428 stage grid
    // scaled onto a 1920x1080 frame — BlockRect's one derivation).
    auto expectRect = [](const p::ContentBlock& b) {
        return r::Rect(static_cast<float>(b.x / 754.0 * 1920.0),
                       static_cast<float>(b.y / 428.0 * 1080.0),
                       static_cast<float>(b.width / 754.0 * 1920.0),
                       static_cast<float>(b.height / 428.0 * 1080.0));
    };
    auto rectMatches = [](const r::VideoObject* v, const r::Rect& want) {
        return v && std::abs(v->Bounds().x - want.x) < 0.5f
                 && std::abs(v->Bounds().y - want.y) < 0.5f
                 && std::abs(v->Bounds().width - want.width) < 0.5f
                 && std::abs(v->Bounds().height - want.height) < 0.5f;
    };

    r::VideoObject* a = window("namedA");
    CHECK(a != nullptr);
    if (a) {
        CHECK(a->HasFrame());
        // ITS OWN BUS, not a route of the template bus — and, being named, it
        // consumes no route index (the whole point of this design).
        CHECK(a->Frame().width == 9 && a->Frame().height == 9);
        CHECK(a->SourceSeq() == compositor.GetBusFrameSeq(kNamedBus));
        CHECK(a->SourceSeq() != 0);
        CHECK(rectMatches(a, expectRect(namedA)));
        CHECK(a->LayerId() == "media");
    }

    r::VideoObject* b = window("openB");
    CHECK(b != nullptr);
    if (b) {
        // ROUTE 0, even though it is the SECOND bus block on the design.
        CHECK(b->HasFrame());
        CHECK(b->Frame().width == 6 && b->Frame().height == 6);
        CHECK(b->SourceSeq() == compositor.GetBusFrameSeq(route0));
        CHECK(b->SourceSeq() != 0);
        CHECK(rectMatches(b, expectRect(openB)));
    }

    r::VideoObject* c = window("openC");
    CHECK(c != nullptr);
    if (c) {
        CHECK(c->HasFrame());
        CHECK(c->Frame().width == 4 && c->Frame().height == 4);
        CHECK(c->SourceSeq() == compositor.GetBusFrameSeq(route1));
        CHECK(c->SourceSeq() != 0);
        CHECK(rectMatches(c, expectRect(openC)));
    }

    compositor.ClearBusFrames();
    compositor.SetBusFrame(route1, solid(4, 4));
    auto again = builder.BuildTemplateBusScene(kTemplateBus, blocks, "#ff101010", engine);
    CHECK(again.ok() && again.value() == built.value());

    r::VideoObject* b2 = window("openB");
    CHECK(b2 != nullptr && b2->SourceSeq() == 0);
    r::VideoObject* c2 = window("openC");
    CHECK(c2 != nullptr);
    if (c2) {
        CHECK(c2->Frame().width == 4 && c2->Frame().height == 4);
        CHECK(c2->SourceSeq() == compositor.GetBusFrameSeq(route1));
    }

    compositor.ClearBusFrames();
    compositor.SetBusFeeds({});
    (void)engine.DestroyScene(built.value());
}

// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
void TestMediaFitOnTheFileWins() {
    (void)bps::ConfigurationManager::Instance().Set(
        "render.backend", bps::json::Value::String("software"));
    r::RenderEngine& engine = r::RenderEngine::Instance();
    CHECK(engine.Initialize().ok());
    CHECK(engine.Start().ok());
    CHECK(engine.SetBackend("software").ok());

    p::Presentation pres;
    pres.id = "pres-media-fit";
    p::Slide slide;
    slide.id = "media-fit-1";   // no blocks: the take is the whole picture
    slide.background = "";

    p::OutputStyleSpec style;
    style.name = "Fit output";
    style.templateKey = "fullscreen";
    style.backgroundColor = "#ff00ff00";   // a style colour that MUST clear
    style.mediaFit = "contain";            // the output's own default

    p::SceneBuilder builder;
    auto& compositor = p::CompositorState::Instance();

    // A PORTRAIT take (100x200): contain letterboxes it, cover fills the stage,
    // fill stretches over it. Solid, because only the geometry is under test.
    auto takeWithFit = [&compositor](std::string fit) {
        p::CompositorState::MediaFrame frame;
        frame.width = 100;
        frame.height = 200;
        frame.fit = std::move(fit);
        frame.rgba.assign(static_cast<size_t>(frame.width) * frame.height * 4, 0);
        for (size_t i = 0; i < frame.rgba.size(); i += 4) {
            frame.rgba[i + 0] = 0x40;   // R
            frame.rgba[i + 1] = 0x80;   // G
            frame.rgba[i + 2] = 0xC0;   // B
            frame.rgba[i + 3] = 0xFF;   // A
        }
        frame.hasAlpha = false;
        compositor.SetMediaFrame(std::move(frame));
    };
    // The SAME frame, but with real transparency in its pixels and the flag to
    // match (a keyed PNG or an alpha WebP on air).
    auto takeWithAlpha = [&compositor] {
        p::CompositorState::MediaFrame frame;
        frame.width = 100;
        frame.height = 200;
        frame.rgba.assign(static_cast<size_t>(frame.width) * frame.height * 4, 0);
        for (size_t i = 0; i < frame.rgba.size(); i += 4) {
            frame.rgba[i + 0] = 0x40;   // R
            frame.rgba[i + 1] = 0x80;   // G
            frame.rgba[i + 2] = 0xC0;   // B
            frame.rgba[i + 3] = 0x00;   // A — transparent
        }
        frame.hasAlpha = true;
        compositor.SetMediaFrame(std::move(frame));
    };

    auto mediaBounds = [&engine](const std::string& sceneId, const char* name) {
        auto obj = engine.GetObject(sceneId, name);
        if (!obj.ok()) return r::Rect();
        auto* video = dynamic_cast<r::VideoObject*>(obj.value());
        return video ? video->Bounds() : r::Rect();
    };

    // 1. CONTAIN IS THE DEFAULT when the file names none — the whole picture,
    takeWithFit("");
    auto built = builder.BuildSlideScene(pres, slide, style, engine);
    CHECK(built.ok());
    if (!built.ok())
        return;
    const std::string sceneId = built.value();
    {
        const r::Rect rect = mediaBounds(sceneId, "mediaonair");
        CHECK(rect.height > 1079.0f);          // full height...
        CHECK(rect.width < 700.0f);            // ...and the picture's own width
        CHECK(!engine.GetObject(sceneId, "mediabars").ok());
        // THE TAKE CLEARS THE STYLE: no background object of the style's, and the
        // composition says so (the preview surfaces read this to clear too).
        CHECK(!engine.GetObject(sceneId, "bg").ok());
        const auto comp = p::SceneBuilder::ComposeSlide(slide, style);
        CHECK(comp.clearedByTake);
        CHECK(!comp.paintBackground);
        CHECK(comp.background.empty());
        CHECK(comp.styleImageSuppressed);
        CHECK(!comp.frameTransparent);
        CHECK(!compositor.MediaFrameIsTransparent());
    }

    takeWithAlpha();
    {
        CHECK(compositor.MediaFrameIsTransparent());
        const auto comp = p::SceneBuilder::ComposeSlide(slide, style);
        CHECK(comp.clearedByTake);
        CHECK(comp.frameTransparent);
    }

    // 2. THE FILE'S OWN FIT WINS: "cover" on the file crops it to the stage even
    takeWithFit("cover");
    auto again = builder.BuildSlideScene(pres, slide, style, engine);
    CHECK(again.ok());
    if (again.ok()) {
        CHECK(again.value() == sceneId);
        const r::Rect rect = mediaBounds(sceneId, "mediaonair");
        CHECK(std::abs(rect.width - 1920.0f) < 0.5f);
        CHECK(std::abs(rect.height - 3840.0f) < 0.5f);
    }

    // 3. "fill" on the file stretches to the stage, and the OUTPUT's default is
    //    overridden the other way too: a file saying "contain" on an output that
    //    says "cover" keeps its own aspect.
    takeWithFit("fill");
    (void)builder.BuildSlideScene(pres, slide, style, engine);
    {
        const r::Rect rect = mediaBounds(sceneId, "mediaonair");
        CHECK(std::abs(rect.width - 1920.0f) < 0.5f);
        CHECK(std::abs(rect.height - 1080.0f) < 0.5f);
    }
    style.mediaFit = "cover";
    takeWithFit("contain");
    (void)builder.BuildSlideScene(pres, slide, style, engine);
    {
        const r::Rect rect = mediaBounds(sceneId, "mediaonair");
        CHECK(rect.width < 700.0f);            // the FILE's answer, not the output's
        CHECK(rect.height > 1079.0f);
        CHECK(!engine.GetObject(sceneId, "mediabars").ok());
    }

    style.mediaFit = "cover";
    takeWithFit("");
    (void)builder.BuildSlideScene(pres, slide, style, engine);
    {
        const r::Rect rect = mediaBounds(sceneId, "mediaonair");
        CHECK(rect.width < 700.0f);            // NOT the style/output's "cover"
        CHECK(rect.height > 1079.0f);
        CHECK(!engine.GetObject(sceneId, "mediabars").ok());
    }

    // 4. OFF AIR the style's own background is back: clearing the take re-keys to
    //    the scene that still has its poster (the "@m:on" token in the scene id).
    compositor.ClearMedia();
    auto plain = builder.BuildSlideScene(pres, slide, style, engine);
    CHECK(plain.ok());
    if (plain.ok()) {
        CHECK(plain.value() != sceneId);
        CHECK(engine.GetObject(plain.value(), "bg").ok());
        CHECK(!engine.GetObject(plain.value(), "mediaonair").ok());
        const auto comp = p::SceneBuilder::ComposeSlide(slide, style);
        CHECK(!comp.clearedByTake);
        CHECK(comp.paintBackground);
        CHECK(comp.background == style.backgroundColor);
        CHECK(!comp.frameTransparent);
    }

    // 5. A TAKEN CAMERA/SCREEN FEED IS AN OPAQUE PICTURE TOO: the feed replaces
    //    the frame the style described, so the composition says the frame is not
    //    transparent even where the style named no colour at all.
    {
        style.backgroundColor = "";            // nothing named: keyable on its own
        compositor.SetTakenInput("selftest-device", "camera");
        const auto comp = p::SceneBuilder::ComposeSlide(slide, style);
        CHECK(!comp.frameTransparent);
        compositor.ClearTakenInput();
        const auto withoutTake = p::SceneBuilder::ComposeSlide(slide, style);
        CHECK(withoutTake.frameTransparent);    // ...and keyable once it is gone
        style.backgroundColor = "#ff00ff00";
        (void)engine.DestroyScene(plain.value());
    }
    (void)engine.DestroyScene(sceneId);
}

// ---------------------------------------------------------------------------
// ShippedConfig — the BUILD's configuration reaching an install that exists
// ---------------------------------------------------------------------------
void TestShippedConfig() {
    using J = bps::json::Value;
    auto& fs = bps::platform::PlatformAccessor::Get().Filesystem();
    const std::string root = "/tmp/bps_shipped_config";
    (void)fs.RemoveAll(root);
    const std::string dbFile = root + "/kernel.json";
    const std::string baseline = root + "/kernel.shipped.json";

    auto& db = DatabaseManager::Instance();
    CHECK(db.Open(dbFile).ok());

    const auto entry = [](const char* id, const char* name) {
        J::Object o;
        o["id"] = J::String(id);
        o["name"] = J::String(name);
        return J(std::move(o));
    };

    // The install's rosters: a style the build ALSO ships (older definition) and
    // one style of the user's own.
    {
        J::Array styles;
        styles.push_back(entry("s1", "Main Output (old)"));
        styles.push_back(entry("mine", "My Sunday look"));
        CHECK(db.Put("styles", "roster", J(std::move(styles))).ok());
    }

    // What this build ships: refreshed s1, a new style, a new output, and a
    // production graph (which is this machine's hardware and must be left alone).
    const auto baselineJson = [&](const char* secondName, bool withOutput) {
        J::Array shippedStyles;
        shippedStyles.push_back(entry("s1", "Main Output"));
        shippedStyles.push_back(entry("s2", secondName));
        J::Object styles;
        styles["roster"] = J(std::move(shippedStyles));

        J::Object collections;
        collections["styles"] = J(std::move(styles));
        if (withOutput) {
            J::Array outputs;
            outputs.push_back(entry("out-1", "Main Output"));
            J::Object outs;
            outs["roster"] = J(std::move(outputs));
            collections["outputs"] = J(std::move(outs));
        }
        J::Array nodes;
        nodes.push_back(entry("bus:this-machine", "Studio"));
        J::Object graph;
        graph["nodes"] = J(std::move(nodes));
        J::Object production;
        production["graph"] = J(std::move(graph));
        collections["production"] = J(std::move(production));

        J::Object document;
        document["version"] = J::Number(1);
        document["collections"] = J(std::move(collections));
        return J(std::move(document)).ToString();
    };
    CHECK(fs.WriteAtomic(baseline, baselineJson("NDI Screen", true)).ok());

    // ---- first application: refresh, add, keep, never delete ----
    const auto first = proj::ApplyShippedConfigurationFrom(baseline);
    CHECK(first.applied && !first.fingerprint.empty());
    CHECK(first.added == 2 && first.refreshed == 1);   // s2 + out-1 added, s1 refreshed

    auto styles = db.Get("styles", "roster");
    CHECK(styles.ok() && styles.value().asArray() && styles.value().asArray()->size() == 3);
    if (styles.ok() && styles.value().asArray()) {
        const auto& list = *styles.value().asArray();
        CHECK(list[0].Find("name")->asString() == std::string("Main Output"));   // refreshed in place
        CHECK(list[1].Find("id")->asString() == std::string("mine"));            // the user's, same place
        CHECK(list[2].Find("id")->asString() == std::string("s2"));              // appended
    }
    auto outputs = db.Get("outputs", "roster");
    CHECK(outputs.ok() && outputs.value().asArray() && outputs.value().asArray()->size() == 1);
    CHECK(!db.Get("production", "graph").ok());   // a per-machine graph is not merged

    // ---- the same file again is a no-op (the fingerprint gate) ----
    const auto again = proj::ApplyShippedConfigurationFrom(baseline);
    CHECK(!again.applied && again.fingerprint.empty());

    // ---- a DIFFERENT build applies once more; the user's entry still wins ----
    CHECK(fs.WriteAtomic(baseline, baselineJson("NDI Screen 2", false)).ok());
    const auto second = proj::ApplyShippedConfigurationFrom(baseline);
    CHECK(second.applied && second.refreshed == 1 && second.added == 0);
    styles = db.Get("styles", "roster");
    CHECK(styles.ok() && styles.value().asArray() && styles.value().asArray()->size() == 3);
    if (styles.ok() && styles.value().asArray()) {
        const auto& list = *styles.value().asArray();
        CHECK(list[1].Find("id")->asString() == std::string("mine"));            // still there
        CHECK(list[2].Find("name")->asString() == std::string("NDI Screen 2"));  // refreshed
    }

    // ---- the record and the rosters survive a reopen (Flush did its job) ----
    CHECK(db.Flush().ok());
    CHECK(db.Open(dbFile).ok());
    CHECK(db.Get("config", "shipped").ok());
    auto persisted = db.Get("styles", "roster");
    CHECK(persisted.ok() && persisted.value().asArray() && persisted.value().asArray()->size() == 3);

    // ---- the rule itself, without a store ----
    size_t added = 0, refreshed = 0;
    J::Array unidentified;
    J::Object noId;
    noId["name"] = J::String("unmatched");
    unidentified.push_back(J(std::move(noId)));
    J::Array storedList;
    J::Object storedSingle;
    storedSingle["id"] = J::String("x");
    storedList.push_back(J(std::move(storedSingle)));
    const auto untouched =
        proj::MergeShippedEntries(J(std::move(storedList)), J(std::move(unidentified)), added, refreshed);
    CHECK(added == 0 && refreshed == 0 && untouched.asArray() && untouched.asArray()->size() == 1
          && (*untouched.asArray())[0].Find("id")->asString() == std::string("x"));

    added = refreshed = 0;
    J::Array onlyUser;
    onlyUser.push_back(entry("a", "A"));
    const auto unchanged =
        proj::MergeShippedEntries(J(std::move(onlyUser)), J::String("not a list"), added, refreshed);
    CHECK(added == 0 && refreshed == 0 && unchanged.asArray() && unchanged.asArray()->size() == 1);

    (void)fs.RemoveAll(root);
    CHECK(db.Open("").ok());   // leave the shared store in-memory for the next suite
}

// A show slide in a category the style OVERRIDES must be drawn with that category's template,
// not the style's default Shows template: Quiet Time (cross picture, script font) went live as the
// style's "Big Bold" (black, plain text) while the monitor showed the Quiet Time design.
void TestShowCategoryOverrideWins() {
    auto block = [](const char* id, const char* kind, const char* bind, const char* text) {
        p::ContentBlock b;
        b.id = id; b.kind = kind; b.bind = bind; b.text = text;
        b.x = 0; b.y = 0; b.width = 754; b.height = 400;
        return b;
    };
    p::Slide slide;
    slide.id = "show-1";
    slide.title = "Verse 1";
    slide.text = "This, in the sanctuary here, is where the Lord dwells.";
    slide.metaJson = R"({"line1":"This, in the sanctuary here, is where the Lord dwells.","ref":"63-1226 - Church Order Pr 150","showCategory":"Quiet Time","templateOverride":false})";
    slide.blocks = { block("item-1", "text", "", "This, in the sanctuary here, is where the Lord dwells."),
                     block("item-2", "text", "", "63-1226 - Church Order Pr 150") };

    p::OutputStyleSpec style;
    style.templateKey = "lowerThird";
    style.SetFamilyGate(p::kFamilySlotShows, true);
    style.familyTemplateKeys[p::kFamilySlotShows] = "tpl-big_bold";
    style.familyTemplateBlocks[p::kFamilySlotShows] = { block("big", "text", "text", "") };

    p::OutputStyleSpec::ShowCategoryTemplate quiet;
    quiet.category = "Quiet Time";
    quiet.templateKey = "tpl-5";
    quiet.overrideEnabled = true;
    quiet.blocks = { block("item-3", "image", "", "cross7"),
                     block("item-1", "text", "title", "Title"),
                     block("item-2", "text", "text", "Text") };
    style.showCategoryTemplates.push_back(quiet);

    const auto comp = p::SceneBuilder::ComposeSlide(slide, style, /*applyGates=*/true);
    std::printf("[showcat] source=%d blocks=%zu first=%s\n", static_cast<int>(comp.source),
                comp.blocks.size(), comp.blocks.empty() ? "-" : comp.blocks[0].kind.c_str());
    CHECK(comp.source == p::SceneBuilder::SlideComposition::Source::FamilyTemplate);
    CHECK(comp.blocks.size() == 3);                                // the category's three, not Big Bold's one
    if (!comp.blocks.empty())
        CHECK(comp.blocks[0].kind == "image");                      // the cross picture

    // ...and without the override (or for another category) the style's default Shows template stands.
    p::Slide other = slide;
    other.metaJson = R"({"showCategory":"Sermons","templateOverride":false})";
    const auto fallback = p::SceneBuilder::ComposeSlide(other, style, /*applyGates=*/true);
    CHECK(fallback.blocks.size() == 1);
}

void TestForcedLineBreaks() {
    auto& engine = r::RenderEngine::Instance();
    CHECK(engine.Initialize().ok());
    CHECK(engine.SetBackend("software").ok());
    const r::FontManager& fonts = engine.Fonts();

    const std::string text = "one two three four five six\nseven eight";
    //                        0       8          19         28
    auto forced = std::make_shared<r::ForcedLineBreaks>();
    forced->starts = {0, 8, 19, 28};
    forced->textHash = r::TextLayout::TextHash(text);

    r::TextStyle style;
    style.fontId = "Segoe UI";
    style.size = 40.0f;
    style.wrap = true;
    style.wrapWidth = 2000.0f;          // wide: normal wrapping would keep each paragraph on one line
    style.forcedBreaks = forced;

    // The table IS the character map when the text is the one it was cut from.
    CHECK(r::TextLayout::FlowCharacterStarts(text, style, fonts) == forced->starts);

    const auto layout = r::TextLayout::Measure(text, style, fonts);
    if (fonts.HasSystemFont(fonts.SubstituteFamily(style.fontId))) {
        // Exactly the lines the table names, whatever the wrap width says.
        CHECK(layout.lines.size() == 4);
        if (layout.lines.size() == 4) {
            CHECK(layout.lines[0].text == "one two");
            CHECK(layout.lines[1].text == "three four");
            CHECK(layout.lines[2].text == "five six");
            CHECK(layout.lines[3].text == "seven eight");
            CHECK(!layout.lines[0].paragraphEnd);    // a soft break: justified
            CHECK(!layout.lines[1].paragraphEnd);
            CHECK(layout.lines[2].paragraphEnd);     // ends at the paragraph's newline
            CHECK(layout.lines[3].paragraphEnd);     // the text's last line
        }
    }

    // Different text: the table is not applied, the text wraps on its own.
    r::TextStyle other = style;
    other.wrapWidth = 2000.0f;
    const auto ownWrap = r::TextLayout::Measure(text + " more", other, fonts);
    CHECK(ownWrap.lines.size() == 2);   // two paragraphs, nothing forced
}

void TestFlowScrollMath() {
    namespace fs = bps::rendering::flowscroll;
    const std::vector<int> reference{0, 40, 80, 125, 160};   // a wide wrap
    const std::vector<int> narrow{0, 18, 37, 60, 79, 100, 121, 140, 160, 181};   // a narrow one

    // Reference line <-> character round-trips exactly, so the tile that speaks the
    // reference's own lines and the engine reading the character agree.
    for (double line : {0.0, 0.25, 1.0, 1.5, 2.9, 3.0})
        CHECK(std::abs(fs::LineForChar(reference, fs::CharForLine(reference, line)) - line) < 1e-9);

    // Half-way through reference line 1 is 20 characters in. Every wrap must put its
    // own line at the line holding that character, fractionally (linear in chars).
    const double ch = fs::CharForLine(reference, 1.5);
    CHECK(std::abs(ch - 60.0) < 1e-9);
    CHECK(std::abs(fs::LineForChar(narrow, ch) - 3.0) < 1e-9);   // char 60 starts narrow line 3

    // A glide of N characters moves a surface by N / (its line length) lines: the same
    // words in the same time, whatever the wrap.
    const double a = fs::LineForChar(narrow, 45.0), b = fs::LineForChar(narrow, 55.0);
    CHECK(std::abs((b - a) - 10.0 / 23.0) < 1e-9);

    // Past the passage it rests on the last line instead of running off blank.
    CHECK(fs::LineForChar(narrow, 5000.0) <= static_cast<double>(narrow.size() - 1));
    CHECK(fs::LineForChar(std::vector<int>{}, 7.5) == 7.5);   // no table: already lines
}

void TestReadingFlowWindowRefresh() {
    auto& engine = r::RenderEngine::Instance();
    CHECK(engine.Initialize().ok());
    CHECK(engine.SetBackend("software").ok());

    // Every line is distinct text, so a picture of the wrong window is a
    // different picture.
    const auto line = [](int i) { return "line " + std::to_string(i) + " of the passage"; };
    const auto textFrom = [&](int from, int count) {
        std::string text;
        for (int i = from; i < from + count; ++i) {
            if (i > from) text += "\n";
            text += line(i);
        }
        return text;
    };
    // The character a line starts at inside a window that begins at `from`.
    const auto lineStart = [&](int from, int upto) {
        size_t at = 0;
        for (int i = from; i < upto; ++i) at += line(i).size() + 1;
        return static_cast<float>(at);
    };

    p::Presentation pres;
    pres.id = "reading-window-refresh";
    p::Slide slide;
    slide.id = "reading-window-refresh-1";
    slide.text = textFrom(0, 6);        // the PAGE (six lines)
    slide.flowText = textFrom(0, 12);   // the passage window the block scrolls
    slide.flowChar = 0.0f;

    p::SceneBuilder builder;
    const auto built = builder.BuildSlideScene(pres, slide, engine, r::Size(400, 240));
    CHECK(built.ok());
    if (!built.ok())
        return;
    const std::string sceneId = built.value();

    const auto readingObject = [&]() -> r::TextObject* {
        for (auto* object : engine.CollectObjects(sceneId))
            if (auto* text = dynamic_cast<r::TextObject*>(object); text && text->HasReadingFlow())
                return text;
        return nullptr;
    };
    r::TextObject* reading = readingObject();
    CHECK(reading != nullptr);
    if (!reading)
        return;
    CHECK(reading->FlowPageLines() == 6);          // the page, not the window
    CHECK(reading->Text() == slide.flowText);
    // A position past the window's last line is clamped INTO the window: that is
    // the old window's ceiling, and the reason a moved window needs the scene.
    float pastWindow = lineStart(0, 15);
    CHECK(reading->FlowLineForChar(pastWindow) < 12.0f);

    const auto ink = [](const r::RgbaImage& img) {
        if (img.empty()) return size_t{0};
        const uint32_t base = img.pixels.front();
        size_t painted = 0;
        for (uint32_t px : img.pixels)
            if (px != base) ++painted;
        return painted;
    };
    const auto frameAt = [&](float charAt) {
        r::RenderOptions opts;
        opts.capture = true;
        opts.distribute = false;
        opts.contentScrollChars = charAt;
        opts.contentScrollFromScene = false;
        auto frame = engine.RenderOffscreen(sceneId, r::Size(400, 240), opts);
        return frame.ok() ? frame.value() : r::RgbaImage{};
    };
    const auto topBefore = frameAt(0.0f);
    const auto deepBefore = frameAt(pastWindow);
    CHECK(ink(topBefore) > 0);
    CHECK(ink(deepBefore) > 0);    // clamped to the old window's last line

    // ---- the window MOVES under the same scene ------------------------------
    p::Slide moved = slide;
    moved.flowText = textFrom(0, 30);            // the same page, a longer passage
    moved.flowChar = pastWindow;                 // resting where it could not reach
    CHECK(builder.RefreshReadingFlow(engine, sceneId, moved));

    // THE SCENE IS THE SAME ONE — not a rebuild (a rebuild would mint a new id
    // and a second cached scene per page turn).
    CHECK(engine.GetScene(sceneId).ok());
    CHECK(builder.BuildSlideScene(pres, moved, engine, r::Size(400, 240)).value() == sceneId);
    reading = readingObject();
    CHECK(reading != nullptr);
    if (!reading)
        return;
    CHECK(reading->Text() == moved.flowText);
    CHECK(reading->FlowPageLines() == 6);        // the page did not change, only the window
    // The position the old window clamped is a real line now.
    CHECK(reading->FlowLineForChar(pastWindow) >= 14.0f);

    // ...and the output follows: the page's top is the same picture (the words
    // there did not move), while the deep position draws where it was blank.
    const auto topAfter = frameAt(0.0f);
    const auto deepAfter = frameAt(pastWindow);
    CHECK(ink(topAfter) > 0);
    CHECK(ink(deepAfter) > 0);
    CHECK(topAfter.pixels == topBefore.pixels);
    CHECK(!(deepAfter.pixels == deepBefore.pixels));

    p::Slide slideMode = moved;
    slideMode.flowText.clear();
    slideMode.flowChar = 0.0f;
    CHECK(builder.RefreshReadingFlow(engine, sceneId, slideMode));
    CHECK(!reading->HasReadingFlow());
    CHECK(!reading->HasFlowCharTable());
    CHECK(reading->Text() == slide.text);
    const auto restoredPage = frameAt(0.0f);
    p::Slide plainSlide = slide;             // the page, NO passage attached
    plainSlide.id = "reading-window-refresh-plain";
    plainSlide.flowText.clear();
    plainSlide.flowChar = -1.0f;
    const auto plainBuilt =
        builder.BuildSlideScene(pres, plainSlide, engine, r::Size(400, 240));
    CHECK(plainBuilt.ok());
    if (!plainBuilt.ok())
        return;
    const auto plainFrame = [&] {
        r::RenderOptions opts;
        opts.capture = true;
        opts.distribute = false;
        auto frame = engine.RenderOffscreen(plainBuilt.value(), r::Size(400, 240), opts);
        return frame.ok() ? frame.value() : r::RgbaImage{};
    }();
    CHECK(!plainFrame.empty());
    CHECK(restoredPage.pixels == plainFrame.pixels);
    const auto restoredDeep = frameAt(pastWindow);
    CHECK(restoredDeep.pixels == restoredPage.pixels);

    // Switching back to Scroll mode must re-attach the flow to this same scene.
    CHECK(builder.RefreshReadingFlow(engine, sceneId, moved));
    CHECK(reading->HasReadingFlow());
    CHECK(reading->Text() == moved.flowText);

    // A scene that draws no flow says so instead of guessing (a refused family /
    // a bus feed reach this with no reading object at all).
    CHECK(!builder.RefreshReadingFlow(engine, "reading-window-refresh-missing", moved));

    (void)engine.DestroyScene(sceneId);
}

void TestReadingPageKeepsPick() {
    auto& render = r::RenderEngine::Instance();
    CHECK(render.Initialize().ok());
    CHECK(render.Start().ok());
    CHECK(render.SetBackend("software").ok());
    auto& eng = p::PresentationEngine::Instance();
    CHECK(eng.Initialize().ok());
    auto& live = l::LiveOutputController::Instance();
    CHECK(live.Initialize().ok());
    CHECK(live.Start().ok());

    const auto line = [](int i) { return "line " + std::to_string(i) + " of the passage"; };
    const auto textFrom = [&](int count) {
        std::string text;
        for (int i = 0; i < count; ++i) {
            if (i > 0) text += "\n";
            text += line(i);
        }
        return text;
    };

    p::Slide slide;
    slide.title = "Read Through";
    slide.text = textFrom(6);               // the page
    slide.flowText = textFrom(12);          // the passage the reading block scrolls
    slide.flowChar = 0.0f;

    CHECK(live.StartFromSlides("Reading Pick", {slide}).ok());
    CHECK(live.RenderOnce().ok());
    const p::Presentation* onAir = eng.Runtime().ActivePresentation();
    CHECK(onAir != nullptr && onAir->slides.size() == 1);
    if (!onAir || onAir->slides.empty())
        return;
    CHECK(eng.Runtime().Compiled().slides.size() == 1);
    const std::string sceneBefore = eng.Runtime().Compiled().slides[0].sceneId;
    CHECK(!sceneBefore.empty());

    // ---- the window moves, the pick does not -------------------------------
    p::Slide moved = slide;
    moved.flowText = textFrom(30);
    moved.flowChar = 260.0f;
    CHECK(live.UpdateReadingSlide("Reading Pick", moved).ok());
    CHECK(live.RenderOnce().ok());

    // THE PICK IS THE SAME ONE: the on-air slide carries the new window and the
    // scene that was already built draws it — no recompile, no rebuild.
    onAir = eng.Runtime().ActivePresentation();
    CHECK(onAir != nullptr && onAir->slides.size() == 1);
    if (onAir && !onAir->slides.empty()) {
        CHECK(onAir->slides[0].flowText == moved.flowText);
        CHECK(std::abs(onAir->slides[0].flowChar - moved.flowChar) < 0.01f);
    }
    CHECK(eng.Runtime().Compiled().slides.size() == 1);
    CHECK(eng.Runtime().Compiled().slides[0].sceneId == sceneBefore);
    CHECK(render.GetScene(sceneBefore).ok());
    // ...and no content replacement was queued behind it.
    CHECK(!live.IsLiveContentReplacementPending());
    bool flowAttached = false;
    for (auto* object : render.CollectObjects(sceneBefore))
        if (auto* text = dynamic_cast<r::TextObject*>(object);
            text && text->HasReadingFlow() && text->Text() == moved.flowText)
            flowAttached = true;
    CHECK(flowAttached);

    // ---- a DIFFERENT picture is still a real commit ------------------------
    p::Slide other = moved;
    other.title = "A Different Page";       // the page's own words/reference moved
    other.text = textFrom(4);
    CHECK(live.UpdateReadingSlide("Reading Pick", other).ok());
    CHECK(live.RenderOnce().ok());
    // The refusal is a QUEUED replacement (the loop applies it, never this
    // thread): wait for the picture itself, not for the transient flag.
    bool committed = false;
    for (int i = 0; i < 200 && !committed; ++i) {
        const p::Presentation* now = eng.Runtime().ActivePresentation();
        committed = now && !now->slides.empty() && now->slides[0].title == other.title;
        if (!committed)
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    CHECK(committed);

    // Nothing queued once the session ends (and no worker left behind it).
    CHECK(live.StopLive().ok());
    CHECK(!live.IsLive());
    CHECK(!live.IsLiveContentReplacementPending());
    // REGISTRY HYGIENE: StartFromSlides mirrors "temp:Reading Pick".
    CHECK(eng.Delete("temp:Reading Pick").ok());
    CHECK(live.Stop().ok());
    (void)render.RemoveOutput(l::LiveOutputController::kPreviewName);
}