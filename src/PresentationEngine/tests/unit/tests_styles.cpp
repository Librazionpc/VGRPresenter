// Unit tests: Output styles (Settings · Styles).
// Persistence through the kernel's StyleStore + the style-aware SceneBuilder.
// Split so a single phase can run alone:
//   ./bps_unit_tests styles
#include "TestHarness.hpp"
#include "modules/presentation/BookFill.hpp"
#include "modules/presentation/CompositorState.hpp"
#include "modules/presentation/LiveOutputController.hpp"
#include "modules/presentation/PresentationEngine.hpp"
#include "modules/presentation/PresentationRuntime.hpp"
#include "modules/presentation/SceneBuilder.hpp"
#include "modules/presentation/SlideRules.hpp"
#include "modules/project/OutputStore.hpp"
#include "modules/project/ShippedConfig.hpp"
#include "modules/project/StyleStore.hpp"
#include "modules/rendering/PngCodec.hpp"
#include "modules/settings/Telemetry.hpp"
#include "platform/PlatformAccessor.hpp"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
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
// Portable background paths: a roster saved on ANOTHER machine carries an
// absolute path that cannot exist here (seeded install on a new PC). The
// re-attach pass must find this machine's own copy by file name (the
// installer seeds it into the user-data dir) and leave every other case —
// valid local path, no local copy, empty — untouched.
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
            // NOTHING NAMED A COLOUR: no background object and a KEYABLE
            // scene. The engine used to fill such a frame with an opaque black
            // of its own; an empty style is "not painting yet", so what is keyed
            // behind it shows through until something paints (a style colour, a
            // design's own backdrop, an image, pushed content). See
            // TestTransparentBackground case 2, which pins the pixels.
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
// The style background image must reach the RENDERED FRAME, not just the scene
// graph. TestStyleBackgroundImage above pins the object and its cover-fit
// bounds; the app's own "output frame sanity" diagnostic then showed every real
// output's frame all-black (meanLuma=0, maxLuma=0 over the whole stage) for
// exactly this kind of scene — which is how "the monitor shows it but HDMI/NDI
// is black" is possible at all: the tile, the output window and the library
// cards are QML layers drawn by Qt, while a real output gets ONLY this frame.
// A solid RED style image over a black style must therefore leave red pixels in
// the frame; anything else means the draw never happened.
void TestStyleBackgroundImagePixels() {
    // A real 4x4 solid-red PNG on disk (EncodePngRgba8; no hand-rolled bytes).
    // Colour packing is the engine's own 0xAABBGGRR (R in the LOW byte) —
    // 0xFF0000FF is opaque red.
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

    // BOTH SIZES: a small offscreen canvas and the REAL live one. The app's own
    // "output frame sanity" line reports a 1920x1080 frame that is entirely
    // black with maxAlpha=255 while the same scene's draw commands are
    // submitted, so the backend's large-canvas path is as much under test as
    // the builder's — a bug that only shows at the stage size the outputs
    // actually use is exactly the one an offscreen-only test would miss.
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
// THE BLANK GO-LIVE CARRIER (LiveOutputService::goLiveBlank) plus the
// compositor's own layers.
//
// GO LIVE with nothing staged sends ONE carrier slide that carries no blocks,
// no title and a "transparent" background, precisely so the outputs can show
// what the preview is already showing: the taken camera/screen input (or media
// on air) over the active style's background. The report is that this case
// renders BLACK — the media never reaches the output — while the same take IS
// visible the moment real content (scripture/table) is on air, which points at
// the scene this carrier builds rather than at the tap: both halves ride the
// same builder, so the carrier must compose its compositor layers exactly like
// a content slide does.
//
// The compositor's media hand-off is used here (CompositorState::SetMediaFrame,
// a solid GREEN 8x8) because it needs no device: it lands in the SAME object,
// on the SAME layer ("media") and therefore in the SAME pipeline pass as a
// taken camera/screen input does — so a green pixel in the frame is the whole
// question, and no PAL tap is involved.
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

    // The carrier exactly as LiveOutputService::goLiveBlank builds it: id
    // "temp:<empty name>", one slide with NO blocks, NO title, background
    // "transparent" (the variant map's own three fields).
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
        // STAGE-SHAPED (16:9), not square. A taken file is laid out with
        // contain-fit — its own aspect preserved, letterboxed on the style's
        // background (a portrait photo stays portrait; see ContainRect) — so a
        // SQUARE frame would now leave the top/bottom bands of the design
        // visible, and these assertions are about what sits OVER the feed, not
        // about the feed's own margins. A 16:9 take fills the stage exactly, so
        // their meaning is unchanged.
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

    // A carrier AND the same carrier with content on it — the two halves the
    // operator compares. Content changes nothing about the compositor layer, so
    // the media must be visible either way.
    // The full-stage slab that bury a live feed: the 754x428 design grid, so
    // 0/0/754/428 IS the whole stage (the operator's "Big Bold" variant).
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
    // ...and the design that must keep decorating OVER the feed: a lower-third
    // band (orange) with a bound line. It never reaches the stage, so it is
    // content, not background — the feed shows around it and it stays on top.
    // `meta` is the band's own block meta, which is where a design author states
    // "underVideo" instead of leaving the placement to the geometry.
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
    // THE OPERATOR'S OWN DESIGN SHAPE, and the reason the rule looks at the
    // whole leading run: a body panel plus a bottom bar that only COVER the
    // stage together (neither one of them reaches it alone). Distinct dark blue,
    // so "is this panel drawn over the feed?" is a pixel count, not a guess.
    // `asText` builds the same pair the way the app's own designs express it —
    // as TEXT items that carry the fill in their own background (see the flow:
    // a full-stage text item with an opaque box IS the design's backdrop).
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
        // THE REPORTED CASE: the style wears an engine template whose design
        // carries a full-stage background slab. On a blank carrier the slab has
        // no text to sit behind — it is pure cover — and it used to bury the
        // compositor's live video layer (an opaque black output).
        { "blank carrier + slab template", false, 1, true, false, false, false },
        // ...and it no longer buries the feed WITH content either: a design's
        // full-stage background belongs UNDER the video layer, because a live
        // feed IS the output's picture. The template's text still renders on
        // top of it.
        { "carrier + content + slab template", true, 1, true, false, false, false },
        // A design whose background is TWO leading boxes that only cover the
        // stage together: the whole run is the background, so neither panel is
        // drawn over the feed (demoting just the second one — the first fix here
        // — left the body panel painting across 85% of the picture).
        { "carrier + content + panel pair", true, 5, true, false, false, true },
        // ...and the same pair as the app's designs actually write it: TEXT items
        // whose own background box is the fill. The item's paint is the backdrop
        // (below the feed), its text still renders over the feed.
        { "carrier + content + text-item pair", true, 6, true, false, false, true },
        // The half that must NOT change: a lower-third band is the design's
        // decoration, not its background, so it stays above the video layer
        // (the feed is visible around it, and the band is drawn over it).
        { "carrier + content + band template", true, 2, true, true, false, false },
        // AN EXPLICIT "underVideo": false WINS over the geometry: the author
        // wants this slab over the feed, so it covers it (a deliberately
        // opaque design, not an accident).
        { "carrier + content + slab over-feed", true, 3, false, false, false, false },
        // ...and "underVideo": true puts a band UNDER the feed: the band is
        // part of the design's background by choice, so a live feed paints over
        // it (the mirror image of the case above).
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
            // THE FAILURE: the compositor's media is buried (the operator's "only
            // media doesn't go": a black, media-less frame) by a template slab
            // that was painted over it.
            CHECK((green > 0) == c.mediaVisible);
            if ((green > 0) != c.mediaVisible)
                std::cout << "[blank-carrier] " << c.name << ": media "
                          << (green > 0 ? "visible" : "INVISIBLE")
                          << " (green=" << green << ", red=" << red << ")\n";
            // The other half of the rule: a design's BAND is decoration and must
            // stay drawn OVER the feed (it is content, never background) — unless
            // the design itself said "underVideo": true, in which case the feed
            // paints over it and no band pixel may survive.
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
        // The style's own background IMAGE is still composed on a blank carrier
        // that wears a template — dropping the template must not cost the
        // operator's branded backdrop (it is the whole reason GO LIVE with
        // nothing paints the style rather than a bare black frame). Read with
        // the compositor's media switched off, because the media layer is
        // cover-fit and would hide the image underneath it.
        if (c.templateKind == 1 && !c.withContent) {   // the geometric slab case
            setMedia(false);
            // Re-run the builder first: the compositor's media layer is refreshed
            // through the scene cache (AddCompositorLayers on a cache hit), so
            // clearing the state alone leaves the LAST frame in the object.
            //
            // AND RENDER WHAT IT RETURNED. A take is part of the scene ID now
            // (StyledSceneIdFor's "@m:on": a file on air steps the style's
            // backdrop aside, so the two compositions cannot share one cached
            // scene). Rendering the old id here would render the take's scene —
            // the one deliberately built WITHOUT the backdrop — which is a
            // stale question, not a failing rule.
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

        // 3b. PER-OUTPUT PASSES at a NON-DEFAULT resolution: a styled live
        //     output gets its own scene built AT its configured size and its own
        //     frame buffer — and the main pass's distribute must NOT write that
        //     buffer. Two writers on one buffer is what the live report showed
        //     as an output "glitching between the two templates" (the NDI sender
        //     and the real output window both sample snapshots on their own
        //     cadence, so they caught whichever write landed last); a frame
        //     resampled up from the 1920x1080 layout was the "not following the
        //     Resolution" half.
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

        // 3c. THE TWO HALVES OF "do not pay twice for a frame that is already
        //     composed": an output whose own scene IS the composed one takes a
        //     COPY of it, and a static scene is RE-PRESENTED on the heartbeat
        //     instead of being rasterized again. Both are what stopped the live
        //     rate from falling with every enabled output (measured in the
        //     running app before this: two more outputs halved the delivered
        //     frame rate — see tools/selftest/run_live_latency_test.sh).
        {
            const std::string sharedBuf = "__out_shared_test__";
            // The SAME spec as the on-air style, and no pushed size (so the
            // gated build uses the engine's own 1080p default — the size the
            // compiled scene is built at). The scene id is the fingerprint of
            // (slide, style, size), so this lands on the COMPOSED id.
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
                // THE COPY IS THE COMPOSED FRAME: same target size, identical
                // pixels to what the preview feed received — not a re-drawn
                // scene that happens to look the same.
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

                // The dirty-frame path: with the scene unchanged, the outputs
                // keep receiving frames (the buffer's own count moves) while the
                // raster count does NOT — the heartbeat is a copy now.
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

                // --- THE MEASUREMENT THE CHANGE WAS MADE FOR -----------------
                // "Each extra enabled output halved the delivered frame rate" was
                // measured in the app BEFORE this (tools/selftest/run_live_latency_test.sh's
                // rungs: 1.43-1.45 fps on the plain ladder, 0.81 fps with two more
                // outputs enabled) and the reason was structural: for a scene that
                // had not changed, the heartbeat re-rasterised ONE FULL FRAME PER
                // OUTPUT once a second, so a fixed per-frame budget bought fewer
                // and fewer frames as outputs were enabled. The engine's own live
                // section is the AFTER number in the loop's own units, and this is
                // the whole of it in one SECOND OF STATIC SHOW: frameMs is what a
                // raster + the per-output passes cost (now ONE raster however many
                // outputs share the composed scene), representMs is what the copy
                // that replaced each of those rasters costs, and the pair
                // (composedFps, presentFps) is how many real rasters a second of
                // show needed against how often the outputs were FED.
                //
                // The sample is taken from the closed CURVE rather than from the
                // open second: LiveSnapshot's rates divide the second so far, so a
                // reading taken 20 ms into a second reports one frame as 50 fps.
                bps::settings::LiveSample quiet;
                bool measured = false;
                for (int i = 0; i < 300 && !measured; ++i) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(20));
                    const std::vector<bps::settings::LiveSample> curve =
                        bps::settings::Telemetry::Instance().LiveHistory();
                    if (curve.empty())
                        continue;
                    const bps::settings::LiveSample& last = curve.back();
                    // A second with NO raster at all is the claim: a fully static
                    // scene costs the loop nothing to draw and still feeds the
                    // outputs (and the per-output rasters became copies).
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
                // Nothing was re-drawn for that second, and the outputs were fed
                // anyway: the dirty-frame path, and the rate it delivers when the
                // scene is not moving.
                CHECK(quiet.composedFps == 0.0);
                CHECK(quiet.presentFps > 0.0);
                // The copy is the cheap half — a raster plus every per-output pass
                // costs multiples of handing the composed frame over again, which
                // is why the delivered rate no longer divides by the output count.
                CHECK(totals.frameMs > totals.representMs);
                CHECK(quiet.sharedCopies >= 1);

                // AND THE NAME BEHIND THE SHAPE. The same second also carries
                // the heaviest LAYER of the frame the loop composed (the
                // renderer's own cost profile, published by the loop), so a dip
                // in the curve is attributable instead of merely visible. The
                // whole chain is asserted here — RenderEngine's profile, the
                // controller's publish, the per-second sample — and the silent
                // failure mode it catches is the one that would leave the curve
                // shapely but nameless.
                std::fprintf(stderr,
                             "[live] hotspot: %s — %.0f%% of the frame (%.1f ms), background %.0f%%\n",
                             quiet.costElement.c_str(), quiet.costSharePct, quiet.costMs,
                             quiet.costBackgroundPct);
                CHECK(!quiet.costElement.empty());
                CHECK(quiet.costSharePct > 0.0 && quiet.costSharePct <= 100.0);
                CHECK(quiet.costMs > 0.0);
                // The ENGINE'S OWN CLEAR IS NOT THE NAME: it is a full frame of
                // pixels before any layer draws, so naming it would report
                // "Background clear" in every second of every service. Its share
                // travels alongside the named one instead (and here it is the
                // bulk of the frame — a full-frame background behind text).
                CHECK(quiet.costElement.find("Background clear") == std::string::npos);
                CHECK(quiet.costBackgroundPct > 0.0);
            }
            CHECK(eng.SetLiveOutputStyles({}, {}).ok());
            CHECK(render.RemoveOutput(sharedBuf).ok());
        }

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
// A TEXT BLOCK'S OWN BOX FILL — the "Background" the Edit canvas paints
// behind a text item (EditScreen's itemBackground target writes the item's
// style.backgroundColor) and EVERY QML surface draws (DesignPreview's
// `hasBox` Rectangle, the box the canvas shows and a template previews).
//
// AddStageBlocks turned a text block into a TextObject and NOTHING else — the
// style was carried all the way into the engine (ShowConverter.blockFromVariant
// → ContentBlock::style.backgroundColor) and then ignored, so a template whose
// text sits on a box (the shipped scripture recipe's #66000000 "transparent
// black" band is exactly this) rendered WITHOUT that box on every ENGINE
// output — the NDI feed, the distributed frames, recordings — while the in-app
// preview showed it: the reported "the template defined a transparent black bg
// behind the text but it doesn't show". A box/shape BLOCK was already honored
// (below); a text block's own box was not.
//
// Pinned at both levels: the box object exists under its text in the text's own
// layer, and the rendered pixels really are darkened inside it (a structural
// check alone would pass for a box that never rasterizes).
// ---------------------------------------------------------------------------
void TestBlockTextBox() {
    r::RenderEngine& engine = r::RenderEngine::Instance();
    // This phase may run ALONE (./bps_unit_tests block-text-box), and
    // SetBackend needs an initialized engine — the same self-boot TestGoLivePaths
    // does for the backend it renders through. Both calls are idempotent.
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

    // …and it rasterizes: inside the box the white background is dimmed to
    // ~40% black (the text's own glyphs sit in there too, hence "some pixels",
    // not "the pixel at the box centre"), while everything right of the box is
    // still untouched white.
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
// ROUNDED CORNERS on template boxes — the engine twin of Qt's
// `Rectangle { radius: … }`, which is what DesignPreview (library cards, the
// Edit canvas) and the in-app previews round a template's band/panel with.
//
// ShapeComponent::radius existed but was never written by any caller and never
// read by the rasterizer, so EVERY engine-rendered rounded box drew square:
// "Box"'s radius 8, "Trendy Curved"'s 30, the Table designs' 20 — on NDI, the
// distributed frames and recordings — while the same template looked rounded
// everywhere in the app. Geometry is authored on the 754x428 stage grid, so the
// radius scales like the width (20 stage px = ~50.9 output px at 1080p).
// Pinned at both ends: the radius reaches the shape object AND the corners
// really rasterize rounded (a corner pixel stays background, the centre is
// filled, an anti-aliased edge band exists, and radius 0 still squares off).
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
// autoSize vocabulary — the engine twin of the canvas's solver. AddStageBlocks
// previously honored ONLY literal "shrink", so every "growToFit"/"shrinkToFit"
// template (TextItemPanel's picker vocabulary) rendered at its FIXED set size
// on the engine outputs — the live "auto size only works for shows; scripture
// and the table always shrink even when the template says otherwise" report.
// Grow: a short verse in a tall box must END UP BIGGER than its set size.
// Shrink: an overflowing verse must end up smaller. None/absent: exact.
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
    // GROW PARITY (the canvas solver's `lo`): grow with an OVERFLOWING verse
    // descends to the largest size that fits — the set size is not a floor.
    // (The old engine kept the overflowing set size; every growToFit
    // template meeting a long verse rendered ~2.5× the canvas's answer and
    // clipped off the frame.) All three modes must now land on the SAME
    // fitted size — the box is the master regardless of mode.
    const float grownLong = builtSize(slideWithMode("growToFit", longVerse));
    CHECK(grownLong > 3.0f * kScale && grownLong < setOutputPx - 1.0f);
    CHECK(std::abs(grownLong - shrunk) < 0.5f);
    // CANVAS PARITY (EditScreen.recomputeFit: "EVERY mode solves, none
    // included"): a none-mode block that overflows SQUEEZES too — the box is
    // the master on every surface. (The engine used to render "none" at its
    // fixed set size and honestly overflow; that divergence WAS the "NDI
    // output renders the verse bigger than the canvas" report.)
    const float fixed = builtSize(slideWithMode("none", longVerse));
    CHECK(fixed > 3.0f * kScale && fixed < setOutputPx - 1.0f);
    CHECK(std::abs(fixed - shrunk) < 0.5f);   // none and shrinkToFit agree now
}

