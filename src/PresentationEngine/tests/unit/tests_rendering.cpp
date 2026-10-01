// Unit tests: Rendering Engine (docs/specs/17).
// Split from tests/unit/main.cpp so a single phase can run alone:
//   ./bps_unit_tests rendering
#include "TestHarness.hpp"
#include "modules/rendering/PngCodec.hpp"

#include <fstream>

void TestRenderTypes() {
    // Color pack/unpack round trip (0xAABBGGRR native little-endian).
    r::Color c(0.5f, 0.25f, 1.0f, 0.75f);
    uint32_t px = c.Pack();
    r::Color back = r::Color::Unpack(px);
    CHECK(std::abs(back.r - 0.5f) < 0.01f);
    CHECK(std::abs(back.g - 0.25f) < 0.01f);
    CHECK(std::abs(back.b - 1.0f) < 0.01f);
    CHECK(std::abs(back.a - 0.75f) < 0.01f);
    CHECK(r::Color::Red().Pack() == 0xFF0000FFu);
    CHECK(r::Color::Green().Pack() == 0xFF00FF00u);
    CHECK(r::Color::Blue().Pack() == 0xFFFF0000u);
    CHECK(r::Color::White().Pack() == 0xFFFFFFFFu);
    CHECK(r::Color::Unpack(r::Color::Red().Pack()).r > 0.9f);
    CHECK(c.WithAlpha(1.0f).a == 1.0f);

    // Rect helpers.
    r::Rect rc(10, 20, 100, 50);
    CHECK(rc.Right() == 110.0f && rc.Bottom() == 70.0f);
    CHECK(rc.Contains(10, 20) && rc.Contains(109, 69));
    CHECK(!rc.Contains(110, 70) && !rc.Contains(5, 5));

    // Vec2 / Size / Transform.
    r::Vec2 v(3, 4);
    CHECK(v.x == 3.0f && v.y == 4.0f);
    r::Size s(640, 480);
    CHECK(s.width == 640.0f && s.height == 480.0f);
    r::Transform t(r::Vec2(5, 6));
    CHECK(t.position.x == 5.0f && t.scale.x == 1.0f);

    // Shape kind + blend enum sanity.
    CHECK(static_cast<int>(r::ShapeKind::Rectangle) == 0);
    CHECK(static_cast<int>(r::BlendMode::Alpha) == 0);
}
void TestRenderBackends() {
    // --- Software backend: framebuffer + raster ops ---
    r::SoftwareGraphicsBackend sw;
    CHECK(sw.Capabilities().name == "software");
    CHECK(sw.BeginFrame(4, 4).ok());
    r::DrawCommand fill;
    fill.type = r::DrawCommand::Type::FillRect;
    fill.rect = r::Rect(1, 1, 2, 2);
    fill.color = r::Color::Red();
    CHECK(sw.Submit({fill}).ok());
    auto fb = sw.Readback();
    CHECK(fb.ok() && fb.value().width == 4 && fb.value().pixels.size() == 16);
    if (fb.ok()) {
        CHECK(r::Color::Unpack(fb.value().pixels[1 * 4 + 1]).r > 0.9f);   // red fill
        CHECK(r::Color::Unpack(fb.value().pixels[0]).r < 0.1f);           // untouched black
        CHECK(r::Color::Unpack(fb.value().pixels[3 * 4 + 3]).r < 0.1f);
    }

    // Blend modes: 50% white over black → ~128.
    uint32_t out = r::SoftwareGraphicsBackend::BlendPixel(
        r::Color::Black().Pack(), r::Color(1, 1, 1, 0.5f).Pack(), r::BlendMode::Alpha);
    r::Color blended = r::Color::Unpack(out);
    CHECK(blended.r > 0.4f && blended.r < 0.6f);
    CHECK(blended.a > 0.9f);
    // Multiply: white * gray stays gray.
    out = r::SoftwareGraphicsBackend::BlendPixel(
        r::Color(0.5f, 0.5f, 0.5f, 1).Pack(), r::Color::White().Pack(), r::BlendMode::Multiply);
    CHECK(r::Color::Unpack(out).r > 0.45f && r::Color::Unpack(out).r < 0.55f);
    // Additive: black + red → red.
    out = r::SoftwareGraphicsBackend::BlendPixel(
        r::Color::Black().Pack(), r::Color::Red().Pack(), r::BlendMode::Additive);
    CHECK(r::Color::Unpack(out).r > 0.9f);

    // Pixel effects: brightness (gray 0.5 × 2 → white).
    r::RgbaImage img;
    img.width = 2;
    img.height = 1;
    img.pixels = {r::Color(0.5f, 0.5f, 0.5f, 1).Pack(),
                  r::Color(0.5f, 0.5f, 0.5f, 1).Pack()};
    r::SoftwareGraphicsBackend::ApplyEffect(img, r::Rect(0, 0, 2, 1), 4, 2.0f);
    CHECK(r::Color::Unpack(img.pixels[0]).r > 0.98f);
    // Contrast 1.0 is identity; saturation 0 → grayscale.
    r::RgbaImage colored;
    colored.width = 1;
    colored.height = 1;
    colored.pixels = {r::Color(1.0f, 0.0f, 0.0f, 1).Pack()};
    r::SoftwareGraphicsBackend::ApplyEffect(colored, r::Rect(0, 0, 1, 1), 6, 0.0f);
    r::Color gray = r::Color::Unpack(colored.pixels[0]);
    CHECK(gray.r > 0.1f && gray.r < 0.4f);   // red → mid gray
    CHECK(std::abs(gray.r - gray.g) < 0.05f && std::abs(gray.r - gray.b) < 0.05f);
    // Opacity effect halves alpha.
    r::RgbaImage opaque;
    opaque.width = 1;
    opaque.height = 1;
    opaque.pixels = {r::Color::White().Pack()};
    r::SoftwareGraphicsBackend::ApplyEffect(opaque, r::Rect(0, 0, 1, 1), 3, 0.5f);
    CHECK(r::Color::Unpack(opaque.pixels[0]).a > 0.4f &&
          r::Color::Unpack(opaque.pixels[0]).a < 0.6f);

    // Textures + shaders on the software backend.
    r::RgbaImage tex;
    tex.width = 2;
    tex.height = 2;
    tex.pixels = {0, 0, 0, 0};
    auto tid = sw.CreateTexture(tex);
    CHECK(tid.ok() && tid.value().valid());
    auto read = sw.ReadTexture(tid.value());
    CHECK(read.width == 2 && read.height == 2);
    auto sh = sw.CreateShader("test", "#version 100\nvoid main(){}\n");
    CHECK(sh.ok() && sh.value() > 0);
    CHECK(sw.ShaderCount() == 1);
    CHECK(sw.DestroyShader(sh.value()).ok());
    CHECK(sw.ShaderCount() == 0);
    CHECK(sw.DestroyTexture(tid.value()).ok());
    CHECK(sw.Shutdown().ok());

    // --- Null backend: headless accounting ---
    r::NullGraphicsBackend nb;
    CHECK(nb.Capabilities().name == "null");
    CHECK(nb.BeginFrame(2, 2).ok());
    CHECK(nb.Submit({fill}).ok());
    CHECK(nb.SubmitCount() == 1);
    auto nfb = nb.Readback();
    CHECK(nfb.ok() && nfb.value().pixels.size() == 4);
    auto nt = nb.CreateTexture(tex);
    CHECK(nt.ok() && nt.value().valid());
    CHECK(nb.LiveTextures() == 1);
    CHECK(nb.DestroyTexture(nt.value()).ok());
    CHECK(nb.LiveTextures() == 0);
    CHECK(nb.Shutdown().ok());
}
void TestRenderSceneGraph() {
    // Scene + layers.
    r::Scene sc("s", "Scene", r::Size(100, 100));
    CHECK(sc.SceneSize().width == 100.0f);
    CHECK(sc.AddLayer(r::Layer("bg", "Background", r::LayerKind::Background, 5)).ok());
    CHECK(sc.AddLayer(r::Layer("txt", "Text", r::LayerKind::Text, 1)).ok());
    CHECK(!sc.AddLayer(r::Layer("bg", "Dup", r::LayerKind::Image, 0)).ok());   // duplicate id
    CHECK(sc.LayerCount() == 2);
    auto sorted = sc.LayersSorted();
    CHECK(sorted.size() == 2 && sorted[0].id == "txt" && sorted[1].id == "bg");  // by order
    CHECK(sc.FindLayer("bg") != nullptr && sc.FindLayer("nope") == nullptr);
    sc.SetLayerEnabled("bg", false);
    for (const auto& l : sc.LayersSorted())
        if (l.id == "bg") CHECK(!l.enabled);
    CHECK(sc.RemoveLayer("bg").ok());
    CHECK(!sc.RemoveLayer("bg").ok());
    CHECK(sc.LayerCount() == 1);

    // Scene graph hierarchy + world transforms.
    auto* root = sc.Root();
    CHECK(root != nullptr);
    auto child = std::make_shared<r::SceneNode>("c", "Child");
    CHECK(root->AddChild(child) == child.get());
    CHECK(child->Parent() == root);
    CHECK(sc.FindNode("c") == child.get());
    child->Local().position = r::Vec2(10, 20);
    r::Transform w = child->World();
    CHECK(w.position.x == 10.0f && w.position.y == 20.0f);

    // Nested: parent scaled ×2, child offset (5,5) → world (20,30).
    child->Local().scale = r::Vec2(2.0f, 2.0f);
    auto grand = std::make_shared<r::SceneNode>("g", "Grand");
    child->AddChild(grand);
    grand->Local().position = r::Vec2(5, 5);
    r::Transform gw = grand->World();
    CHECK(gw.position.x == 20.0f && gw.position.y == 30.0f);
    CHECK(gw.scale.x == 2.0f);
    CHECK(sc.FindNode("g") == grand.get());

    // Components attach to nodes.
    struct Tag : r::Component {
        const char* TypeName() const noexcept override { return "Tag"; }
    };
    grand->AddComponent(std::make_shared<Tag>());
    CHECK(grand->FindComponent<Tag>() != nullptr);
    CHECK(grand->Components().size() == 1);
    grand->SetVisible(false);
    CHECK(!grand->Visible());
    grand->SetOpacity(0.5f);
    CHECK(grand->Opacity() == 0.5f);

    // Removal.
    CHECK(child->RemoveChild("g"));
    CHECK(sc.FindNode("g") == nullptr);
    CHECK(child->Children().empty());

    // Camera defaults.
    r::Camera cam(r::Size(1920, 1080));
    CHECK(cam.viewport.width == 1920.0f && cam.zoom == 1.0f);
    CHECK(cam.safeArea.width == 1920.0f);

    // SceneGraph facade.
    r::SceneGraph graph;
    CHECK(graph.CreateScene("a", "A", r::Size(1, 1)).ok());
    CHECK(!graph.CreateScene("a", "B", r::Size(1, 1)).ok());   // duplicate id
    CHECK(graph.Count() == 1);
    CHECK(graph.GetScene("a").ok());
    CHECK(graph.DestroyScene("a").ok());
    CHECK(!graph.GetScene("a").ok());
    CHECK(graph.Count() == 0);
}
void TestRenderObjects() {
    // Text object.
    auto text = std::make_shared<r::TextObject>("t", "Title", "Hello");
    CHECK(text->Kind() == r::ObjectKind::Text);
    CHECK(text->TypeName() == std::string("Text"));
    CHECK(text->Text() == "Hello");
    text->SetText("World");
    CHECK(text->Text() == "World");

    // Image object + source component.
    auto img = std::make_shared<r::ImageObject>("i", "Image");
    CHECK(img->Kind() == r::ObjectKind::Image);
    r::RgbaImage im;
    im.width = 2;
    im.height = 2;
    im.pixels = {0, 0, 0, 0};
    img->SetImage(im, "asset-1");
    CHECK(img->Source() != nullptr && img->Source()->source.loaded);
    CHECK(img->Source()->source.assetId == "asset-1");
    CHECK(img->Source()->source.width == 2);

    // Video object frame push.
    auto vid = std::make_shared<r::VideoObject>("v", "Video");
    CHECK(!vid->HasFrame());
    vid->SetFrame(im);
    CHECK(vid->HasFrame() && vid->Frame().width == 2);

    // Shape object.
    auto shape = std::make_shared<r::ShapeObject>("sh", "Shape");
    shape->SetRectangle(r::Color::Blue());
    CHECK(shape->Shape() != nullptr && shape->Shape()->kind == r::ShapeKind::Rectangle);
    CHECK(shape->Shape()->fill.b > 0.9f);

    // Background object.
    auto bg = std::make_shared<r::BackgroundObject>("b", "Background", r::Color::Red());
    CHECK(bg->BackgroundColor().r > 0.9f);
    bg->SetBackgroundColor(r::Color::Green());
    CHECK(bg->BackgroundColor().g > 0.9f);

    // Gradient object.
    auto grad = std::make_shared<r::GradientObject>("gr", "Gradient");
    grad->SetVertical(r::Color::White(), r::Color::Black());
    CHECK(grad->Gradient() != nullptr && grad->Gradient()->vertical);
    CHECK(grad->Gradient()->bottom.b < 0.1f);

    // Overlay object.
    auto ov = std::make_shared<r::OverlayObject>("o", "Logo", "logo.png");
    CHECK(ov->AssetId() == "logo.png");

    // Countdown formatting.
    const int64_t now = 1'000'000;
    auto cd = std::make_shared<r::CountdownObject>("cd", "Countdown", now + 90'000);
    CHECK(cd->Countdown() != nullptr);
    CHECK(cd->FormatRemaining(now) == "01:30");
    cd->Countdown()->format = "HH:MM:SS";
    CHECK(cd->FormatRemaining(now) == "00:01:30");
    CHECK(cd->FormatRemaining(now + 200'000) == "00:00:00");   // expired → 0

    // Clock object.
    auto clk = std::make_shared<r::ClockObject>("ck", "Clock");
    CHECK(clk->Clock() != nullptr);

    // Common render-object properties.
    bg->SetBounds(r::Rect(0, 0, 100, 100));
    CHECK(bg->Bounds().width == 100.0f);
    bg->SetTint(r::Color(1, 1, 1, 0.5f));
    CHECK(bg->Tint().a == 0.5f);
    bg->SetOpacity(0.5f);
    CHECK(bg->EffectiveAlpha() == 0.25f);   // opacity × tint alpha
    bg->SetVisible(false);
    CHECK(!bg->Visible());
    bg->SetRotationRad(0.5f);
    CHECK(bg->RotationRad() == 0.5f);
    bg->SetLayer("L1");
    CHECK(bg->LayerId() == "L1");
}
void TestRenderText() {
    // Font manager: builtin font + fallback + glyph metrics.
    r::FontManager fm;
    fm.RegisterBuiltin();
    CHECK(fm.Count() >= 1);
    auto f = fm.GetFont("builtin");
    CHECK(f.ok());
    CHECK(fm.Resolve("missing-font").id == "builtin");   // falls back
    CHECK(!fm.FontIds().empty());
    auto g = fm.Glyph('A', 24);
    CHECK(g.width > 0 && g.advance > 0);
    CHECK(fm.Glyph(127, 24).codepoint == '?');   // out-of-range → '?'

    // Atlas building.
    std::map<uint8_t, r::FontGlyph> glyphs;
    auto atlas = fm.BuildAtlas(24, glyphs);
    CHECK(!atlas.empty() && atlas.width > 0 && atlas.height > 0);
    CHECK(!glyphs.empty());
    CHECK(glyphs.count(static_cast<uint8_t>('A')) == 1);

    // Layout: measure + wrap + split.
    r::TextStyle style;
    auto res = r::TextLayout::Measure("Hello world", style, fm);
    CHECK(res.lines.size() == 1);
    CHECK(res.totalWidth > 0.0f && res.totalHeight > 0.0f);
    auto lines = r::TextLayout::SplitLines("a\nb\nc", false);
    CHECK(lines.size() == 3);
    r::TextStyle wrap;
    wrap.wrap = true;
    wrap.wrapWidth = 40.0f;
    auto wrapped = r::TextLayout::Measure("abcdefghij", wrap, fm);
    CHECK(wrapped.lines.size() >= 2);   // long string wraps

    // Unicode punctuation folds to the ASCII the glyph atlases actually
    // contain (regression: U+2019 reached the byte-wise draw loop as UTF-8
    // E2 80 99 — three atlas misses, three 0.6×size blank gaps, live symptom
    // "that     s when" for "that's" on engine-rendered output).
    auto apostrophed = r::TextLayout::SplitLines("that\xE2\x80\x99s", false);
    CHECK(apostrophed.size() == 1);
    CHECK(apostrophed[0] == "that's");
    auto quoted = r::TextLayout::SplitLines(
        "\xE2\x80\x9C" "Don\xE2\x80\x99t\xE2\x80\x9D \xE2\x80\xA6 \xE2\x80\x94 ok\xC2\xA0!", false);
    CHECK(quoted.size() == 1);
    CHECK(quoted[0] == "\"Don't\" ... - ok !");
    // Text already ASCII must pass through byte-for-byte (fast path).
    CHECK(r::TextLayout::SplitLines("plain ascii", false)[0] == "plain ascii");
    // Invalid UTF-8 lead byte degrades to one '?', not three blank gaps.
    auto bad = r::TextLayout::SplitLines("a\xFF" "b", false);
    CHECK(bad[0] == "a?b");
    // Corpus stragglers: non-breaking hyphen and the Greek question mark.
    CHECK(r::TextLayout::SplitLines("a\xE2\x80\x91" "b", false)[0] == "a-b");
    CHECK(r::TextLayout::SplitLines("x\xCD\xBE" "y", false)[0] == "x;y");
    // Real engine path: layout + atlas contain only ASCII for a raw-UTF-8
    // line, so every drawn byte resolves in the atlas (no 0.6×size gaps).
    auto foldedLayout = r::TextLayout::Measure("Don\xE2\x80\x99t you believe that?", r::TextStyle{}, fm);
    CHECK(foldedLayout.lines.size() == 1);
    bool allAscii = true;
    for (char ch : foldedLayout.lines[0].text)
        if (static_cast<unsigned char>(ch) >= 0x80 || ch < 32) allAscii = false;
    CHECK(allAscii);

    // RenderCache: glyph atlas + layout caching.
    r::RenderCache cache;
    r::FontManager cfm;
    cfm.RegisterBuiltin();
    auto atl = cache.GlyphAtlas(24);
    CHECK(atl.ok() && !atl.value()->atlas.empty());
    CHECK(cache.AtlasCount() >= 1);
    auto lay = cache.Layout("hello", r::TextStyle{}, cfm);
    CHECK(lay.ok() && !lay.value().lines.empty());
    CHECK(cache.LayoutCount() >= 1);
    cache.InvalidateGlyphAtlas();
    cache.InvalidateLayouts();
    cache.Clear();
    CHECK(cache.AtlasCount() == 0 && cache.LayoutCount() == 0);
}
void TestRenderAnimation() {
    // Easing bounds and monotonicity.
    CHECK(r::Ease(r::Easing::Linear, 0.0) == 0.0);
    CHECK(r::Ease(r::Easing::Linear, 0.5) == 0.5);
    CHECK(r::Ease(r::Easing::Linear, 1.0) == 1.0);
    CHECK(r::Ease(r::Easing::Linear, -1.0) == 0.0);     // clamped
    CHECK(r::Ease(r::Easing::Linear, 2.0) == 1.0);
    CHECK(r::Ease(r::Easing::EaseIn, 0.5) < 0.5);       // slow start
    CHECK(r::Ease(r::Easing::EaseOut, 0.5) > 0.5);      // fast start
    CHECK(std::abs(r::Ease(r::Easing::EaseInOut, 0.5) - 0.5) < 1e-9);
    CHECK(r::Ease(r::Easing::BounceOut, 1.0) == 1.0);

    // Keyframe sampling.
    r::AnimTrack track;
    track.objectId = "o";
    track.property = r::AnimProperty::Opacity;
    track.keyframes = {{0.0, 1.0, r::Easing::Linear}, {1.0, 0.0, r::Easing::Linear}};
    CHECK(track.Duration() == 1.0);
    CHECK(std::abs(track.Sample(0.0) - 1.0) < 1e-9);
    CHECK(std::abs(track.Sample(0.5) - 0.5) < 1e-9);
    CHECK(std::abs(track.Sample(1.0) - 0.0) < 1e-9);
    CHECK(std::abs(track.Sample(5.0) - 0.0) < 1e-9);    // clamped past end

    // Animator drives an object's property.
    auto obj = std::make_shared<r::TextObject>("o", "Obj", "x");
    r::Animator anim;
    anim.AddTrack(track);
    CHECK(anim.TrackCount() == 1);
    CHECK(anim.Duration() == 1.0);
    auto lookup = [&](std::string_view id) -> r::RenderObject* {
        return id == "o" ? obj.get() : nullptr;
    };
    anim.Play();
    CHECK(anim.Playing());
    anim.Update(0.5, lookup);
    CHECK(std::abs(obj->Opacity() - 0.5f) < 0.01f);
    CHECK(!anim.Finished());
    anim.Update(0.6, lookup);
    CHECK(anim.Finished());
    CHECK(obj->Opacity() < 0.01f);
    anim.Stop();
    CHECK(!anim.Playing() && anim.Time() == 0.0);
    anim.Seek(0.25);
    CHECK(anim.Time() == 0.25);
    anim.Pause();
    CHECK(!anim.Playing());
    anim.Clear();
    CHECK(anim.TrackCount() == 0);

    // Transitions.
    r::TransitionEngine te;
    r::TransitionSpec fade;
    fade.type = r::TransitionType::Fade;
    fade.durationSec = 0.5;
    auto s0 = te.Evaluate(fade, 0.0);
    CHECK(s0.progress == 0.0 && s0.inOpacity == 0.0f && s0.outOpacity == 1.0f);
    auto s1 = te.Evaluate(fade, 0.5);
    CHECK(s1.progress == 1.0 && s1.inOpacity == 1.0f && s1.outOpacity == 0.0f);

    r::TransitionSpec slide;
    slide.type = r::TransitionType::Slide;
    slide.direction = r::TransitionDirection::Right;
    slide.durationSec = 1.0;
    auto ss0 = te.Evaluate(slide, 0.0);
    CHECK(ss0.inOffset.x == 1.0f);   // incoming off-screen right
    auto ss1 = te.Evaluate(slide, 1.0);
    CHECK(ss1.inOffset.x == 0.0f);

    r::TransitionSpec zoom;
    zoom.type = r::TransitionType::Zoom;
    zoom.durationSec = 1.0;
    CHECK(te.Evaluate(zoom, 0.0).inScale == 0.8f);
    CHECK(te.Evaluate(zoom, 1.0).inScale == 1.0f);

    r::TransitionSpec wipe;
    wipe.type = r::TransitionType::Wipe;
    wipe.durationSec = 1.0;
    CHECK(te.Evaluate(wipe, 0.0).wipe == 0.0f);
    CHECK(te.Evaluate(wipe, 1.0).wipe == 1.0f);

    // CrossFade holds both visible mid-way.
    r::TransitionSpec cf;
    cf.type = r::TransitionType::CrossFade;
    cf.durationSec = 1.0;
    auto cf05 = te.Evaluate(cf, 0.5);
    CHECK(cf05.inOpacity == 1.0f && cf05.outOpacity == 0.0f);   // 2× eased, clamps
    CHECK(std::string(r::ToString(r::TransitionType::Fade)) == "Fade");
}
void TestRenderEffects() {
    r::EffectStack stack;
    CHECK(stack.Count() == 0);
    stack.AddBlur(3.0f);
    stack.AddGlow(2.0f);
    stack.AddShadow(4.0f);
    stack.AddOpacity(0.5f);
    stack.AddBrightness(1.2f);
    stack.AddContrast(1.1f);
    stack.AddSaturation(0.8f);
    stack.AddCrop(r::Rect(10, 10, 50, 50));
    CHECK(stack.Count() == 8);
    CHECK(stack.Has(r::EffectType::Blur) && stack.Has(r::EffectType::Crop));
    CHECK(!stack.Has(r::EffectType::Mask));
    CHECK(stack.CropRegion().width == 50.0f);
    stack.SetEnabled(0, false);
    CHECK(!stack.Effects()[0].enabled);
    CHECK(std::string(r::ToString(r::EffectType::Blur)) == "Blur");
    stack.Clear();
    CHECK(stack.Count() == 0);
}
void TestRenderPipeline() {
    // Layer → pass mapping.
    CHECK(r::PassForLayer(r::LayerKind::Background) == r::PassId::Background);
    CHECK(r::PassForLayer(r::LayerKind::Video) == r::PassId::Video);
    CHECK(r::PassForLayer(r::LayerKind::Image) == r::PassId::Image);
    CHECK(r::PassForLayer(r::LayerKind::Text) == r::PassId::Text);
    CHECK(r::PassForLayer(r::LayerKind::Overlay) == r::PassId::Overlay);
    CHECK(r::PassForLayer(r::LayerKind::Debug) == r::PassId::Debug);
    CHECK(r::PassForLayer(r::LayerKind::Custom) == r::PassId::Overlay);

    // Distribution respects layer enablement + kinds.
    r::RenderPipeline pipe;
    auto bg = std::make_shared<r::BackgroundObject>("b", "bg", r::Color::Red());
    bg->SetLayer("bg");
    auto txt = std::make_shared<r::TextObject>("t", "text", "hi");
    txt->SetLayer("txt");
    std::vector<r::RenderObject*> objs{bg.get(), txt.get()};
    std::vector<r::Layer> layers{
        r::Layer("bg", "B", r::LayerKind::Background, 0),
        r::Layer("txt", "T", r::LayerKind::Text, 1)};
    pipe.Distribute(objs, layers);
    CHECK(pipe.Pass(r::PassId::Background)->Count() == 1);
    CHECK(pipe.Pass(r::PassId::Text)->Count() == 1);
    CHECK(pipe.TotalObjects() == 2);

    // Pass execution order: Background before Text.
    std::vector<r::PassId> order;
    pipe.Execute([&](r::PassId p, const std::vector<r::RenderObject*>&) {
        order.push_back(p);
    });
    bool sawBg = false, sawTxt = false, bgBeforeTxt = true;
    for (r::PassId p : order) {
        if (p == r::PassId::Background) sawBg = true;
        if (p == r::PassId::Text) {
            if (!sawBg) bgBeforeTxt = false;
            sawTxt = true;
        }
    }
    CHECK(sawBg && sawTxt && bgBeforeTxt);

    // A disabled layer drops its objects.
    std::vector<r::Layer> disabled{
        r::Layer("bg", "B", r::LayerKind::Background, 0),
        r::Layer("txt", "T", r::LayerKind::Text, 1)};
    disabled[1].enabled = false;
    pipe.Distribute(objs, disabled);
    CHECK(pipe.Pass(r::PassId::Text)->Count() == 0);
    CHECK(pipe.TotalObjects() == 1);
    pipe.ClearPasses();
    CHECK(pipe.TotalObjects() == 0);

    // Render graph: topological ordering + cycle detection + custom passes.
    r::RenderGraph graph;
    CHECK(graph.AddNode({"a", r::PassId::Image, {}, 0}).ok());
    CHECK(graph.AddNode({"b", r::PassId::Image, {"a"}, 0}).ok());
    CHECK(graph.AddNode({"c", r::PassId::Text, {"b"}, 0}).ok());
    CHECK(graph.Count() == 3);
    auto orderOk = graph.ExecutionOrder();
    CHECK(orderOk.ok() && orderOk.value().size() == 3);
    if (orderOk.ok()) CHECK(orderOk.value()[0] == "a");
    // Cycle detection.
    r::RenderGraph cyc;
    CHECK(cyc.AddNode({"x", r::PassId::Image, {"y"}, 0}).ok());
    CHECK(cyc.AddNode({"y", r::PassId::Image, {"x"}, 0}).ok());
    CHECK(!cyc.ExecutionOrder().ok());
    // Custom pass functions run in topo order.
    r::RenderGraph pg;
    CHECK(pg.AddNode({"p1", r::PassId::Image, {}, 0}).ok());
    CHECK(pg.AddNode({"p2", r::PassId::Image, {"p1"}, 0}).ok());
    std::vector<std::string> ran;
    CHECK(pg.RegisterPassFn("p1", [&](const r::RenderObject*) { ran.push_back("p1"); }).ok());
    CHECK(pg.RegisterPassFn("p2", [&](const r::RenderObject*) { ran.push_back("p2"); }).ok());
    CHECK(!pg.RegisterPassFn("missing", [&](const r::RenderObject*) {}).ok());
    pg.ExecutePasses();
    CHECK(ran.size() == 2 && ran[0] == "p1" && ran[1] == "p2");
}
void TestRenderGpu() {
    // GPU resource manager over the software backend.
    r::SoftwareGraphicsBackend sw;
    r::GPUResourceManager gpu;
    gpu.BindBackend(&sw);
    r::RgbaImage tex;
    tex.width = 2;
    tex.height = 2;
    tex.pixels = {0, 0, 0, 0};
    auto id1 = gpu.AcquireTexture("tex", tex);
    CHECK(id1.ok() && id1.value().valid());
    auto id2 = gpu.AcquireTexture("tex", tex);   // dedup → same id, refs++
    CHECK(id2.ok() && id2.value().id == id1.value().id);
    CHECK(gpu.TextureCount() == 1);
    auto got = gpu.GetTexture("tex");
    CHECK(got.ok() && got.value().id == id1.value().id);
    CHECK(!gpu.GetTexture("nope").ok());
    CHECK(gpu.TextureMemoryBytes() > 0);
    // Eviction keeps referenced textures.
    CHECK(gpu.EvictIdle(0) == 0);   // both refs > 0 → none dropped
    CHECK(gpu.ReleaseTexture("tex").ok());   // refs back to 1
    CHECK(gpu.ReleaseTexture("tex").ok());   // refs 0 → destroyed
    CHECK(gpu.TextureCount() == 0);
    CHECK(!gpu.ReleaseTexture("tex").ok());  // gone
    // Budget enforcement.
    gpu.SetTextureBudget(10);
    auto over = gpu.AcquireTexture("big", tex);
    CHECK(!over.ok() && over.error().code == Err::Render_TextureAllocFailed);
    gpu.Clear();
    CHECK(gpu.TextureCount() == 0);

    // FrameGraph idle tracking.
    r::FrameGraph fg;
    fg.BeginFrame();
    fg.Use("tex", 1024);
    fg.EndFrame();
    fg.BeginFrame();
    fg.EndFrame();
    fg.BeginFrame();
    fg.EndFrame();
    auto idle = fg.IdleResources(2);   // not used for 2 frames
    bool foundTex = false;
    for (const auto& n : idle)
        if (n == "tex") foundTex = true;
    CHECK(foundTex);
    CHECK(fg.LiveCount() >= 1);
    fg.Use("tex", 1024);   // used again → resets idle counter
    fg.EndFrame();
    CHECK(fg.IdleResources(2).empty());

    // ResourceUploader: async decode on the core thread pool, drained by Pump.
    r::ResourceUploader up;
    auto& pool = ThreadPool::Instance();
    if (pool.WorkerCount() == 0) (void)pool.Initialize(2);   // may have been shut down earlier
    std::atomic<int> doneCalls{0};
    up.Submit("img", std::vector<uint8_t>{1, 2, 3},
              [](const std::vector<uint8_t>&) {
                  r::RgbaImage i;
                  i.width = 1;
                  i.height = 1;
                  i.pixels = {0xFFFFFFFFu};
                  return i;
              },
              [&](bps::Result<r::TextureId>) { doneCalls++; });
    bool drained = false;
    for (int i = 0; i < 100; ++i) {
        up.Pump();
        if (up.Pending() == 0 && doneCalls.load() > 0) {
            drained = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    CHECK(drained);
    up.Shutdown();
}
void TestRenderOutputs() {
    // FrameBufferOutput: present + scale to target.
    auto out = std::make_shared<r::FrameBufferOutput>(r::OutputKind::Preview, "preview",
                                                      r::Size(32, 32));
    CHECK(out->Kind() == r::OutputKind::Preview);
    CHECK(std::string(out->Name()) == "preview");
    CHECK(out->TargetSize().width == 32.0f);
    r::Frame f;
    f.sceneId = "s1";
    f.frame = 1;
    f.width = 64;
    f.height = 64;
    f.pixels.assign(64 * 64, r::Color::Red().Pack());
    CHECK(out->Present(f).ok());
    CHECK(out->FramesReceived() == 1);
    auto lf = out->LastFrame();
    CHECK(lf.width == 32 && lf.height == 32);   // scaled down
    CHECK(!lf.pixels.empty());
    out->SetEnabled(false);
    CHECK(!out->Enabled());
    CHECK(out->Present(f).ok());
    CHECK(out->FramesReceived() == 1);   // disabled → ignored
    out->SetEnabled(true);

    // Output manager: add/get/remove + distribute.
    r::OutputManager mgr;
    auto a = std::make_shared<r::FrameBufferOutput>(r::OutputKind::Audience, "aud",
                                                    r::Size(64, 64));
    auto st = std::make_shared<r::FrameBufferOutput>(r::OutputKind::Stage, "stage",
                                                     r::Size(32, 32));
    CHECK(mgr.Add(a).ok());
    CHECK(mgr.Add(st).ok());
    CHECK(!mgr.Add(a).ok());   // duplicate name
    CHECK(mgr.Count() == 2);
    CHECK(mgr.Get("aud").ok() && !mgr.Get("nope").ok());
    f.width = 64;
    f.height = 64;
    f.pixels.assign(64 * 64, r::Color::Blue().Pack());
    mgr.Distribute(f);
    CHECK(a->FramesReceived() == 1);
    CHECK(st->FramesReceived() == 1);
    CHECK(st->LastFrame().width == 32);
    CHECK(mgr.Remove("stage").ok());
    CHECK(mgr.Count() == 1);
    mgr.Clear();
    CHECK(mgr.Count() == 0);

    // Screenshot output → PPM file via the PAL.
    auto shot = std::make_shared<r::ScreenshotOutput>("shot", r::Size(8, 8));
    f.width = 8;
    f.height = 8;
    f.pixels.assign(64, r::Color::Green().Pack());
    CHECK(shot->Present(f).ok());
    const std::string ppmPath = "/tmp/bps_render_shot.ppm";
    CHECK(shot->SavePpm(ppmPath).ok());
    auto bytes = bps::platform::PlatformAccessor::Get().Filesystem().ReadBinary(ppmPath);
    CHECK(bytes.ok());
    if (bytes.ok()) {
        CHECK(bytes.value().size() > 20);
        CHECK(std::string(bytes.value().begin(), bytes.value().begin() + 2) == "P6");
    }
    (void)bps::platform::PlatformAccessor::Get().Filesystem().Remove(ppmPath);
    // No frame → clear error.
    auto empty = std::make_shared<r::ScreenshotOutput>("e", r::Size(4, 4));
    CHECK(!empty->SavePpm("/tmp/nope.ppm").ok());
}
void TestRenderEngine() {
    auto& engine = r::RenderEngine::Instance();
    CHECK(engine.Initialize().ok());
    CHECK(engine.Start().ok());
    CHECK(engine.SetBackend("software").ok());
    CHECK(!engine.SetBackend("vulkan").ok());   // unavailable
    CHECK(engine.GetHealth().state == HealthState::Healthy);

    // Scene with background + image + text + shape layers.
    CHECK(engine.CreateScene("s1", "Test", r::Size(64, 64)).ok());
    CHECK(!engine.CreateScene("s1", "Dup", r::Size(64, 64)).ok());
    CHECK(engine.AddLayer("s1", r::Layer("bg", "Background", r::LayerKind::Background, 0)).ok());
    CHECK(engine.AddLayer("s1", r::Layer("img", "Images", r::LayerKind::Image, 1)).ok());
    CHECK(engine.AddLayer("s1", r::Layer("txt", "Text", r::LayerKind::Text, 2)).ok());
    CHECK(engine.AddLayer("nope", r::Layer("x", "X", r::LayerKind::Image, 0))
              .error().code == Err::Render_SceneNotFound);

    auto bg = std::make_shared<r::BackgroundObject>("bg1", "Background", r::Color::Red());
    bg->SetBounds(r::Rect(0, 0, 64, 64));
    CHECK(engine.AddObject("s1", bg, "bg").ok());

    r::RgbaImage green;
    green.width = 8;
    green.height = 8;
    green.pixels.assign(64, r::Color::Green().Pack());
    auto img = std::make_shared<r::ImageObject>("img1", "Image");
    img->SetImage(green, "asset-g");
    img->SetBounds(r::Rect(8, 8, 16, 16));
    CHECK(engine.AddObject("s1", img, "img").ok());

    auto text = std::make_shared<r::TextObject>("txt1", "Title", "HELLO");
    text->SetBounds(r::Rect(0, 40, 64, 24));
    text->AddComponent(r::MakeTextStyle(r::TextStyle{}));
    CHECK(engine.AddObject("s1", text, "txt").ok());

    auto shape = std::make_shared<r::ShapeObject>("shp1", "Shape");
    shape->SetRectangle(r::Color::Blue());
    shape->SetBounds(r::Rect(40, 8, 16, 16));
    CHECK(engine.AddObject("s1", shape, "img").ok());

    CHECK(engine.GetObject("s1", "bg1").ok());
    CHECK(!engine.GetObject("s1", "missing").ok());
    CHECK(engine.CollectObjects("s1").size() == 4);

    // Render a frame and verify pixels.
    auto frame = engine.Render("s1");
    CHECK(frame.ok());
    if (frame.ok()) {
        CHECK(frame.value().width == 64 && frame.value().height == 64);
        // Center: red background (outside image/shape regions).
        r::Color center = r::Color::Unpack(frame.value().pixels[32 * 64 + 32]);
        CHECK(center.r > 0.9f && center.g < 0.1f);
        // Inside the green image region.
        r::Color gi = r::Color::Unpack(frame.value().pixels[12 * 64 + 12]);
        CHECK(gi.g > 0.9f && gi.r < 0.1f);
        // Inside the blue shape region.
        r::Color sh = r::Color::Unpack(frame.value().pixels[10 * 64 + 48]);
        CHECK(sh.b > 0.9f && sh.g < 0.1f);
        // Text glyphs produce bright pixels somewhere.
        int bright = 0;
        for (uint32_t px : frame.value().pixels) {
            r::Color c = r::Color::Unpack(px);
            if (c.r > 0.9f && c.g > 0.9f && c.b > 0.9f) ++bright;
        }
        CHECK(bright > 0);
    }

    // Offscreen + thumbnail rendering.
    auto small = engine.RenderOffscreen("s1", r::Size(32, 32));
    CHECK(small.ok() && small.value().width == 32);
    auto thumb = engine.RenderThumbnail("s1", r::Size(16, 16));
    CHECK(thumb.ok() && thumb.value().width == 16);
    CHECK(!engine.Render("missing-scene").ok());

    // Texture upload API.
    auto tid = engine.UploadTexture("up", green);
    CHECK(tid.ok() && tid.value().valid());
    CHECK(engine.GetTexture("up").ok());
    CHECK(engine.ReleaseTexture("up").ok());
    CHECK(!engine.GetTexture("up").ok());

    // Async upload path: decode on the thread pool, texture materialized by
    // Pump (inside Render), done callback fired.
    std::atomic<int> asyncDone{0};
    engine.SubmitAsyncUpload(
        "async", std::vector<uint8_t>{1, 2, 3},
        [](const std::vector<uint8_t>&) {
            r::RgbaImage i;
            i.width = 2;
            i.height = 2;
            i.pixels.assign(4, r::Color::White().Pack());
            return i;
        },
        [&](bps::Result<r::TextureId>) { asyncDone++; });
    // TEMP DIAGNOSTIC (apostrophe/stray-tick probe): render the exact live
    // chorus (Segoe UI bold, wrapped, centered) at full output size and dump
    // a PNG so the engine's actual glyphs can be inspected pixel-level.
    {
        CHECK(engine.CreateScene("probe", "Probe", r::Size(1920, 1080)).ok());
        CHECK(engine.AddLayer("probe", r::Layer("bg", "Background", r::LayerKind::Background, 0)).ok());
        CHECK(engine.AddLayer("probe", r::Layer("text", "Text", r::LayerKind::Text, 2)).ok());
        auto bg = std::make_shared<r::BackgroundObject>("pbg", "Background", r::Color(0, 0, 0));
        bg->SetBounds(r::Rect(0, 0, 1920, 1080));
        CHECK(engine.AddObject("probe", bg, "bg").ok());
        const std::string chorus =
            "When I come into your presence , I\xE2\x80\x99" "m so happy\n"
            "When I come into your presence , I\xE2\x80\x99" "m so \nGlad\n"
            "In your presence, there\xE2\x80\x99" "s anointing\n"
            "And the Spirit moves around me\n"
            "In your presence, the anointing breaks the yoke.";
        auto text = std::make_shared<r::TextObject>("ptxt", "Body", chorus);
        text->SetBounds(r::Rect(50, 88, 1820, 904));
        r::TextStyle ts;
        ts.fontId = "Segoe UI";
        ts.size = 100.0f;
        ts.bold = true;
        ts.color = r::Color::White();
        ts.align = r::TextAlign::Center;
        ts.valign = r::TextVAlign::Middle;
        ts.wrap = true;
        ts.wrapWidth = 1820.0f;
        text->AddComponent(r::MakeTextStyle(ts));
        CHECK(engine.AddObject("probe", text, "text").ok());
        auto img = engine.Render("probe");
        CHECK(img.ok() && img.value().width == 1920);
        if (img.ok()) {
            auto png = r::EncodePngRgba8(
                reinterpret_cast<const uint8_t *>(img.value().pixels.data()),
                img.value().width, img.value().height);
            if (png.ok()) {
                std::ofstream out("logs/apostrophe_probe.png", std::ios::binary);
                out.write(reinterpret_cast<const char *>(png.value().data()),
                          static_cast<std::streamsize>(png.value().size()));
            }
            // Self-contained viewer (the preview server serves exactly one
            // file — sibling assets 404): PNG inlined as a data URI.
            if (png.ok()) {
                static const char *kB64 =
                    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
                std::string b64;
                b64.reserve((png.value().size() + 2) / 3 * 4);
                const auto *d = png.value().data();
                size_t i = 0;
                for (; i + 2 < png.value().size(); i += 3) {
                    const uint32_t v = (d[i] << 16) | (d[i + 1] << 8) | d[i + 2];
                    b64 += kB64[(v >> 18) & 63]; b64 += kB64[(v >> 12) & 63];
                    b64 += kB64[(v >> 6) & 63]; b64 += kB64[v & 63];
                }
                if (i + 1 < png.value().size()) {
                    const uint32_t v = (d[i] << 16) | (d[i + 1] << 8);
                    b64 += kB64[(v >> 18) & 63]; b64 += kB64[(v >> 12) & 63];
                    b64 += kB64[(v >> 6) & 63]; b64 += '=';
                } else if (i < png.value().size()) {
                    const uint32_t v = d[i] << 16;
                    b64 += kB64[(v >> 18) & 63]; b64 += kB64[(v >> 12) & 63];
                    b64 += "==";
                }
                std::ofstream html("probe.html", std::ios::binary);
                html << "<!doctype html><html><body style='margin:0;background:#222'>"
                     << "<img style='width:100%' src='data:image/png;base64," << b64
                     << "'></body></html>";
            }
        }
        (void)engine.DestroyScene("probe");
    }

    // TEMP DIAGNOSTIC (style-font probe): the user's actual template font
    // (TT Nooks Trial — installed per-user) with folded apostrophes. This
    // font has no U+2019, so before the FontOwnsGlyph fallback every
    // apostrophe cell drew blank .notdef ink and vanished; now each one
    // draws the ASCII quote the font DOES own. Self-contained viewer:
    // fontprobe.html (the preview server serves exactly one file).
    {
        CHECK(engine.CreateScene("fontprobe", "Probe", r::Size(1920, 1080)).ok());
        CHECK(engine.AddLayer("fontprobe", r::Layer("bg", "Background", r::LayerKind::Background, 0)).ok());
        CHECK(engine.AddLayer("fontprobe", r::Layer("text", "Text", r::LayerKind::Text, 2)).ok());
        auto nbg = std::make_shared<r::BackgroundObject>("pbg", "Background", r::Color(0, 0, 0));
        nbg->SetBounds(r::Rect(0, 0, 1920, 1080));
        CHECK(engine.AddObject("fontprobe", nbg, "bg").ok());
        const std::string nooksBody =
            "God\xE2\x80\x99" "s Spirit moves, it\xE2\x80\x99" "s alive (TT Nooks Trial)\n"
            "He said, come unto me, all ye that labour\n"
            "And I will give you rest, saith the Lord. It's a straight one.\n"
            "Matthew 11:28-30";
        auto ntext = std::make_shared<r::TextObject>("ptxt", "Body", nooksBody);
        ntext->SetBounds(r::Rect(50, 88, 1820, 904));
        r::TextStyle nts;
        nts.fontId = "TT Nooks Trial";
        nts.size = 90.0f;
        nts.color = r::Color::White();
        nts.align = r::TextAlign::Center;
        nts.valign = r::TextVAlign::Middle;
        nts.wrap = true;
        nts.wrapWidth = 1820.0f;
        ntext->AddComponent(r::MakeTextStyle(nts));
        CHECK(engine.AddObject("fontprobe", ntext, "text").ok());
        auto nimg = engine.Render("fontprobe");
        CHECK(nimg.ok() && nimg.value().width == 1920);
        if (nimg.ok()) {
            auto png = r::EncodePngRgba8(
                reinterpret_cast<const uint8_t *>(nimg.value().pixels.data()),
                nimg.value().width, nimg.value().height);
            if (png.ok()) {
                std::ofstream out("logs/font_probe.png", std::ios::binary);
                out.write(reinterpret_cast<const char *>(png.value().data()),
                          static_cast<std::streamsize>(png.value().size()));
                static const char *kB64 =
                    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
                std::string b64;
                b64.reserve((png.value().size() + 2) / 3 * 4);
                const auto *d = png.value().data();
                size_t i = 0;
                for (; i + 2 < png.value().size(); i += 3) {
                    const uint32_t v = (d[i] << 16) | (d[i + 1] << 8) | d[i + 2];
                    b64 += kB64[(v >> 18) & 63]; b64 += kB64[(v >> 12) & 63];
                    b64 += kB64[(v >> 6) & 63]; b64 += kB64[v & 63];
                }                if (i + 1 < png.value().size()) {
                    const uint32_t v = (d[i] << 16) | (d[i + 1] << 8);
                    b64 += kB64[(v >> 18) & 63]; b64 += kB64[(v >> 12) & 63];
                    b64 += kB64[(v >> 6) & 63]; b64 += '=';
                } else if (i < png.value().size()) {
                    const uint32_t v = d[i] << 16;
                    b64 += kB64[(v >> 18) & 63]; b64 += kB64[(v >> 12) & 63];
                    b64 += "==";
                }

                std::ofstream html("fontprobe.html", std::ios::binary);
                html << "<!doctype html><html><body style='margin:0;background:#222'>"
                     << "<img style='width:100%' src='data:image/png;base64," << b64
                     << "'></body></html>";
            }
        }
        (void)engine.DestroyScene("fontprobe");
    }

    bool asyncOk = false;
    for (int i = 0; i < 100; ++i) {
        (void)engine.Render("s1");   // pump() runs inside Render
        if (engine.Stats().renderQueueSize == 0 && asyncDone.load() > 0) {
            asyncOk = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    CHECK(asyncOk);
    (void)engine.ReleaseTexture("async");   // texture was materialized

    // Outputs: distribution to an attached output.
    auto preview =
        std::make_shared<r::FrameBufferOutput>(r::OutputKind::Preview, "preview", r::Size(32, 32));
    CHECK(engine.AddOutput(preview).ok());
    CHECK(engine.OutputCount() == 1);
    CHECK(engine.GetOutput("preview").ok());
    (void)engine.Render("s1");
    CHECK(preview->FramesReceived() >= 1);
    CHECK(preview->LastFrame().width == 32);
    CHECK(engine.RemoveOutput("preview").ok());
    CHECK(engine.OutputCount() == 0);

    // EventBus integration: RenderFrameRendered published each frame.
    int rendered = 0;
    Subscription sub = EventBus::Instance().Subscribe<events::RenderFrameRendered>(
        [&](const events::RenderFrameRendered&) { ++rendered; }, 0);
    (void)engine.Render("s1");
    CHECK(rendered >= 1);
    (void)EventBus::Instance().Unsubscribe(sub);

    // Diagnostics.
    auto st = engine.Stats();
    CHECK(st.frames >= 1);
    CHECK(st.textureCount >= 1);   // font atlas uploaded during text draw
    CHECK(st.frameMs >= 0.0);
    CHECK(engine.MetricsSnapshot().health == HealthState::Healthy);

    // Memory pressure: evicts + keeps rendering.
    engine.OnMemoryPressure(PressureLevel::High);   // must not crash
    (void)engine.Render("s1");
    CHECK(engine.Stats().frames >= 2);

    // Config hot-reload path (Reload keeps backend consistent).
    CHECK(engine.Reload().ok());

    // Cleanup.
    CHECK(engine.Stop().ok());
    CHECK(engine.Reset().ok());
    CHECK(engine.Shutdown().ok());
    CHECK(engine.GetHealth().state == HealthState::Degraded);
}
void TestRenderStress() {
    auto& engine = r::RenderEngine::Instance();
    CHECK(engine.Initialize().ok());
    CHECK(engine.Start().ok());
    CHECK(engine.SetBackend("software").ok());
    CHECK(engine.CreateScene("stress", "Stress", r::Size(64, 64)).ok());
    CHECK(engine.AddLayer("stress", r::Layer("bg", "Background", r::LayerKind::Background, 0)).ok());
    CHECK(engine.AddLayer("stress", r::Layer("shapes", "Shapes", r::LayerKind::Image, 1)).ok());
    auto bg = std::make_shared<r::BackgroundObject>("bg", "Background", r::Color::Black());
    bg->SetBounds(r::Rect(0, 0, 64, 64));
    CHECK(engine.AddObject("stress", bg, "bg").ok());

    // 2000 render objects through the pipeline in one scene.
    auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < 2000; ++i) {
        auto s = std::make_shared<r::ShapeObject>("s" + std::to_string(i), "Shape");
        s->SetRectangle(r::Color(0.1f + 0.0004f * i, 0.2f, 0.3f, 1.0f));
        s->SetBounds(r::Rect(static_cast<float>(i % 60), static_cast<float>((i / 60) % 60), 3, 3));
        if (!engine.AddObject("stress", s, "shapes").ok()) break;
    }
    CHECK(engine.CollectObjects("stress").size() == 2001);   // 2000 shapes + background
    auto frame = engine.Render("stress");
    CHECK(frame.ok() && frame.value().width == 64);
    auto t1 = std::chrono::steady_clock::now();
    const double buildMs = std::chrono::duration<double, std::milli>(t1 - t0).count();
    CHECK(buildMs < 5000.0);   // sanity: 2000 objects + one frame stays fast
    CHECK(engine.Stats().frames >= 1);
    CHECK(engine.Reset().ok());
    CHECK(engine.Shutdown().ok());
}
#ifdef BPS_HAVE_ZLIB
#include <zlib.h>
#endif

#ifdef BPS_HAVE_ZLIB
// Builds a minimal valid PNG (RGBA8, no interlace, unfiltered scanlines) from
// raw pixels using zlib's deflate, then decodes it with PngCodec to prove the
// full encode -> decode round trip. (Whole definition guarded: it is the only
// user of zlib here, and a bare definition in a zlib-less build failed to
// compile — crc32/uLongf undeclared.)
static std::vector<uint8_t> MakeTestPng(int w, int h,
                                        const std::vector<uint32_t>& rgba) {
    std::vector<uint8_t> out = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    auto put32 = [&](uint32_t v) {
        out.push_back(static_cast<uint8_t>(v >> 24));
        out.push_back(static_cast<uint8_t>((v >> 16) & 0xFF));
        out.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
        out.push_back(static_cast<uint8_t>(v & 0xFF));
    };
    auto chunk = [&](const char type[5], const uint8_t* data, size_t len) {
        put32(static_cast<uint32_t>(len));
        size_t typeStart = out.size();
        out.insert(out.end(), type, type + 4);
        out.insert(out.end(), data, data + len);
        uint32_t crc = static_cast<uint32_t>(::crc32(0L, out.data() + typeStart,
                                                     out.size() - typeStart));
        put32(crc);
    };
    uint8_t ihdr[13] = {0, 0, 0, 0,  0, 0, 0, 0,  8, 6, 0, 0, 0};
    ihdr[0] = static_cast<uint8_t>(w >> 24); ihdr[1] = static_cast<uint8_t>(w >> 16);
    ihdr[2] = static_cast<uint8_t>(w >> 8);  ihdr[3] = static_cast<uint8_t>(w);
    ihdr[4] = static_cast<uint8_t>(h >> 24); ihdr[5] = static_cast<uint8_t>(h >> 16);
    ihdr[6] = static_cast<uint8_t>(h >> 8);  ihdr[7] = static_cast<uint8_t>(h);
    chunk("IHDR", ihdr, 13);

    // Raw scanlines: filter byte 0 (None) + 4 bytes per pixel.
    std::vector<uint8_t> raw;
    raw.reserve((size_t)h * (1 + (size_t)w * 4));
    for (int y = 0; y < h; ++y) {
        raw.push_back(0);   // filter None
        for (int x = 0; x < w; ++x) {
            uint32_t px = rgba[(size_t)y * w + x];
            raw.push_back(static_cast<uint8_t>(px));          // R
            raw.push_back(static_cast<uint8_t>(px >> 8));     // G
            raw.push_back(static_cast<uint8_t>(px >> 16));    // B
            raw.push_back(static_cast<uint8_t>(px >> 24));    // A
        }
    }
    uLongf bound = compressBound(static_cast<uLong>(raw.size()));
    std::vector<uint8_t> comp(bound);
    uLongf clen = bound;
    (void)compress2(comp.data(), &clen, raw.data(), static_cast<uLong>(raw.size()),
                    Z_DEFAULT_COMPRESSION);
    chunk("IDAT", comp.data(), clen);
    chunk("IEND", nullptr, 0);
    return out;
}
#endif   // BPS_HAVE_ZLIB

void TestPngCodec() {
    namespace r = bps::rendering;
#ifdef BPS_HAVE_ZLIB
    // 3x2 image with distinct per-pixel colors.
    const int W = 3, H = 2;
    std::vector<uint32_t> px = {
        0xFF000000u, 0xFF7F0000u, 0xFFFF0000u,   // black, dark red, red
        0xFFFFFFFFu, 0xFF00FF00u, 0xFF0000FFu    // white, green, blue
    };
    auto png = MakeTestPng(W, H, px);
    CHECK(r::LooksLikePng(png.data(), png.size()));
    auto dec = r::DecodePng(png.data(), png.size());
    CHECK(dec.ok());
    if (dec.ok()) {
        CHECK(dec.value().width == W && dec.value().height == H);
        CHECK(dec.value().pixels.size() == static_cast<size_t>(W) * H);
        for (size_t i = 0; i < px.size(); ++i) CHECK(dec.value().pixels[i] == px[i]);
    }
    // Corruption is rejected, never a crash.
    auto corrupt = png;
    if (!corrupt.empty()) corrupt[20] ^= 0xFF;   // flip a byte inside IHDR
    auto bad = r::DecodePng(corrupt.data(), corrupt.size());
    CHECK(!bad.ok());
    // Decompression-bomb guard: a tiny valid deflate payload behind a huge
    // IHDR must fail fast (IDAT expands beyond the image dimensions), never
    // allocate gigabytes.
    {
        auto bomb = MakeTestPng(1, 1, {0xFFFFFFFFu});
        // Rewrite IHDR width/height to 16384x16384 (bytes 16..23 of the file)
        // and re-stamp the chunk CRC (bytes 29..32) so parsing reaches IDAT.
        if (bomb.size() >= 33) {
            for (int i = 0; i < 4; ++i) {
                bomb[16 + i] = static_cast<uint8_t>(16384 >> (24 - 8 * i));
                bomb[20 + i] = static_cast<uint8_t>(16384 >> (24 - 8 * i));
            }
            uint32_t crc = static_cast<uint32_t>(::crc32(0L, bomb.data() + 12, 17));
            bomb[29] = static_cast<uint8_t>(crc >> 24);
            bomb[30] = static_cast<uint8_t>((crc >> 16) & 0xFF);
            bomb[31] = static_cast<uint8_t>((crc >> 8) & 0xFF);
            bomb[32] = static_cast<uint8_t>(crc & 0xFF);
            auto bb = r::DecodePng(bomb.data(), bomb.size());
            CHECK(!bb.ok());
        }
    }
    // Not a PNG at all.
    const uint8_t junk[4] = {0, 1, 2, 3};
    CHECK(!r::LooksLikePng(junk, sizeof junk));
    CHECK(!r::DecodePng(junk, sizeof junk).ok());
    CHECK(!r::DecodePng(nullptr, 0).ok());
#else
    // Without zlib the decoder must degrade to Unsupported, not crash.
    const uint8_t junk[4] = {0, 1, 2, 3};
    CHECK(!r::DecodePng(junk, sizeof junk).ok());
#endif
}

// ===========================================================================
// Phase 7 — Display & Output Engine (docs/specs/18)
// ===========================================================================
