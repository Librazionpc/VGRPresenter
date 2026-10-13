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
#include "modules/project/StyleStore.hpp"
#include "modules/rendering/PngCodec.hpp"
#include "platform/PlatformAccessor.hpp"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <thread>

#include <cstdint>
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
        p::CompositorState::MediaFrame frame;
        frame.width = 8;
        frame.height = 8;
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
            (void)builder.BuildSlideScene(pres, slide, caseStyle, render, r::Size(1920, 1080));
            auto plain = render.Render(built.value(), opts);
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
    // the blocks are the picture, and they cannot lag a slide change.
    p::ContentBlock bound = text;
    bound.bind = "text";
    p::OutputStyleSpec templated;
    templated.familyTemplateBlocks[0] = { bound };
    const auto owned = p::SceneBuilder::ComposeSlide(slide, templated, /*applyGates=*/true);
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