// ---------------------------------------------------------------------------
// FONT-METRICS FIT PARITY — "auto size works with one font, clips with
// another". The fit solver must measure the SAME vertical extent the renderer
// paints, and both must vary with the font the way real fonts vary:
//   1. lineSpacing tracks the font: SceneBuilder derives it from the font's
//      own natural line height (Qt's ProportionalHeight model), so two fonts
//      at the same set size/lineHeight produce DIFFERENT additive spacing —
//      a font-blind constant was the clip-with-one-font bug.
//   2. measured height fits where it solved: laying the fitted size out
//      through TextLayout::Measure must fit a box the solver fitted. The
//      old model counted a full line box for the LAST line too, over-
//      measuring by (step − ink) — a per-font number — so which sizes
//      "fit" swung with the font while the painted result clipped the tail
//      (second screenshot: last verse line cut at the frame bottom).
// ---------------------------------------------------------------------------
void TestFontMetricsFitParity() {
    auto& fonts = r::RenderEngine::Instance().Fonts();

    // Two families with genuinely different natural line heights if both are
    // installed (they are on every Windows box this suite runs on); with
    // neither, metrics fall back to the size+lineSpacing estimate and the
    // assertions below still hold — just without proving font-dependence.
    const char* famA = "Segoe UI";
    const char* famB = "Georgia";
    r::TextStyle st;
    st.fontId = famA;
    st.size = 100.0f;
    st.lineSpacing = 20.0f;
    st.wrap = true;
    st.wrapWidth = 900.0f;

    // (1) SceneBuilder's derivation: natural step × lineHeight − size, which
    // differs between families; the fallback formula ((lh−1)·size) is what a
    // font-blind engine used to apply to BOTH.
    const auto spacingFor = [&](const char* family) {
        r::FontManager::LineMetrics lm{};
        if (fonts.SystemLineMetrics(family, st.size, false, false, lm))
            return lm.baselineStep * 1.2f - st.size;
        return (1.2f - 1.0f) * st.size;
    };
    if (fonts.HasSystemFont(famA) && fonts.HasSystemFont(famB))
        CHECK(std::abs(spacingFor(famB) - spacingFor(famA)) > st.size * 0.01f);

    // (2) Solver contract: the fitted size measures back as fitting. Grow a
    // short verse in a tall box (the report's growToFit path), then verify —
    // and GROW MUST ACTUALLY GROW: the old proportional-step walker aborted
    // on its first overshoot and returned the SET size (the live "grow does
    // nothing until I change the font" symptom), so this also pins the
    // bisection rewrite.
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

    // Multi-line height is ink-corrected: strictly less than the naive
    // n·step (the old over-measure) whenever ink < step — true for every
    // installed font at these sizes and identity for the builtin font.
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
// LINE HEIGHT IS ONE RULE — the report "the height in the template is set to
// zero and look at the line height ... this height is too much and it makes no
// sense, let things obey the template".
//
// A block's `lineHeight` is a PROPORTIONAL multiplier of the font's own natural
// line height; 0 — and a template that never names the field — means THE FONT'S
// OWN spacing. Three surfaces draw one block (the Edit canvas, every preview
// tile, this engine) and each used to supply its own default: the Text panel
// showed 0 for the very item the canvas, the tiles and the engine all drew at
// 1.2. The value the operator could see was never the value on air. This pins
// the engine's half of the one rule (the QML half is
// MetadataFields.lineHeight / naturalLineHeight):
//   1. absent and 0 render the SAME, natural step;
//   2. that step is the font's own baseline step — NOT the old hidden 1.2;
//   3. an explicit 2.0 still scales it, so the knob keeps working.
// ---------------------------------------------------------------------------
void TestLineHeightObeysTheTemplate() {
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

    // A proportional lineHeight of `lh` on the engine side is a line STEP of
    // lh x the font's own baseline step (SceneBuilder: baselineStep x lh - size,
    // then Measure adds the size back). The fallback formula, for a font
    // without metrics, lands on the same lh x size.
    const auto expectedStep = [&](float sizePx, double lh) {
        r::FontManager::LineMetrics lm{};
        if (engine.Fonts().SystemLineMetrics("Segoe UI", sizePx, false, false, lm))
            return lm.baselineStep * static_cast<float>(lh);
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
    // TAGGED with the content family: this is the case the rule is FOR — a
    // scripture/table slide's blocks are the content tab's own template, laid
    // out for a different design, and the output style is what restyles them.
    // An UNTAGGED slide is SHOWS content, whose blocks ARE its design and must
    // survive (see TestShowSlideKeepsItsOwnDesign).
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

    // 6. RESOLUTION: an output's scene is built AT that output's own size. The
    //    styled id carries a size tag for any non-default size, so two outputs
    //    at different resolutions never share one cached scene (a cache hit
    //    returns the scene at the size it was FIRST built with — so without the
    //    tag a 4K output would be served the 1080p scene and only look resized),
    //    and the built scene really is that size, which is what makes the render
    //    backend rasterize the output's frame natively instead of resampling a
    //    1080p layout up to it (the "NDI is not following the Resolution" bug).
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
// Transparent style background — a frame the style does not paint is a keyable
// overlay whichever way it came about: an EXPLICITLY transparent template, or an
// UNSET background (the style names no colour at all). Both paint NO background
// object, flag the scene so the renderer clears to alpha 0, and really come out
// see-through in the raster — which is what the monitor tile shows as its
// checkerboard and what the output window and the NDI feed carry. They are the
// same pixels and are kept apart only by the composition that recorded them: an
// explicit "transparent" is a CHOICE, an unnamed one is simply "not painting
// yet" — both are ParseColor alpha 0, so they are told apart by the string, not
// the colour.
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
    //    object, scene flagged, and the pixels are see-through — the same
    //    frame as an explicitly transparent style, because an empty style is
    //    "not painting yet" rather than a black fill of the engine's own. What
    //    is keyed, recorded or composited behind it shows through until
    //    something paints. The SLIDE's own background is "transparent" here
    //    (the data model's default for every slide), which must NOT be mistaken
    //    for a choice — see the rule in SceneBuilder.
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
    //    TRANSPARENT STYLE. This is the reported "the lower third template never
    //    shows the checkerboard" case: a slide can OWN an opaque background the
    //    operator never chose (historically every Table slide did — the content
    //    template baked #000000 into each one; SlideBuilder no longer does, but
    //    imported/older shows still carry it). The
    //    style's "transparent" has to win — otherwise the preview tile, the
    //    output window and the NDI feed all render opaque black over what should
    //    be a keyable overlay.
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
    //    VGR_ALPHA_INSPECT): with it ON a transparent scene's clear is
    //    composited over the checkerboard, so the frame an NDI feed, an HDMI
    //    screen or the real output window shows is OPAQUE and visibly checkered
    //    instead of invisible alpha. Opaque content is left bit-identical, and
    //    the flag is GLOBAL state, so this restores it before it returns.
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

// WHAT A BLOCK RENDER CANNOT SHOW (SlideRules::NeedsDistributedFrame).
//
// The monitor tile and the output window both read ONE answer out of the
// composition to decide whether the compositor's distributed frame or their own
// block render is the honest picture for the slide on air. The rule is pinned
// here as well as in the composition's own hand-over, because the two surfaces
// used to carry a copy each — a rule with two copies is a rule that eventually
// answers two ways.
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

    // AND THE COMPOSITION'S HALF OF IT, which is what makes the answer
    // reachable for a real output: a slide carrying its own blocks (no style
    // template to override them) is composed FROM those blocks, so the
    // preview's answer and the frame's content come from the same list.
    p::Slide slide;
    slide.id = "frame-rule-1";
    slide.blocks = { text, camera };
    p::OutputStyleSpec unset;   // no template key, no family picks
    const auto own = p::SceneBuilder::ComposeSlide(slide, unset, /*applyGates=*/true);
    CHECK(own.source == p::SceneBuilder::SlideComposition::Source::SlideBlocks);
    CHECK(p::SlideRules::NeedsDistributedFrame(slide.blocks));

    // A style's TEMPLATE owns the layout instead: what is composed is the
    // template's own blocks (its text regions), so the frame is not wanted —
    // the blocks are the picture, and they cannot lag a slide change. A
    // CONTENT-FAMILY slide is the case (a scripture pick restyled by an output
    // style): a show's own authored blocks keep their layout, see
    // TestShowSlideKeepsItsOwnDesign.
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
//
// The reported shape: a table design's slot carried {table_code_paragraphs} (and
// a body of {table_number} {table_text}) with NO bind — that is how the metadata
// palette inserts a field. The preview pane showed the sentence (it fills through
// BookFill), while the outputs drew the box with no text at all: BindTemplateBlocks
// blanked every non-`bind` block that still held a `{`. This pins both halves of
// the fix — resolution through the shared vocabulary, and the deliberate blank for
// a token whose value never reaches the engine.
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
//
// The reported shape: a table design built from the metadata palette — a date
// code in its top corner, the sermon body under it — rendered its CODE where the
// body belonged on the outputs, at the body's own size and colour ("that is
// table code what was defined in the template is table text").
//
// How: a table slide goes live with NO `text` of its own (SlideBuilder::slides
// never fills line1/line2, so ShowConverter's slideFromVariant leaves it empty)
// and its body riding in the DESIGN's blocks, which BookFill has already filled
// — code slot "47-0412 4", body slot the sentence. ContentText's fallback then
// took "the first text-bearing block", i.e. whichever slot the author placed
// first, which for a design with a code in its corner IS the code. Every consumer
// of "the body" inherited it: the bind="text" block, {table_text}, and
// {scripture_text} on a design whose first slot is the verse number.
//
// The design NAMES its body — the palette's Text field and the shipped layouts'
// Text() both carry bind "text" (MetadataFields) — so that is what ContentText
// reads first, and the code slot stays exactly where the author put it.
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
    //    on either slot (the palette of the day inserted bare tokens), so nothing
    //    in the block list can name the body — which is why the fill CARRIES it
    //    (BookSlide::body, put on the slide by SlideBuilder and read by
    //    ShowConverter) instead of leaving the engine to pick by position.
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

        // What the slide carries once the services have built it (the fill's
        // `body` becomes the slide's own text) — the code can no longer be
        // mistaken for the content, whatever order the design put its slots in.
        p::Slide carried = slide;
        carried.blocks = filledSlides[0].blocks;
        carried.text = filledSlides[0].body;
        CHECK(p::SlideRules::ContentText(carried) == filledSlides[0].body);

        // 5. THE REST OF THE VOCABULARY rides the same way. "It just remains the
        //    code" was the report: the design's corner slot asks for
        //    {table_code_paragraphs} — the date code AND this slide's paragraph
        //    numbers — and the engine can only answer it from the slide, because
        //    a filled block no longer says what it used to ask for.
        const p::BookValues &v = filledSlides[0].values;
        CHECK(v.code == "47-0412");
        CHECK(v.numbers == "3");
        CHECK(p::FillTokens("{table_code_paragraphs}", v) == "47-0412 3");
        CHECK(p::FillTokens("{table_reference}", v) == "47-0412 - Faith Is The Substance 3");

        // ...and it renders: the meta the services now write (SlideBuilder's
        // `fill` map -> ShowConverter's metaJson) is what SlideValues reads, so
        // the SAME slide with that meta draws the code in the corner slot.
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
            // `refLast` is PRESENT and empty here: the fill saying "this is not
            // the last slide". It must stay blank — falling back to the slide's
            // own reference put the whole selection on every slide.
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
// A LAYOUT WITH NOTHING TO DRAW DOES NOT GET TO SEND A BLANK FRAME.
//
// Reported live: the NDI output read "sending - 2205 frames - 2 monitor(s)"
// while the receiving monitor stayed black, and — from the operator — "it did
// not even show the background image before you even get to slides". The engine
// frame the feed carried was EMPTY (the presenter's own sanity line: maxAlpha=0,
// not one object painted anything) while the app's tiles showed the sermon,
// because the tiles draw the slide's own blocks and the style's family template
// had claimed the composition. That template is a TABLE design whose slots are
// palette FIELD tokens ({table_text}, {table_code_paragraphs}) and the slide
// that went on air carried its content in its OWN blocks, so every token bound
// to nothing: a layout of empty text, under a transparent style whose
// clearBackgroundOnText had hidden the style image, is a fully see-through
// frame. Squarely the rule this file exists to keep: the engine must not render
// a picture the preview already shows content in.
//
// The rule the suite pins: a template or preset that would paint NOTHING hands
// the composition back to the slide's own blocks (the same picture the preview
// draws), while a template that DOES paint keeps FreeShow's precedence — output
// style > slide — exactly as before.
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

    // The slide the Table tab puts on air: its content IS its own blocks (the
    // fill put the sermon there) and it carries no title/body of its own — the
    // exact shape a template's field slots and a preset's title/body cannot draw.
    p::Slide tableSlide;
    tableSlide.id = "blank-tpl-1";
    tableSlide.metaJson = R"({"contentType":"table"})";
    tableSlide.blocks = {
        textBlock("b1", "Faith is the substance of things hoped for...", 60),
    };

    // The style an NDI row wears: a transparent background, the branded image
    // stepping aside for content, and a TABLE family template made of the
    // palette's field slots (id values the slide was never given).
    p::OutputStyleSpec style;
    style.name = "NDI screen";
    style.backgroundColor = "transparent";
    style.clearBackgroundOnText = true;
    style.backgroundImage = "";   // no disk dependency in a unit test
    style.familyTemplateKeys[p::SlideRules::FamilyTemplateSlot("table")] = "tpl-3";
    style.familyTemplateBlocks[p::SlideRules::FamilyTemplateSlot("table")] = {
        // The slots a palette design carries: field tokens whose values the
        // slide was never given (the slide's own block holds the sermon, and
        // SlideValues has no `table_notes` to hand this slot), so both bind to
        // nothing and the layout is a line-up of empty text.
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
    //    slide's own block is a real object in it AND the raster is not a blank
    //    frame. This is the assertion the black monitor failed — before the
    //    fallback the same scene rasterized to maxAlpha=0.
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
// CLEAR ALL REALLY CLEARS — the report "the clear all doesn't clear well: it
// shows the style bg image ... the bg brings back the background image and
// clears the slide".
//
// Clear all used to send the SAME carrier GO LIVE-with-nothing sends, and the
// engine composites the output's style onto an empty slide exactly as it does
// onto a full one — which is deliberate there (going live with nothing is meant
// to show the output's own look) and wrong here: pressing Clear all took the
// slide down and put the style's poster back up. The clear is now its own
// carrier (Slide::clearStage) composed to paint NOTHING.
//
// Both halves are asserted, because only the pair says anything: the blank
// carrier must KEEP the style (nothing here may regress the GO LIVE promise),
// and the clear must take every last bit of it down — including at the pixel
// level, which is what the operator actually sees on the screens.
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

    // DISTINCT IDs, the way the live path names them: SceneBuilder keys scenes
    // by "pres:<presentation>:<slide id>", so two slides with the same id share
    // one cached scene and the second build would be served the first one
    // (StartFromSlides does exactly this naming, folding the slide's content
    // into the id — see HashSlideContent, which is where the flag has to be too).
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
    // ...AND THAT IS THE ONE ANSWER TO "IS THIS FRAME TRANSPARENT?" (see
    // SlideComposition::frameTransparent): a clear is transparency by
    // definition, while the blank GO LIVE carrier beside it wears the style's
    // own opaque poster.
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
                // THE FIX, in the operator's own terms: not one pixel of the
                // style's colour, its background image or its design survives —
                // the frame is the empty one a keyer/black screen gets.
                CHECK(countNonBlack(frame.value()) == 0);
                CHECK(countVisible(frame.value()) == 0);
            }
            (void)render.DestroyScene(builtClear.value());
        }

        // 2b. "NOTHING" INCLUDES THE COMPOSITOR'S OWN LAYERS.
        //
        // The wall's Blackout control sends this same carrier, and the report
        // it exists for is "nothing at all on screen". The engine used to
        // composite a taken camera/screen, a media file and the overlay stack
        // INTO every scene before the content switch — the clear included — so
        // a blackout mid-service still showed the camera. The clear carrier now
        // owns no compositor layers; the state itself is untouched, which is
        // what lets the blackout be lifted with the feed and overlays back.
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
    //
    // Clear all reaches the engine as StartFromSlides(name, {carrier}), and the
    // slide id those pushed slides get is a CONTENT fingerprint: an empty clear
    // carrier and an empty blank carrier fingerprint identically unless the flag
    // is part of the hash. SceneBuilder caches scenes by that id, so a collision
    // means the clear is SERVED THE BLANK CARRIER'S SCENE — the style painted
    // again, no matter how right the composition is. Nothing else can catch
    // this: the composition is correct in both runs.
    auto &eng = p::PresentationEngine::Instance();
    CHECK(eng.Initialize().ok());
    auto &live = l::LiveOutputController::Instance();
    CHECK(live.Initialize().ok());
    CHECK(live.Start().ok());
    {
        // Both pushes under the SAME name — the way the wall does it (the
        // "temp:<name>" presentation, a fresh carrier each press). STOPPED
        // between them because a live REPLACE is applied by the render worker
        // (LiveOutputController::StartFromSlides queues it), so reading the
        // compiled set straight after the second push would read the FIRST
        // one's scenes and assert nothing.
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
//
// The reported shape: a show authored on the Edit canvas — a title item, a text
// band, a shape — showed completely in the monitor preview and only partly on
// the real outputs ("scripture and The Table work, shows do not"): the NDI feed
// and HDMI window drew the output style's whole-style template instead, which
// binds the slide's BODY into the style's own slots (the hymn lines, the
// operator's title gone, the band resized or squeezed small), while the tile and
// canvas — which draw the slide's blocks — showed the whole design.
//
// Why only shows: a scripture/table slide's blocks are a DESIGN's field slots,
// so a style template restyling them is the feature. A show's blocks ARE the
// design, and the style's template (style.contentType empty, so not keyed to any
// family) was claiming them as well. ComposeSlide now keeps a shows-family
// slide's own blocks; the style still paints the background, and a style that
// explicitly keys shows keeps the old precedence.
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
    //    "Template for Shows" → Change) DOES lay the show out — that pick is an
    //    explicit request, and the picker promises it ("the engine renders that
    //    design as the style's layout"). The report behind this assertion:
    //    "I chose Lower Third for style 2 and it does the opposite" — the pick
    //    was being ignored on every authored show, so choosing a lower third
    //    left the full-frame canvas layout on air.
    //
    //    Still NOT a licence for the WHOLE-STYLE template to do the same
    //    (assertion 1: that one names no family and must leave an authored show
    //    alone), and a style that merely NAMES "shows" as its content type is
    //    not a pick either (assertion 3c).
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

    // 3c. ...while a style that only NAMES "shows" as its own content type —
    //     both of the operator's styles do — is not a pick: the authored show
    //     still keeps its layout. This is the shape the picker has to beat.
    p::OutputStyleSpec namedShowsStyle = style;
    namedShowsStyle.contentType = "shows";
    const auto namedShowsComp =
        p::SceneBuilder::ComposeSlide(showSlide, namedShowsStyle, /*applyGates=*/true);
    CHECK(namedShowsComp.source == p::SceneBuilder::SlideComposition::Source::SlideBlocks);
    CHECK(namedShowsComp.blocks.empty());

    // 3d. BAKED BLOCKS ARE NOT A PICK. The front-end's shows fallback bakes the
    //     Scripture design into an UNPICKED shows slot (keys stay ""); both of
    //     the operator's styles carry exactly that, so "has blocks" would send
    //     every authored show to a scripture layout they never chose — the same
    //     "hymn lines, no title, no authored band" fault in a new coat. Only a
    //     KEY names a choice.
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

    // 4b. THE SAME RULE, ASKED AS A QUESTION Settings · Outputs PRINTS. The row
    //     under a style's name says what that style does to a slide, and it is
    //     this call — so the sentence on the row and the picture on air cannot
    //     drift apart.
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
        // Naming "shows" as the style's own type is not a layout request (a
        // new style defaults to it) — the row must not claim it lays shows
        // out when ComposeSlide keeps the authored show.
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
// The Edit canvas draws a bus's feed inside its rectangle; before this the
// outputs drew NOTHING there — a "bus" block fell through AddStageBlocks' kind
// switch (text / box / shape, everything else ignored), so NDI and HDMI showed
// an empty hole where the operator's canvas showed video. This pins the three
// things that make it a WINDOW rather than a placeholder:
//   * the plane is painted at the BLOCK's own rect, not full-frame (the taken
//     input and media layers are full-frame; a bus block's geometry is the
//     design's);
//   * a new frame reaches it through a CACHE HIT — a scene that is already
//     built must not freeze on the first frame it ever saw;
//   * a bus whose feed stops goes INVISIBLE (not frozen), and a bus block that
//     names no bus paints nothing at all.
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
    // A caption: the plane lives on the compositor's video layer, UNDER the
    // slide's text, which is exactly the layering a lower-third over a feed
    // relies on.
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

            // A NEW FRAME THROUGH A CACHE HIT. The second build is the same
            // sceneId (same slide + style), so nothing is rebuilt — and the bus
            // must STILL pick up the newer frame, or a live feed freezes the
            // moment its slide first goes on air.
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
                }
            }

            // A BUS WHOSE FEED STOPPED GOES INVISIBLE, not frozen on its last
            // picture (the renderer skips a VideoObject with no frame).
            compositor.ClearBusFrames();
            CHECK(!compositor.HasBusFrames());
            (void)builder.BuildSlideScene(pres, slide, style, engine);
            auto blanked = engine.GetObject(built.value(), "bus:busA");
            CHECK(blanked.ok());
            if (blanked.ok()) {
                auto* v = dynamic_cast<r::VideoObject*>(blanked.value());
                CHECK(v != nullptr);
                if (v)
                    CHECK(!v->HasFrame());
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
// ONE PICTURE, ONE SHAPE: the FILE's own fill decides, else contain — and a take
// clears the style's background.
//
// The operator's ask, in two halves:
//   * "per-media fill override (Contain/Cover/Fill on the file itself)" — the
//     choice belongs to the PICTURE, so a portrait photo marked "cover" crops
//     on every output, and one nobody has said anything about is CONTAIN
//     everywhere. The output's own (Settings · Outputs) value no longer decides
//     a take at all: two outputs left on different values showed one file two
//     ways on the monitor wall, for the same slide, at the same moment;
//   * "the style already said shows should clear bg — anything that wants to
//     show removes the style's bg" — a take is the frame, not a guest inside the
//     style's poster, so the style's colour (and image) must not survive under
//     it in the letterbox margins.
//
// Both are read off the real scene the outputs render: `mediaonair`'s bounds say
// which fit was applied, `mediabars` says whether the contained picture needed
// the blown-up bar fill, and the absence of `bg` says the style cleared.
// ---------------------------------------------------------------------------
void TestMediaFitOnTheFileWins() {
    // Self-contained: the suite is reachable on its own (its name contains
    // "media", so `./bps_unit_tests media` runs it), and a render engine that
    // was never booted refuses SetBackend.
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
        // Opaque everywhere — the alpha flag the take carries (see
        // MediaFrame::hasAlpha) says so, exactly as LiveOutputService's scan of a
        // decoded JPEG does.
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
    //    its own aspect kept (a narrow band down the middle, empty space either
    //    side). NOT the output's fill: this style says "contain" here and the
    //    check below says a style saying "cover" answers the same way.
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
        // THE MARGINS STAY EMPTY: no blown-up copy of the picture under it (the
        // operator's "if the image is 16:1 I expect the transparent bg around
        // it, that is EMPTY SPACE").
        CHECK(!engine.GetObject(sceneId, "mediabars").ok());
        // THE TAKE CLEARS THE STYLE: no background object of the style's, and the
        // composition says so (the preview surfaces read this to clear too).
        CHECK(!engine.GetObject(sceneId, "bg").ok());
        const auto comp = p::SceneBuilder::ComposeSlide(slide, style);
        CHECK(comp.clearedByTake);
        CHECK(!comp.paintBackground);
        CHECK(comp.background.empty());
        CHECK(comp.styleImageSuppressed);
        // AND IT IS NOT A TRANSPARENT FRAME: an opaque picture on air replaced
        // the background, so the frame is opaque where the picture is — the
        // margins a contain-fit leaves are the OUTPUT's empty pixels, never the
        // picture's alpha. This is the one answer every surface draws its
        // transparency convention from (the monitor wall used to read the take
        // itself and painted checkered pillarbox bars no output had).
        CHECK(!comp.frameTransparent);
        CHECK(!compositor.MediaFrameIsTransparent());
    }

    // 1b. A PICTURE THAT REALLY CARRIES TRANSPARENCY is a transparent frame —
    //     the convention exists for exactly this ("the checkerboard signifies
    //     transparent"), and it must survive the rule above.
    takeWithAlpha();
    {
        CHECK(compositor.MediaFrameIsTransparent());
        const auto comp = p::SceneBuilder::ComposeSlide(slide, style);
        CHECK(comp.clearedByTake);
        CHECK(comp.frameTransparent);
    }

    // 2. THE FILE'S OWN FIT WINS: "cover" on the file crops it to the stage even
    //    though this output says "contain". Same scene id (a fit change is a
    //    LAYOUT change, not a new composition), refreshed through the cache hit —
    //    which is exactly how a pick while the file is on air reaches the output.
    takeWithFit("cover");
    auto again = builder.BuildSlideScene(pres, slide, style, engine);
    CHECK(again.ok());
    if (again.ok()) {
        CHECK(again.value() == sceneId);
        const r::Rect rect = mediaBounds(sceneId, "mediaonair");
        // COVER preserves the picture's aspect and CROPS: a 100x200 take on a
        // 1920x1080 stage covers it at 1920x3840 (the stage is a window onto the
        // middle of the picture), not stretched to the stage.
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

    // 3b. AND THE OUTPUT'S FILL DECIDES NOTHING — with the style asking for
    //     "cover" and the file naming none, the take is CONTAIN (the whole
    //     picture). This is the promise that makes every output track the one
    //     picture instead of cropping on the screens whose Fill said so, so it is
    //     pinned here rather than left to the style push to honour.
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
        // A STYLE THAT PAINTS ITS OWN COLOUR IS NOT A TRANSPARENT FRAME — the
        // other half of the one answer: with the take gone, what the operator
        // sees is the style's poster, not transparency.
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
// An install that had already run kept its styles and outputs forever: the
// installer only copies seed files that do not exist. This merges
// <install>/assets/kernel.shipped.json once per CONTENT FINGERPRINT — shipped
// entries are refreshed in place or appended, the user's own entries keep their
// place, and nothing is ever removed. A graph is per-machine and is not merged.
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
