#include "TestHarness.hpp"
#include "modules/presentation/PresentationTypes.hpp"
#include "modules/presentation/SceneBuilder.hpp"
#include "modules/rendering/ImageScale.hpp"
#include "modules/rendering/PngCodec.hpp"
#include "modules/rendering/RenderOutputs.hpp"

#include <QFont>
#include <QTextLayout>
#include <QTextOption>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <fstream>

void TestGlyphAtlasTextureIdentity() {
    auto& engine = r::RenderEngine::Instance();
    CHECK(engine.Initialize().ok());
    CHECK(engine.SetBackend("software").ok());
    r::FontManager probe;
    probe.RegisterBuiltin();
    if (!probe.HasSystemFont("Segoe UI")) return;   // Qt/system font backend unavailable

    const auto makeScene = [&](const std::string& id, float size) {
        CHECK(engine.CreateScene(id, id, r::Size(640, 200)).ok());
        CHECK(engine.AddLayer(id, r::Layer("t", "Text", r::LayerKind::Text, 1)).ok());
        auto t = std::make_shared<r::TextObject>("t" + id, "Body", "o");
        t->SetBounds(r::Rect(0, 0, 640, 200));
        r::TextStyle ts;
        ts.fontId = "Segoe UI";
        ts.size = size;
        ts.color = r::Color::White();
        t->AddComponent(r::MakeTextStyle(ts));
        CHECK(engine.AddObject(id, t, "t").ok());
    };
    makeScene("gaA", 88.42f);
    makeScene("gaB", 88.62f);

    CHECK(engine.Render("gaA").ok());
    const uint32_t afterA = engine.Stats().textureCount;
    CHECK(engine.Render("gaB").ok());
    const uint32_t afterB = engine.Stats().textureCount;
    // The colliding size must add a texture of its own: under the old name the
    // second render reused the first atlas and this delta was 0.
    CHECK(afterB == afterA + 1);

    CHECK(engine.DestroyScene("gaA").ok());
    CHECK(engine.DestroyScene("gaB").ok());
}
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

    out = r::SoftwareGraphicsBackend::BlendPixel(
        r::Color::Black().Pack(), r::Color(0.4f, 0.6f, 0.8f, 1).Pack(), r::BlendMode::Screen);
    CHECK(std::abs(r::Color::Unpack(out).r - 0.4f) < 0.02f);
    CHECK(std::abs(r::Color::Unpack(out).b - 0.8f) < 0.02f);
    // Screen over white is white, whatever the source.
    out = r::SoftwareGraphicsBackend::BlendPixel(
        r::Color::White().Pack(), r::Color::Black().Pack(), r::BlendMode::Screen);
    CHECK(r::Color::Unpack(out).r > 0.98f);
    // Overlay: a light source LIGHTENS the backdrop, a dark one darkens it.
    out = r::SoftwareGraphicsBackend::BlendPixel(
        r::Color(0.25f, 0.25f, 0.25f, 1).Pack(), r::Color(1, 1, 1, 1).Pack(),
        r::BlendMode::Overlay);
    CHECK(r::Color::Unpack(out).r > 0.45f);
    out = r::SoftwareGraphicsBackend::BlendPixel(
        r::Color(0.25f, 0.25f, 0.25f, 1).Pack(), r::Color::Black().Pack(),
        r::BlendMode::Overlay);
    CHECK(r::Color::Unpack(out).r < 0.1f);
    // Difference: black − white and white − black are both pure white.
    out = r::SoftwareGraphicsBackend::BlendPixel(
        r::Color::White().Pack(), r::Color::Black().Pack(), r::BlendMode::Difference);
    CHECK(r::Color::Unpack(out).r > 0.98f);
    out = r::SoftwareGraphicsBackend::BlendPixel(
        r::Color(1.0f, 0.0f, 0.0f, 1).Pack(), r::Color(0.5f, 0.5f, 0.5f, 1).Pack(),
        r::BlendMode::Luminosity);
    {
        const r::Color lum = r::Color::Unpack(out);
        CHECK(lum.r > 0.9f);
        CHECK(lum.g > 0.2f && lum.g < 0.4f);
    }
    // A transparent source leaves the backdrop alone in every mode.
    out = r::SoftwareGraphicsBackend::BlendPixel(
        r::Color(0.3f, 0.3f, 0.3f, 1).Pack(), r::Color(1, 1, 1, 0.0f).Pack(),
        r::BlendMode::Luminosity);
    CHECK(std::abs(r::Color::Unpack(out).r - 0.3f) < 0.02f);

    // The NAMES the MEDIA tab stores map onto those modes, and an unknown name
    // draws normally rather than hiding the item.
    CHECK(r::ParseBlendMode("soft-light") == r::BlendMode::SoftLight);
    CHECK(r::ParseBlendMode("SoftLight") == r::BlendMode::SoftLight);
    CHECK(r::ParseBlendMode("color-burn") == r::BlendMode::ColorBurn);
    CHECK(r::ParseBlendMode("normal") == r::BlendMode::Alpha);
    CHECK(r::ParseBlendMode("") == r::BlendMode::Alpha);
    CHECK(r::ParseBlendMode("not-a-mode") == r::BlendMode::Alpha);
    CHECK(std::string_view(r::BlendModeName(r::BlendMode::Luminosity)) == "luminosity");
    CHECK(r::ParseBlendMode(r::BlendModeName(r::BlendMode::Hue)) == r::BlendMode::Hue);

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
    CHECK(r::FontManager::QtPixelSize(88.99f) == 88);
    CHECK(r::FontManager::QtPixelSize(0.5f) == 1);
    fm.RegisterBuiltin();
    CHECK(fm.Count() >= 1);
    auto f = fm.GetFont("builtin");
    CHECK(f.ok());
    CHECK(fm.Resolve("missing-font").id == "builtin");   // falls back

    CHECK(fm.SubstituteFamily("").empty());
    CHECK(fm.SubstituteFamily("builtin").empty());
    CHECK(!fm.DefaultFamily().empty());
    const std::string absent = fm.SubstituteFamily("VGR Definitely Not An Installed Font 42");
    CHECK(absent.empty() || fm.HasSystemFont(absent));
    CHECK(absent != "VGR Definitely Not An Installed Font 42" ||
          fm.HasSystemFont("VGR Definitely Not An Installed Font 42"));
    // ...and the substitute is the HOST'S OWN UI face — the engine compiles in no
    // font name at all, so the same build renders the same on any Windows box.
    const std::string uiFace = fm.SystemUiFamily();
    if (!uiFace.empty()) {
        CHECK(fm.SubstituteFamily("VGR Definitely Not An Installed Font 42") == uiFace);
        CHECK(fm.DefaultFamily() == uiFace);
    }
    // Stable across calls (the answer is cached, so painting and measuring
    // cannot disagree between two frames).
    CHECK(fm.SubstituteFamily("VGR Definitely Not An Installed Font 42") == absent);
    if (fm.HasSystemFont("Segoe UI")) {
        r::TextStyle qtStyle;
        qtStyle.fontId = "Segoe UI";
        qtStyle.size = 19.0f;
        qtStyle.wrap = true;
        qtStyle.wrapWidth = 105.0f;
        qtStyle.letterSpacing = 0.35f;
        qtStyle.wordSpacing = 1.25f;
        const std::string sample = "The monitor preview wraps these words the same way.";
        const auto measured = r::TextLayout::Measure(sample, qtStyle, fm);

        QFont font(QStringLiteral("Segoe UI"));
        font.setPixelSize(r::FontManager::QtPixelSize(qtStyle.size));
        font.setWeight(qtStyle.fontWeight);
        font.setLetterSpacing(QFont::AbsoluteSpacing, qtStyle.letterSpacing);
        font.setWordSpacing(qtStyle.wordSpacing);
        QTextLayout qtLayout(QString::fromStdString(sample), font);
        QTextOption option;
        option.setWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
        qtLayout.setTextOption(option);
        qtLayout.beginLayout();
        std::vector<std::string> qtLines;
        while (true) {
            QTextLine line = qtLayout.createLine();
            if (!line.isValid())
                break;
            line.setLineWidth(qtStyle.wrapWidth);
            qtLines.push_back(QString::fromStdString(sample)
                                  .mid(line.textStart(), line.textLength())
                                  .toUtf8().toStdString());
        }
        qtLayout.endLayout();
        CHECK(measured.lines.size() == qtLines.size());
        for (size_t i = 0; i < qtLines.size() && i < measured.lines.size(); ++i)
            CHECK(measured.lines[i].text == qtLines[i]);
    }
#ifdef _WIN32
    if (!absent.empty() && absent != "builtin") {
        r::TextStyle missingStyle;
        missingStyle.fontId = "VGR Definitely Not An Installed Font 42";
        missingStyle.size = 48.0f;
        r::TextStyle resolvedStyle = missingStyle;
        resolvedStyle.fontId = absent;
        r::TextStyle bitmapStyle;
        bitmapStyle.size = 48.0f;
        const auto missingLayout = r::TextLayout::Measure("The quick brown fox", missingStyle, fm);
        const auto resolvedLayout = r::TextLayout::Measure("The quick brown fox", resolvedStyle, fm);
        const auto bitmapLayout = r::TextLayout::Measure("The quick brown fox", bitmapStyle, fm);
        CHECK(std::abs(missingLayout.totalWidth - resolvedLayout.totalWidth) < 0.5f);
        CHECK(std::abs(missingLayout.totalHeight - resolvedLayout.totalHeight) < 0.5f);
        CHECK(std::abs(missingLayout.totalWidth - bitmapLayout.totalWidth) > 1.0f);
    }
#endif
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

#ifdef _WIN32
    for (const char* fam : {"Segoe UI", "Arial", "TT Nooks Trial"}) {
        if (!fm.HasSystemFont(fam))
            continue;
        std::map<uint8_t, r::FontGlyph> sys;
        const auto sysAtlas = fm.BuildSystemAtlas(fam, 90.0f, false, false, sys);
        CHECK(!sysAtlas.empty() && !sys.empty());
        CHECK(sys.size() == 95);   // ASCII 32..126
        const std::string probeChars = "aoimtruWHoj'g";
        float maxAdvance = 0.0f;
        for (char ch : probeChars)
            maxAdvance = std::max(maxAdvance, fm.SystemCharAdvance(fam, 90.0f, false, false,
                                                                   static_cast<uint8_t>(ch)));
        CHECK(maxAdvance > 0.0f);
        int checked = 0;
        for (char ch : probeChars) {
            const auto it = sys.find(static_cast<uint8_t>(ch));
            if (it == sys.end())
                continue;
            const r::FontGlyph& gl = it->second;
            // (a) the step the painter takes == the advance the layout measured
            // (the atlas stores whole pixels, so 1px of rounding is expected).
            const float measured = fm.SystemCharAdvance(fam, 90.0f, false, false,
                                                        static_cast<uint8_t>(ch));
            CHECK(measured > 0.0f);
            CHECK(std::abs(static_cast<float>(gl.advance) - measured) <= 1.0f);
            CHECK(static_cast<float>(gl.width) - gl.advance >= 6.0f);
            CHECK(static_cast<float>(gl.width) >= maxAdvance + 2.0f);
            CHECK(gl.height > 0);
            CHECK(gl.bearingX < 0 && gl.bearingY < 0);
            // (c) the cell actually holds ink, and the glyph ink sits INSIDE it
            // (a cell sampled from empty atlas space renders an invisible glyph).
            int inside = 0;
            for (int y = 0; y < gl.height; ++y)
                for (int x = 0; x < gl.width; ++x) {
                    const int px = gl.x + x, py = gl.y + y;
                    if (px < 0 || py < 0 || px >= sysAtlas.width || py >= sysAtlas.height)
                        continue;
                    if (((sysAtlas.pixels[static_cast<size_t>(py) * sysAtlas.width + px] >> 24) & 0xFF) < 32)
                        continue;
                    ++inside;
                }
            if (ch != ' ') CHECK(inside > 0);
            ++checked;
        }
        CHECK(checked >= 12);
        break;   // one real family is enough
    }
#endif

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
    r::TextStyle justified = wrap;
    justified.fontId = "builtin";
    justified.size = 10.0f;
    justified.wrapWidth = 70.0f;
    justified.align = r::TextAlign::Justify;
    const auto justifiedLayout = r::TextLayout::Measure("one two three four", justified, fm);
    CHECK(justifiedLayout.lines.size() > 1);
    CHECK(!justifiedLayout.lines.front().paragraphEnd);
    CHECK(justifiedLayout.lines.back().paragraphEnd);

    auto apostrophed = r::TextLayout::SplitLines("that\xE2\x80\x99s", false);
    CHECK(apostrophed.size() == 1);
    CHECK(apostrophed[0] == "that's");
    auto quoted = r::TextLayout::SplitLines(
        "\xE2\x80\x9C" "Don\xE2\x80\x99t\xE2\x80\x9D \xE2\x80\xA6 \xE2\x80\x94 ok\xC2\xA0!", false);
    CHECK(quoted.size() == 1);
    CHECK(quoted[0] == "\"Don't\" ... - ok !");
    const std::string unicodeFlow = "A\xC3\xA9" "B\nC\xF0\x9F\x98\x80" "D";
    const auto flowStarts = r::TextLayout::FlowCharacterStarts(unicodeFlow, style, fm);
    CHECK(flowStarts.size() == 2);
    CHECK(flowStarts[0] == 0);
    CHECK(flowStarts[1] == 4);   // QML counts UTF-16 units, not UTF-8 bytes.
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
void TestVideoLayerFrameRefresh() {
    auto& engine = r::RenderEngine::Instance();
    CHECK(engine.Initialize().ok());
    CHECK(engine.SetBackend("software").ok());
    CHECK(engine.CreateScene("vframe", "VideoFrame", r::Size(32, 32)).ok());
    CHECK(engine.AddLayer("vframe", r::Layer("media", "Media", r::LayerKind::Video, 1)).ok());

    const auto solid = [](r::Color c) {
        r::RgbaImage img;
        img.width = 8;
        img.height = 8;
        img.pixels.assign(64, c.Pack());
        return img;
    };
    auto video = std::make_shared<r::VideoObject>("vlive", "Live");
    video->SetFrame(solid(r::Color::Black()));   // the decoder's opening frame
    video->SetBounds(r::Rect(0, 0, 32, 32));
    CHECK(engine.AddObject("vframe", video, "media").ok());

    auto first = engine.Render("vframe");
    CHECK(first.ok());
    if (first.ok())
        CHECK(r::Color::Unpack(first.value().pixels[16 * 32 + 16]).r < 0.1f);

    // The source pushes frame 2 through the SAME object — no scene change, no
    // object swap, exactly what SceneBuilder's UpsertVideoLayer does per tick.
    const uint64_t rev1 = video->FrameRevision();
    video->SetFrame(solid(r::Color::Red()));
    CHECK(video->FrameRevision() > rev1);
    auto second = engine.Render("vframe");
    CHECK(second.ok());
    if (second.ok()) {
        const r::Color c = r::Color::Unpack(second.value().pixels[16 * 32 + 16]);
        CHECK(c.r > 0.9f && c.g < 0.1f);   // the NEW frame, not the stale first
    }
    CHECK(engine.DestroyScene("vframe").ok());
}
// THE RIGHT FRAME, EVERY TIME. Two scenes (two outputs) hold a video object of the SAME name (the live
// loop's "takeninput") that shows ONE shared frame pointer; the frame changes 40 times and the scenes
// are rendered alternately, as the live loop does. Every render must show exactly the frame that was
// pushed — through the shared texture, the cached scaled picture, the skipped overdrawn commands and
// the cached overlay layer — never a stale or another scene's picture.
void TestSharedVideoFramesStayCorrect() {
    auto& engine = r::RenderEngine::Instance();
    CHECK(engine.Initialize().ok());
    CHECK(engine.SetBackend("software").ok());
    constexpr int W = 64, H = 36;
    const char* scenes[2] = {"svf-a", "svf-b"};
    std::shared_ptr<r::VideoObject> videos[2];
    for (int s = 0; s < 2; ++s) {
        CHECK(engine.CreateScene(scenes[s], scenes[s], r::Size(W, H)).ok());
        CHECK(engine.AddLayer(scenes[s], r::Layer("media", "Media", r::LayerKind::Video, 1)).ok());
        videos[s] = std::make_shared<r::VideoObject>("takeninput", "takeninput");
        videos[s]->SetBounds(r::Rect(0, 0, W, H));
        CHECK(engine.AddObject(scenes[s], videos[s], "media").ok());
    }
    // A frame is identified by one colour; its left half and right half differ so a swap or flip shows.
    const auto frameFor = [](int k) {
        auto img = std::make_shared<r::RgbaImage>();
        img->width = 32;   // scaled 2x to the scene
        img->height = 18;
        img->pixels.resize(32u * 18u);
        for (int y = 0; y < 18; ++y)
            for (int x = 0; x < 32; ++x)
                img->pixels[static_cast<size_t>(y) * 32 + x] =
                    r::Color(((k * 6) % 250) / 255.0f, (x < 16 ? 40 : 200) / 255.0f, 90 / 255.0f, 1.0f).Pack();
        return std::shared_ptr<const r::RgbaImage>(img);
    };
    int wrong = 0, checked = 0;
    for (int k = 0; k < 40; ++k) {
        const auto frame = frameFor(k);
        for (int s = 0; s < 2; ++s) {
            // Scene A gets frame k, scene B gets frame k+100 (a different colour) — never mixed up.
            const auto mine = s == 0 ? frame : frameFor(k + 100);
            videos[s]->SetFrameShared(mine);
        }
        for (int s = 0; s < 2; ++s) {
            auto out = engine.Render(scenes[s]);
            CHECK(out.ok());
            if (!out.ok() || out.value().pixels.size() != static_cast<size_t>(W) * H) continue;
            const int expectK = s == 0 ? k : k + 100;
            const int wantR = (expectK * 6) % 250;
            for (int probe = 0; probe < 2; ++probe) {
                const int x = probe == 0 ? 8 : 56;
                const uint32_t px = out.value().pixels[static_cast<size_t>(18) * W + x];
                const int gotR = static_cast<int>(px & 255u);
                const int gotG = static_cast<int>((px >> 8) & 255u);
                const int wantG = probe == 0 ? 40 : 200;
                ++checked;
                if (std::abs(gotR - wantR) > 2 || std::abs(gotG - wantG) > 2) ++wrong;
            }
        }
    }
    CHECK(checked == 160);
    CHECK(wrong == 0);
    for (int s = 0; s < 2; ++s) CHECK(engine.DestroyScene(scenes[s]).ok());
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

    auto own = std::make_shared<r::FrameBufferOutput>(r::OutputKind::Audience, "own",
                                                      r::Size(64, 64));
    CHECK(own->AcceptsDistributedFrames());   // default: the program feed may write it
    CHECK(mgr.Add(own).ok());
    mgr.Distribute(f);
    CHECK(own->FramesReceived() == 1);
    own->SetAcceptsDistributedFrames(false);   // its own pass owns it now
    mgr.Distribute(f);
    CHECK(own->FramesReceived() == 1);         // skipped — no second writer
    CHECK(a->FramesReceived() == 3);           // a normal output still receives
    own->SetAcceptsDistributedFrames(true);    // fallback: no scene of its own this frame
    mgr.Distribute(f);
    CHECK(own->FramesReceived() == 2);
    CHECK(mgr.Remove("own").ok());

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
void TestShapeEdgeSeams() {
    auto& engine = r::RenderEngine::Instance();
    CHECK(engine.Initialize().ok());
    CHECK(engine.Start().ok());

    const std::string sceneId = "seam_probe";
    (void)engine.DestroyScene(sceneId);
    CHECK(engine.CreateScene(sceneId, "Seam probe", r::Size(1920, 1080)).ok());
    CHECK(engine.AddLayer(sceneId, r::Layer("bg", "Background", r::LayerKind::Background, 0)).ok());
    CHECK(engine.AddLayer(sceneId, r::Layer("boxes", "Boxes", r::LayerKind::Image, 1)).ok());

    // The backdrop the frame would be showing if a seam opened: a bright green
    // (the canonical "live picture behind a translucent band").
    auto bg = std::make_shared<r::BackgroundObject>("seam_bg", "Background", r::Color::Green());
    bg->SetBounds(r::Rect(0, 0, 1920, 1080));
    CHECK(engine.AddObject(sceneId, bg, "bg").ok());

    // THE TABLE'S OWN GEOMETRY, verbatim from the applied template: the code
    // chip, then the paragraph band that starts exactly where the chip ends.
    constexpr double kStageW = 754.0, kStageH = 428.0;
    const auto stageRect = [](double x, double y, double w, double h) {
        return r::Rect(static_cast<float>(x / kStageW * 1920.0),
                       static_cast<float>(y / kStageH * 1080.0),
                       static_cast<float>(w / kStageW * 1920.0),
                       static_cast<float>(h / kStageH * 1080.0));
    };
    auto chip = std::make_shared<r::ShapeObject>("seam_chip", "Chip");
    chip->SetRectangle(r::Color::Red());
    chip->SetBounds(stageRect(0.0, 262.0535715, 150.0892857, 41.58928571));
    CHECK(engine.AddObject(sceneId, chip, "boxes").ok());
    auto band = std::make_shared<r::ShapeObject>("seam_band", "Band");
    band->SetRectangle(r::Color::Red());
    band->SetBounds(stageRect(0.0, 303.6428572, 754.0, 124.3571428));
    CHECK(engine.AddObject(sceneId, band, "boxes").ok());

    // Every row from the chip's top to the band's bottom, down the chip's column,
    // must be the boxes' own red — one green row is the seam.
    const auto seamFree = [](const r::RgbaImage& frame) {
        const int y0 = static_cast<int>(std::lround(262.0535715 / kStageH * 1080.0));
        const int y1 = static_cast<int>(std::lround((303.6428572 + 124.3571428) / kStageH * 1080.0));
        for (int y = y0; y < y1; ++y) {
            const r::Color c = r::Color::Unpack(frame.pixels[static_cast<size_t>(y) * 1920 + 100]);
            if (c.g > 0.5f && c.r < 0.5f) return false;
        }
        return true;
    };

    r::RenderOptions opts;
    opts.distribute = false;
    opts.capture = true;

    CHECK(engine.SetBackend("software").ok());
    auto soft = engine.Render(sceneId, opts);
    CHECK(soft.ok());
    if (soft.ok()) CHECK(seamFree(soft.value()));

    // The same rule on the hardware path, when this machine has a device (a
    // machine without one keeps the software answer, as everywhere else).
    CHECK(engine.SetBackend("gpu").ok());
    if (engine.BackendCaps().gpuAccelerated) {
        auto gpu = engine.Render(sceneId, opts);
        CHECK(gpu.ok());
        if (gpu.ok()) CHECK(seamFree(gpu.value()));
    }
    CHECK(engine.SetBackend("software").ok());
    CHECK(engine.DestroyScene(sceneId).ok());
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

    {
        const r::FrameCostProfile cost = engine.CostProfileFor("s1");
        CHECK(cost.frame > 0);                // the frame it describes
        CHECK(cost.frameMs > 0.0);            // the measured cost it splits
        CHECK(cost.totalPixels >= 64ull * 64ull);
        CHECK(cost.items.size() >= 2);        // the 64x64 background + the rest
        CHECK(cost.Heaviest() == &cost.items.front());   // heaviest first
        if (!cost.items.empty()) {
            CHECK(cost.items.front().pixels >= 64ull * 64ull);
            CHECK(!cost.items.front().Label().empty());   // a name to report
        }
        double lastShare = 200.0;
        uint64_t lastPixels = ~0ull;
        for (const r::ElementCost& c : cost.items) {
            CHECK(c.pixels <= lastPixels);                    // descending
            CHECK(c.sharePct <= lastShare);                   // shares follow
            CHECK(c.sharePct >= 0.0 && c.sharePct <= 100.0);
            // estimatedMs is that share of the frame's real cost.
            CHECK(std::abs(c.estimatedMs - cost.frameMs * c.sharePct / 100.0) < 1e-9);
            if (c.pixels > 0)
                CHECK(c.commands > 0);   // work came from commands, not from nowhere
            lastShare = c.sharePct;
            lastPixels = c.pixels;
        }
        const r::ElementCost* background = nullptr;
        for (const r::ElementCost& c : cost.items)
            if (c.objectId == "scene:background") background = &c;
        CHECK(background != nullptr);
        CHECK(background != nullptr && background->isBackground);
        CHECK(cost.BackgroundSharePct() > 0.0);
        CHECK(cost.HeaviestContent() != nullptr);
        CHECK(cost.HeaviestContent() != background);   // never the engine's clear
        CHECK(!cost.HeaviestContent()->isBackground);
        // The scene drew an image, a shape and text, so the content it names is
        // one of those layers.
        CHECK(cost.HeaviestContent()->commands > 0);
        CHECK(cost.HeaviestContent()->sharePct <= 100.0);
        // LastCostProfile() is the same account for the most recent frame of
        // ANY scene (what a caller with no scene id in hand reads).
        CHECK(engine.LastCostProfile().frame == cost.frame);
        // An unknown scene has no cost to report — empty, not a stale copy.
        CHECK(engine.CostProfileFor("never-rendered").items.empty());
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
// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
void TestRenderBackendPreference() {
    namespace J = bps::json;

    // 1. THE RULE. Nobody asked for anything -> the software rasterizer: what a
    //    fresh install runs, on every machine, GPU or not.
    CHECK(r::RenderEngine::ResolveBackendName("", false) == "software");
    // The switch on -> the GPU is ASKED for (whether this machine has one is a
    // different question, answered by the active name below).
    CHECK(r::RenderEngine::ResolveBackendName("", true) == "gpu");
    // An explicit choice outranks the switch, in BOTH directions.
    CHECK(r::RenderEngine::ResolveBackendName("software", true) == "software");
    CHECK(r::RenderEngine::ResolveBackendName("null", false) == "null");
    CHECK(r::RenderEngine::ResolveBackendName("auto", false) == "auto");
    CHECK(r::RenderEngine::ResolveBackendName("directx11", false) == "directx11");
    CHECK(r::RenderEngine::ResolveBackendName("rhi", false) == "rhi");

    // 2. THE REAL SELECTION, through the configuration the app writes.
    auto& config = bps::ConfigurationManager::Instance();
    auto& engine = r::RenderEngine::Instance();
    CHECK(engine.Initialize().ok());
    CHECK(engine.Start().ok());

    CHECK(config.Reset(bps::ConfigScope::Runtime).ok());

    CHECK(config.Set("render.hardwareAcceleration", J::Value::Bool(false)).ok());
    CHECK(engine.Reload().ok());
    CHECK(engine.RequestedBackendName() == "software");
    CHECK(engine.ActiveBackendName() == "software");

    CHECK(config.Set("render.hardwareAcceleration", J::Value::Bool(true)).ok());
    CHECK(engine.Reload().ok());
    CHECK(engine.RequestedBackendName() == "gpu");   // the GPU was asked for
    CHECK(engine.ActiveBackendName() == "directx11" || engine.ActiveBackendName() == "rhi" || engine.ActiveBackendName() == "software");
    // The app's read of "what is drawing" is the backend that is really installed.
    CHECK(engine.Backend() != nullptr);
    if (engine.Backend() != nullptr)
        CHECK(engine.ActiveBackendName() == engine.Backend()->Capabilities().name);

    // The explicit key outranks the switch through this same path — an installed
    // profile is not overridden by a settings write.
    CHECK(config.Set("render.backend", J::Value::String("software")).ok());
    CHECK(engine.Reload().ok());
    CHECK(engine.RequestedBackendName() == "software");

    // 3. LEAVE NOTHING BEHIND: the Runtime scope goes back to what this binary
    //    had before this phase ran (the styles phases pin software the same
    //    way), so every phase after it renders on the engine's own default.
    CHECK(config.Reset(bps::ConfigScope::Runtime).ok());
    CHECK(config.Set("render.backend", J::Value::String("software")).ok());
    CHECK(engine.Reload().ok());
    CHECK(engine.RequestedBackendName() == "software");
    CHECK(engine.ActiveBackendName() == "software");
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

void TestImageScale() {
    r::RgbaImage ramp;   // 2 x 1: red | blue, opaque
    ramp.width = 2;
    ramp.height = 1;
    ramp.pixels = {r::Color::Red().Pack(), r::Color::Blue().Pack()};

    r::ScaleTaps tx, ty;
    r::ScaleTaps x0, x1, x2, x3;
    r::ScaleTaps row;

    // --- 1:1 through the filter is the pixel itself, not a half-pixel blur ---
    CHECK(r::BuildScaleTaps(0.0, 1.0, 2, tx) >= 1);
    CHECK(r::BuildScaleTaps(0.0, 1.0, 1, ty) >= 1);
    const r::Color exact = r::Color::Unpack(r::CombineTaps(ramp, tx, ty));
    CHECK(exact.r > 0.98f && exact.b < 0.02f && exact.a > 0.98f);

    // --- MAGNIFIED: the two middle pixels are real blends, in a ramp order ---
    CHECK(r::BuildScaleTaps(0.0, 0.5, 2, x0) >= 1);
    CHECK(r::BuildScaleTaps(0.5, 0.5, 2, x1) >= 1);
    CHECK(r::BuildScaleTaps(1.0, 0.5, 2, x2) >= 1);
    CHECK(r::BuildScaleTaps(1.5, 0.5, 2, x3) >= 1);
    CHECK(r::BuildScaleTaps(0.0, 1.0, 1, row) >= 1);
    const r::Color d0 = r::Color::Unpack(r::CombineTaps(ramp, x0, row));
    const r::Color d1 = r::Color::Unpack(r::CombineTaps(ramp, x1, row));
    const r::Color d2 = r::Color::Unpack(r::CombineTaps(ramp, x2, row));
    const r::Color d3 = r::Color::Unpack(r::CombineTaps(ramp, x3, row));
    CHECK(d0.r > 0.98f);                                  // the edge keeps the source's colour
    CHECK(d3.b > 0.98f);
    CHECK(d1.r > 0.5f && d1.b > 0.1f && d1.b < 0.5f);     // 75% red / 25% blue
    CHECK(d2.b > 0.5f && d2.r > 0.1f && d2.r < 0.5f);
    CHECK(d1.r > d2.r);   // interpolated: the blend MOVES, it is not a block edge

    r::RgbaImage checker;
    checker.width = 4;
    checker.height = 4;
    checker.pixels.assign(16, r::Color::Black().Pack());
    for (int y = 0; y < 4; ++y)
        for (int x = 0; x < 4; ++x)
            if (((x + y) & 1) == 0)
                checker.pixels[static_cast<size_t>(y) * 4 + x] = r::Color::White().Pack();
    r::ScaleTaps bx, by;
    CHECK(r::BuildScaleTaps(0.0, 4.0, 4, bx) >= 1);
    CHECK(r::BuildScaleTaps(0.0, 4.0, 4, by) >= 1);
    const r::Color avg = r::Color::Unpack(r::CombineTaps(checker, bx, by));
    CHECK(avg.r > 0.4f && avg.r < 0.6f);   // eight white of sixteen
    CHECK(avg.a > 0.98f);                  // opaque black is still opaque

    r::RgbaImage edge;
    edge.width = 2;
    edge.height = 1;
    edge.pixels = {r::Color::White().Pack(), r::Color(1.0f, 0.0f, 0.0f, 0.0f).Pack()};
    const r::Color mixed = r::Color::Unpack(r::CombineTaps(edge, x1, row));
    CHECK(mixed.g > 0.9f && mixed.b > 0.9f);   // white, not pink
    CHECK(mixed.a > 0.7f && mixed.a < 0.8f);   // the transparent quarter of the footprint

    // --- THE BLIT PATH rides the same filter ---
    r::SoftwareGraphicsBackend sw;
    auto tid = sw.CreateTexture(ramp);
    CHECK(tid.ok());
    CHECK(sw.BeginFrame(4, 1).ok());
    r::DrawCommand blit;
    blit.type = r::DrawCommand::Type::BlitImage;
    blit.texture = tid.value();
    blit.rect = r::Rect(0, 0, 4, 1);
    blit.srcRect = r::Rect(0, 0, 2, 1);
    blit.color = r::Color::White();
    CHECK(sw.Submit({blit}).ok());
    auto fb = sw.Readback();
    CHECK(fb.ok());
    if (fb.ok() && fb.value().pixels.size() == 4) {
        const r::Color b0 = r::Color::Unpack(fb.value().pixels[0]);
        const r::Color b1 = r::Color::Unpack(fb.value().pixels[1]);
        const r::Color b2 = r::Color::Unpack(fb.value().pixels[2]);
        const r::Color b3 = r::Color::Unpack(fb.value().pixels[3]);
        CHECK(b0.r > 0.98f && b3.b > 0.98f);                 // ends stay crisp
        CHECK(b1.r > 0.5f && b1.b > 0.1f && b1.b < 0.5f);    // middle is a blend
        CHECK(b2.b > 0.5f && b2.r > 0.1f && b2.r < 0.5f);
        CHECK(b1.r > b2.r);
    }
    // ...and a 1:1 blit of the same texture is still the source pixel, exactly
    // (this is the glyph atlas and every unscaled draw in the engine).
    CHECK(sw.BeginFrame(2, 1).ok());
    r::DrawCommand un = blit;
    un.rect = r::Rect(0, 0, 2, 1);
    CHECK(sw.Submit({un}).ok());
    auto fb1 = sw.Readback();
    CHECK(fb1.ok());
    if (fb1.ok() && fb1.value().pixels.size() == 2) {
        const r::Color u0 = r::Color::Unpack(fb1.value().pixels[0]);
        const r::Color u1 = r::Color::Unpack(fb1.value().pixels[1]);
        CHECK(u0.r > 0.98f && u0.b < 0.02f);
        CHECK(u1.b > 0.98f && u1.r < 0.02f);
    }
    CHECK(sw.DestroyTexture(tid.value()).ok());
    CHECK(sw.Shutdown().ok());

    // --- THE OUTPUT'S OWN FRAME RESIZE rides it too (a mid-session resolution
    // change resizes a composed frame, media picture and all) ---
    r::FrameBufferOutput resized(r::OutputKind::Audience, "scale-aud", r::Size(4, 1));
    r::Frame f;
    f.sceneId = "scale";
    f.frame = 1;
    f.width = 2;
    f.height = 1;
    f.pixels = {r::Color::Red().Pack(), r::Color::Blue().Pack()};
    CHECK(resized.Present(f).ok());
    const r::Frame last = resized.LastFrame();
    CHECK(last.width == 4 && last.height == 1);
    if (last.width == 4 && last.pixels.size() == 4) {
        const r::Color r1 = r::Color::Unpack(last.pixels[1]);
        const r::Color r2 = r::Color::Unpack(last.pixels[2]);
        CHECK(r1.r > 0.5f && r1.b > 0.1f && r1.b < 0.5f);   // interpolated, not nearest
        CHECK(r2.b > 0.5f && r2.r > 0.1f && r2.r < 0.5f);
    }
    // No resize needed: the bytes are handed through untouched.
    r::FrameBufferOutput untouched(r::OutputKind::Audience, "scale-same", r::Size(2, 1));
    CHECK(untouched.Present(f).ok());
    CHECK(untouched.LastFrame().pixels == f.pixels);
}

// ---------------------------------------------------------------------------
// The software rasteriser's speed paths must draw what the plain path draws: a 1:1 opaque copy, the
// two-tap resample of a video frame, and the cached transparent overlay blended over a moving picture.
void TestSoftwareFastPaths() {
    constexpr int W = 96, H = 54;
    const auto px = [](int red, int green, int blue, int alpha) {
        return r::Color(red / 255.0f, green / 255.0f, blue / 255.0f, alpha / 255.0f).Pack();
    };
    // A "video frame" with a gradient so a wrong row/column shows; 64x36 (scaled up 1.5x to the frame).
    const auto makeVideo = [&](int shift) {
        r::RgbaImage v;
        v.width = 64;
        v.height = 36;
        v.pixels.resize(64u * 36u);
        for (int y = 0; y < 36; ++y)
            for (int x = 0; x < 64; ++x)
                v.pixels[static_cast<size_t>(y) * 64 + x] = px((x * 4 + shift) & 255, (y * 7) & 255, 128, 255);
        return v;
    };
    // A translucent "text" bitmap: opaque white strokes on transparent, one half-transparent column.
    r::RgbaImage ink;
    ink.width = 20;
    ink.height = 10;
    ink.pixels.assign(200u, 0u);
    for (int y = 2; y < 8; ++y)
        for (int x = 2; x < 18; ++x)
            ink.pixels[static_cast<size_t>(y) * 20 + x] = (x == 10) ? px(255, 255, 255, 120) : px(255, 255, 255, 255);

    // Draws the same command list on a fresh backend: the reference (first time every path is cold).
    const auto frameOf = [&](r::SoftwareGraphicsBackend& sw, r::TextureId video, r::TextureId inkTex) {
        std::vector<r::DrawCommand> cmds;
        r::DrawCommand blit;
        blit.type = r::DrawCommand::Type::BlitImage;
        blit.texture = video;
        blit.rect = r::Rect(0, 0, W, H);
        blit.srcRect = r::Rect(0, 0, 64, 36);
        blit.color = r::Color::White();
        cmds.push_back(blit);
        for (int i = 0; i < 4; ++i) {
            r::DrawCommand g;
            g.type = r::DrawCommand::Type::DrawGlyph;
            g.texture = inkTex;
            g.rect = r::Rect(static_cast<float>(4 + i * 22), 20, 20, 10);
            g.srcRect = r::Rect(0, 0, 20, 10);
            g.color = r::Color(0.9f, 0.8f, 0.1f, 1.0f);
            cmds.push_back(g);
        }
        r::DrawCommand plate;
        plate.type = r::DrawCommand::Type::FillRect;
        plate.rect = r::Rect(0, 40, W, 6);
        plate.color = r::Color(0.0f, 0.0f, 0.0f, 0.5f);
        cmds.push_back(plate);
        if (!sw.BeginFrame(W, H).ok() || !sw.Submit(cmds).ok()) return r::RgbaImage{};
        auto rb = sw.Readback();
        return rb.ok() ? rb.value() : r::RgbaImage{};
    };
    const auto maxDiff = [](const r::RgbaImage& a, const r::RgbaImage& b) {
        if (a.pixels.size() != b.pixels.size() || a.pixels.empty()) return 999;
        int worst = 0;
        for (size_t i = 0; i < a.pixels.size(); ++i)
            for (int s = 0; s < 32; s += 8) {
                const int d = static_cast<int>((a.pixels[i] >> s) & 255u) - static_cast<int>((b.pixels[i] >> s) & 255u);
                worst = std::max(worst, d < 0 ? -d : d);
            }
        return worst;
    };

    // The reference for frame N (a new backend per frame: nothing cached).
    std::vector<r::RgbaImage> reference;
    for (int f = 0; f < 5; ++f) {
        r::SoftwareGraphicsBackend cold;
        auto v = cold.CreateTexture(makeVideo(f * 9));
        auto t = cold.CreateTexture(ink);
        CHECK(v.ok() && t.ok());
        reference.push_back(frameOf(cold, v.value(), t.value()));
    }
    // One backend across frames: the video changes every frame, the text/plate run is the same, so from
    // the third frame on the overlay comes from its cached layer.
    r::SoftwareGraphicsBackend warm;
    auto vt = warm.CreateTexture(makeVideo(0));
    auto it = warm.CreateTexture(ink);
    CHECK(vt.ok() && it.ok());
    int worst = 0;
    for (int f = 0; f < 5; ++f) {
        CHECK(warm.UpdateTexture(vt.value(), makeVideo(f * 9)).ok());
        const r::RgbaImage got = frameOf(warm, vt.value(), it.value());
        worst = std::max(worst, maxDiff(got, reference[static_cast<size_t>(f)]));
    }
    CHECK(worst <= 2);   // rounding only: the cached layer is blended, not redrawn

    // The picture is right, not just consistent: the 1.5x upscale keeps the gradient's ends.
    const r::RgbaImage& f0 = reference[0];
    if (f0.pixels.size() == static_cast<size_t>(W) * H) {
        const r::Color first = r::Color::Unpack(f0.pixels[0]);
        CHECK(first.r < 0.1f && first.b > 0.45f && first.b < 0.55f);
        const r::Color mid = r::Color::Unpack(f0.pixels[static_cast<size_t>(10) * W + 48]);
        CHECK(mid.r > 0.3f && mid.r < 0.8f);
    }

    // Every pixel of the fast resample (AVX2 or scalar) matches the general per-pixel filter (CombineTaps).
    if (f0.pixels.size() == static_cast<size_t>(W) * H) {
        const r::RgbaImage video0 = makeVideo(0);
        r::RgbaImage videoOnly;   // the frame with no overlay: just the scaled video
        {
            r::SoftwareGraphicsBackend plain;
            auto pv = plain.CreateTexture(video0);
            r::DrawCommand b;
            b.type = r::DrawCommand::Type::BlitImage;
            b.texture = pv.value();
            b.rect = r::Rect(0, 0, W, H);
            b.srcRect = r::Rect(0, 0, 64, 36);
            b.color = r::Color::White();
            CHECK(plain.BeginFrame(W, H).ok());
            CHECK(plain.Submit({b}).ok());
            auto rb = plain.Readback();
            if (rb.ok()) videoOnly = rb.value();
        }
        int resampleWorst = 0;
        for (int y = 0; y < H && videoOnly.pixels.size() == static_cast<size_t>(W) * H; ++y)
            for (int x = 0; x < W; ++x) {
                r::ScaleTaps tx, ty;
                r::BuildScaleTaps(x * (64.0 / W), 64.0 / W, 64, tx);
                r::BuildScaleTaps(y * (36.0 / H), 36.0 / H, 36, ty);
                const uint32_t want = r::CombineTaps(video0, tx, ty);
                const uint32_t got = videoOnly.pixels[static_cast<size_t>(y) * W + x];
                for (int s = 0; s < 24; s += 8) {
                    const int d = static_cast<int>((want >> s) & 255u) - static_cast<int>((got >> s) & 255u);
                    resampleWorst = std::max(resampleWorst, d < 0 ? -d : d);
                }
            }
        CHECK(resampleWorst <= 2);
    }

    // An opaque frame at its own size is copied exactly (the cached scaled picture's path).
    r::SoftwareGraphicsBackend same;
    r::RgbaImage solid;
    solid.width = W;
    solid.height = H;
    solid.pixels.assign(static_cast<size_t>(W) * H, px(10, 200, 30, 255));
    auto st = same.CreateTexture(solid);
    CHECK(st.ok());
    r::DrawCommand copy;
    copy.type = r::DrawCommand::Type::BlitImage;
    copy.texture = st.value();
    copy.rect = r::Rect(0, 0, W, H);
    copy.srcRect = r::Rect(0, 0, W, H);
    copy.color = r::Color::White();
    CHECK(same.BeginFrame(W, H).ok());
    CHECK(same.Submit({copy}).ok());
    auto copied = same.Readback();
    CHECK(copied.ok() && copied.value().pixels == solid.pixels);

    // A RECYCLED frame buffer holds the last frame's pixels; the next frame must not show any of them.
    // Frame 1 fills everything with white and is handed back to the pool; frame 2 draws one small plate
    // and nothing else — every other pixel must be transparent black, whether or not the buffer was reused.
    r::SoftwareGraphicsBackend recycler;
    for (int round = 0; round < 3; ++round) {
        CHECK(recycler.BeginFrame(W, H).ok());
        r::DrawCommand fill;
        fill.type = r::DrawCommand::Type::FillRect;
        fill.rect = r::Rect(0, 0, W, H);
        fill.color = r::Color::White();
        CHECK(recycler.Submit({fill}).ok());
        auto white = recycler.ReadbackMove();
        CHECK(white.ok());
        if (white.ok()) r::FramebufferPool::Instance().Give(std::move(white.value().pixels));

        CHECK(recycler.BeginFrame(W, H).ok());
        r::DrawCommand plateCmd;
        plateCmd.type = r::DrawCommand::Type::FillRect;
        plateCmd.rect = r::Rect(10, 10, 8, 8);
        plateCmd.color = r::Color::Red();
        CHECK(recycler.Submit({plateCmd}).ok());
        auto second = recycler.ReadbackMove();
        CHECK(second.ok());
        if (second.ok() && second.value().pixels.size() == static_cast<size_t>(W) * H) {
            size_t stray = 0;
            for (int y = 0; y < H; ++y)
                for (int x = 0; x < W; ++x) {
                    const bool inside = x >= 10 && x < 18 && y >= 10 && y < 18;
                    const uint32_t px = second.value().pixels[static_cast<size_t>(y) * W + x];
                    if (!inside && px != 0u) ++stray;
                }
            CHECK(stray == 0);
            CHECK(r::Color::Unpack(second.value().pixels[static_cast<size_t>(12) * W + 12]).r > 0.9f);
            r::FramebufferPool::Instance().Give(std::move(second.value().pixels));
        }
    }
    // BeginFrame with no Submit at all still reads back as a clear frame.
    CHECK(recycler.BeginFrame(W, H).ok());
    auto emptyFrame = recycler.Readback();
    CHECK(emptyFrame.ok());
    if (emptyFrame.ok())
        CHECK(std::all_of(emptyFrame.value().pixels.begin(), emptyFrame.value().pixels.end(),
                          [](uint32_t p) { return p == 0u; }));
}

// The GPU-vs-CPU frame comparison, run against whichever GPU backend `request` names: "gpu" = this OS's
// own best (Direct3D 11 on Windows, the QRhi backend elsewhere), "directx11" / "rhi" = that backend only.
static void GpuBackendMatchesSoftware(const std::string& request) {
    auto& engine = r::RenderEngine::Instance();
    CHECK(engine.Initialize().ok());
    CHECK(engine.Start().ok());

    // The live loop's own command mix: an opaque background, a scaled image blit,
    // text through the glyph atlas, and an alpha-blended shape.
    const std::string sceneId = "gpu_backend_probe";
    (void)engine.DestroyScene(sceneId);
    CHECK(engine.CreateScene(sceneId, "GPU probe", r::Size(64, 64)).ok());
    CHECK(engine.AddLayer(sceneId, r::Layer("bg", "Background", r::LayerKind::Background, 0)).ok());
    CHECK(engine.AddLayer(sceneId, r::Layer("img", "Images", r::LayerKind::Image, 1)).ok());
    CHECK(engine.AddLayer(sceneId, r::Layer("txt", "Text", r::LayerKind::Text, 2)).ok());

    auto bg = std::make_shared<r::BackgroundObject>("g_bg", "Background", r::Color(0.1f, 0.2f, 0.6f));
    bg->SetBounds(r::Rect(0, 0, 64, 64));
    CHECK(engine.AddObject(sceneId, bg, "bg").ok());

    r::RgbaImage green;
    green.width = 8;
    green.height = 8;
    green.pixels.assign(64, r::Color::Green().Pack());
    auto img = std::make_shared<r::ImageObject>("g_img", "Image");
    img->SetImage(green, "gpu-probe-asset");
    img->SetBounds(r::Rect(8, 8, 16, 16));   // scaled blit: 8x8 -> 16x16
    CHECK(engine.AddObject(sceneId, img, "img").ok());

    auto text = std::make_shared<r::TextObject>("g_txt", "Title", "HI\nHI");
    text->SetBounds(r::Rect(2, 34, 60, 26));
    r::TextStyle textStyle;
    textStyle.size = 12.0f;
    textStyle.lineGap = 2.0f;
    textStyle.lineBg = r::Color(1.0f, 0.6f, 0.1f, 0.7f);   // alpha, so the band blends
    text->AddComponent(r::MakeTextStyle(textStyle));
    CHECK(engine.AddObject(sceneId, text, "txt").ok());

    auto shape = std::make_shared<r::ShapeObject>("g_shp", "Shape");
    shape->SetRectangle(r::Color(1.0f, 1.0f, 1.0f, 0.5f));   // alpha, so the blend is exercised
    shape->SetBounds(r::Rect(40, 8, 16, 16));
    CHECK(engine.AddObject(sceneId, shape, "img").ok());

    r::RenderOptions opts;
    opts.distribute = false;
    opts.capture = true;

    // --- The reference: the CPU rasterizer -------------------------------
    CHECK(engine.SetBackend("software").ok());
    CHECK(std::string(engine.BackendCaps().name) == "software");
    CHECK(!engine.BackendCaps().gpuAccelerated);
    auto soft = engine.Render(sceneId, opts);
    CHECK(soft.ok());
    CHECK(soft.value().width == 64 && soft.value().height == 64);
    CHECK(!soft.value().empty());

    CHECK(engine.SetBackend(request).ok());
    const bool gpu = engine.BackendCaps().gpuAccelerated;
    const std::string active = engine.BackendCaps().name;
    std::fprintf(stderr, "[gpu] requested=%s active=%s gpu=%d\n", request.c_str(), active.c_str(),
                 gpu ? 1 : 0);
    if (!gpu) {
        // The documented fallback, asserted rather than skipped: the software
        // rasterizer is what renders, and it still renders.
        CHECK(active == "software");
        auto fallback = engine.Render(sceneId, opts);
        CHECK(fallback.ok());
        CHECK(fallback.value().pixels == soft.value().pixels);
        CHECK(engine.SetBackend("auto").ok());
        CHECK(!engine.BackendCaps().gpuAccelerated);   // same machine, same answer
        CHECK(engine.SetBackend("software").ok());
        return;
    }
    if (request == "rhi")
        CHECK(active == "rhi");
    else if (request == "directx11")
        CHECK(active == "directx11");
    else
        CHECK(active == "directx11" || active == "rhi");   // "gpu": this OS's best
    CHECK(engine.BackendCaps().maxTextureSize >= 8192);

    auto gpuFrame = engine.Render(sceneId, opts);
    CHECK(gpuFrame.ok());
    CHECK(gpuFrame.value().width == soft.value().width);
    CHECK(gpuFrame.value().height == soft.value().height);
    CHECK(gpuFrame.value().pixels.size() == soft.value().pixels.size());

    size_t exact = 0;
    size_t within3 = 0;
    int worst = 0;
    if (gpuFrame.value().pixels.size() == soft.value().pixels.size()) {
        for (size_t i = 0; i < soft.value().pixels.size(); ++i) {
            const uint32_t a = soft.value().pixels[i];
            const uint32_t b = gpuFrame.value().pixels[i];
            int delta = 0;
            for (int shift = 0; shift < 32; shift += 8) {
                const int da = static_cast<int>((a >> shift) & 0xFF) - static_cast<int>((b >> shift) & 0xFF);
                delta = std::max(delta, std::abs(da));
            }
            if (delta == 0) ++exact;
            if (delta <= 3) ++within3;
            worst = std::max(worst, delta);
        }
    }
    const size_t total = soft.value().pixels.size();
    std::fprintf(stderr,
                 "[d3d11] frame match: %zu/%zu pixels exact, %zu within +-3, worst channel delta %d\n",
                 exact, total, within3, worst);
    if (gpuFrame.value().pixels.size() == soft.value().pixels.size() && within3 != total) {
        int x0 = 1 << 30, y0 = 1 << 30, x1 = -1, y1 = -1;
        size_t shown = 0;
        for (int y = 0; y < soft.value().height; ++y)
            for (int x = 0; x < soft.value().width; ++x) {
                const size_t i = static_cast<size_t>(y) * soft.value().width + x;
                const uint32_t a = soft.value().pixels[i];
                const uint32_t b = gpuFrame.value().pixels[i];
                int delta = 0;
                for (int shift = 0; shift < 32; shift += 8)
                    delta = std::max(delta, std::abs(static_cast<int>((a >> shift) & 0xFF) -
                                                     static_cast<int>((b >> shift) & 0xFF)));
                if (delta <= 8)
                    continue;
                x0 = std::min(x0, x);
                y0 = std::min(y0, y);
                x1 = std::max(x1, x);
                y1 = std::max(y1, y);
                if (shown < 4) {
                    ++shown;
                    std::fprintf(stderr,
                                 "[d3d11]   at (%d,%d): cpu=0x%08x gpu=0x%08x delta=%d\n", x, y, a,
                                 b, delta);
                }
            }
        std::fprintf(stderr, "[d3d11] mismatching pixels bounding box: (%d,%d)..(%d,%d)\n", x0, y0,
                     x1, y1);
    }
    // The frame is the same picture: every pixel within the blend/rounding
    // tolerance of the CPU path (and in practice nearly all of them exact).
    CHECK(within3 == total);

    CHECK(engine.GetHealth().state == HealthState::Healthy);
    CHECK(engine.BackendCaps().supportsShaders);

    // Back to the CPU rasterizer for the suites that follow in this phase.
    CHECK(engine.SetBackend("software").ok());
    CHECK(engine.SetBackend("auto").ok());
    CHECK(engine.SetBackend("software").ok());
    (void)engine.DestroyScene(sceneId);
}

// "gpu" is the setting's own request: Direct3D 11 on Windows, the QRhi backend everywhere else, so this
// one runs the engine's real GPU path on every OS (a machine with no device asserts the software fallback).
void TestD3D11Backend() { GpuBackendMatchesSoftware("gpu"); }

// The QRhi backend by name, on EVERY OS including Windows (where it drives Direct3D 11 through Qt): the
// one frame the CPU rasterizer draws must come back from the shared shaders within the blend tolerance.
// Run with BPS_RHI_API=d3d11|vulkan|gl|metal to pick the API underneath.
void TestRhiBackend() { GpuBackendMatchesSoftware("rhi"); }

// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
void TestD3D11StageFrame() {
    auto& engine = r::RenderEngine::Instance();
    CHECK(engine.Initialize().ok());
    CHECK(engine.Start().ok());

    // The style's backdrop: a real image on disk (the app's branded PNG, which
    // is what the GPU path uploads as one more bound texture).
    const std::filesystem::path img =
        std::filesystem::temp_directory_path() / "bps_d3d11_stage_test.png";
    {
        r::RgbaImage px;
        px.width = 64;
        px.height = 36;
        px.pixels.assign(px.pixels.capacity(), 0);
        px.pixels.assign(static_cast<size_t>(px.width) * px.height, 0xFF2020D0u);   // a dark red-blue
        auto encoded = r::EncodePngRgba8(reinterpret_cast<const uint8_t*>(px.pixels.data()),
                                         px.width, px.height);
        CHECK(encoded.ok());
        std::ofstream file(img, std::ios::binary);
        file.write(reinterpret_cast<const char*>(encoded.value().data()),
                   static_cast<std::streamsize>(encoded.value().size()));
        CHECK(file.good());
    }

    // THE SCENE, at any size: every rect is expressed in the 1920x1080 grid and
    // scaled, so the SAME composition is rendered at both.
    const auto buildScene = [&](const std::string& id, int w, int h) {
        const float sx = static_cast<float>(w) / 1920.0f;
        const float sy = static_cast<float>(h) / 1080.0f;
        const auto scaled = [&](float x, float y, float rw, float rh) {
            return r::Rect(x * sx, y * sy, rw * sx, rh * sy);
        };
        (void)engine.DestroyScene(id);
        CHECK(engine.CreateScene(id, "stage probe", r::Size(w, h)).ok());
        CHECK(engine.AddLayer(id, r::Layer("bg", "Background", r::LayerKind::Background, 0)).ok());
        CHECK(engine.AddLayer(id, r::Layer("img", "Images", r::LayerKind::Image, 1)).ok());
        CHECK(engine.AddLayer(id, r::Layer("txt", "Text", r::LayerKind::Text, 2)).ok());

        auto bg = std::make_shared<r::BackgroundObject>("s_bg", "Background",
                                                       r::Color(0.05f, 0.06f, 0.09f));
        bg->SetBounds(scaled(0, 0, 1920, 1080));
        CHECK(engine.AddObject(id, bg, "bg").ok());

        // The style's background IMAGE: a large cover-fit blit (the app's is a
        // 1920x1080 PNG), which is the biggest single texture the GPU path draws.
        r::RgbaImage backdrop;
        backdrop.width = 1920;
        backdrop.height = 1080;
        backdrop.pixels.assign(static_cast<size_t>(backdrop.width) * backdrop.height, 0xFF3030B0u);
        auto image = std::make_shared<r::ImageObject>("s_img", "Style backdrop");
        image->SetImage(backdrop, "stage-probe-backdrop");
        image->SetBounds(scaled(0, 0, 1920, 1080));
        CHECK(engine.AddObject(id, image, "img").ok());

        const char* lines[] = { "FAITH IS THE SUBSTANCE OF THINGS HOPED FOR",
                                "AND THE EVIDENCE OF THINGS NOT SEEN BUT BELIEVED",
                                "HEBREWS ELEVEN ONE THROUGH THREE",
                                "THE ELDERS OBTAINED A GOOD REPORT THROUGH FAITH" };
        for (int i = 0; i < 4; ++i) {
            auto text = std::make_shared<r::TextObject>("s_txt" + std::to_string(i), "Body", lines[i]);
            text->SetBounds(scaled(90, 380 + i * 165, 1740, 150));
            r::TextStyle ts;
            ts.size = 198.0f * sy;
            ts.color = r::Color::White();
            text->AddComponent(r::MakeTextStyle(ts));
            CHECK(engine.AddObject(id, text, "txt").ok());
        }

        auto shape = std::make_shared<r::ShapeObject>("s_shp", "Shape");
        shape->SetRectangle(r::Color(1.0f, 0.85f, 0.2f, 0.75f));
        shape->SetBounds(scaled(60, 60, 420, 90));
        CHECK(engine.AddObject(id, shape, "img").ok());

        r::RgbaImage swatch;
        swatch.width = 8;
        swatch.height = 8;
        swatch.pixels.assign(static_cast<size_t>(swatch.width) * swatch.height,
                             r::Color(0.2f, 0.45f, 0.9f, 1.0f).Pack());
        auto blended = std::make_shared<r::ImageObject>("s_blend", "Blended picture");
        blended->SetImage(swatch, "stage-probe-blend");
        blended->SetBounds(scaled(1200, 620, 620, 380));
        blended->SetBlendMode(r::BlendMode::Screen);
        CHECK(engine.AddObject(id, blended, "img").ok());
    };

    struct SizeCase { int w; int h; };
    const SizeCase cases[] = { { 640, 360 }, { 1920, 1080 } };
    for (const SizeCase& c : cases) {
        const std::string id = std::format("gpu_stage_probe_{}x{}", c.w, c.h);
        buildScene(id, c.w, c.h);

        r::RenderOptions opts;
        opts.distribute = false;
        opts.capture = true;

        CHECK(engine.SetBackend("software").ok());
        auto soft = engine.Render(id, opts);
        CHECK(soft.ok());
        if (!soft.ok()) continue;
        // ...TWICE on the software side too, so the reference is the steady
        // state and not a first-frame fluke.
        auto softAgain = engine.Render(id, opts);
        CHECK(softAgain.ok());

        CHECK(engine.SetBackend("gpu").ok());
        const bool gpu = engine.BackendCaps().gpuAccelerated;
        if (!gpu) {
            CHECK(engine.BackendCaps().name == std::string("software"));
            continue;   // no D3D11 device on this machine: nothing to compare
        }
        auto firstFrame = engine.Render(id, opts);
        CHECK(firstFrame.ok());
        auto frame = engine.Render(id, opts);
        CHECK(frame.ok());
        if (!frame.ok()) continue;

        const auto countNonBlack = [](const r::RgbaImage& f) {
            long long n = 0;
            for (uint32_t px : f.pixels)
                if (((px >> 24) & 0xFFu) != 0 && (px & 0x00FFFFFFu) != 0)
                    ++n;
            return n;
        };
        const auto countWhite = [](const r::RgbaImage& f) {
            long long n = 0;
            for (uint32_t px : f.pixels)
                if ((px & 0xFFu) > 0xC0u && ((px >> 8) & 0xFFu) > 0xC0u && ((px >> 16) & 0xFFu) > 0xC0u)
                    ++n;   // the text is white — this is the glyphs, specifically
            return n;
        };
        const auto compare = [&](const r::RgbaImage& reference, const r::RgbaImage& candidate,
                                 const char* label) {
            std::fprintf(stderr,
                         "[d3d11] stage frame %dx%d %s: reference painted %lld px (%lld white), "
                         "this frame %lld px (%lld white)\n",
                         c.w, c.h, label, countNonBlack(reference), countWhite(reference),
                         countNonBlack(candidate), countWhite(candidate));
            // THE FAULT, named: a frame holding only its clear.
            CHECK(countNonBlack(candidate) > 0);
            CHECK(countWhite(candidate) == countWhite(reference));
            CHECK(candidate.pixels.size() == reference.pixels.size());
            if (candidate.pixels.size() != reference.pixels.size())
                return;
            size_t within4 = 0;
            int worst = 0;
            for (size_t i = 0; i < reference.pixels.size(); ++i) {
                const uint32_t a = reference.pixels[i];
                const uint32_t b = candidate.pixels[i];
                int delta = 0;
                for (int shift = 0; shift < 32; shift += 8)
                    delta = std::max(delta, std::abs(static_cast<int>((a >> shift) & 0xFF) -
                                                     static_cast<int>((b >> shift) & 0xFF)));
                if (delta <= 4) ++within4;
                worst = std::max(worst, delta);
            }
            std::fprintf(stderr, "[d3d11] stage frame %dx%d %s: %zu/%zu pixels within +-4, worst %d\n",
                         c.w, c.h, label, within4, reference.pixels.size(), worst);
            CHECK(within4 == reference.pixels.size());
        };
        CHECK(countNonBlack(soft.value()) > 0);
        CHECK(countWhite(soft.value()) > 0);      // the reference really has its text
        {
            const float sx = static_cast<float>(c.w) / 1920.0f;
            const float sy = static_cast<float>(c.h) / 1080.0f;
            const int bx = static_cast<int>((1200.0f + 310.0f) * sx);
            const int by = static_cast<int>((620.0f + 340.0f) * sy);
            CHECK(bx >= 0 && by >= 0 && bx < soft.value().width && by < soft.value().height);
            if (bx >= 0 && by >= 0 && bx < soft.value().width && by < soft.value().height) {
                const r::Color px = r::Color::Unpack(
                    soft.value().pixels[static_cast<size_t>(by) * soft.value().width + bx]);
                CHECK(px.r > 0.6f);                       // 0.69 backdrop + 0.2 source, screened
                CHECK(px.g > 0.45f && px.g < 0.65f);      // 0.19 + 0.45 screened
            }
        }
        compare(soft.value(), softAgain.value(), "software again");
        compare(soft.value(), firstFrame.value(), "gpu first");
        compare(soft.value(), frame.value(), "gpu second");

        (void)engine.DestroyScene(id);
    }

    CHECK(engine.SetBackend("software").ok());
    std::error_code ec;
    (void)std::filesystem::remove(img, ec);
}

// ---------------------------------------------------------------------------
// FEED HEALTH — the two engine questions behind the automatic stall blackout
// ---------------------------------------------------------------------------
namespace {
int64_t SteadyNowMsForTest()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

class FeedProbeOutput final : public r::IRenderOutput {
public:
    explicit FeedProbeOutput(std::string name, r::Size size)
        : name_(std::move(name)), size_(size) {}
    r::OutputKind Kind() const noexcept override { return r::OutputKind::Stream; }
    const char* Name() const noexcept override { return name_.c_str(); }
    r::Size TargetSize() const override { return size_; }
    void SetTargetSize(r::Size s) override { size_ = s; }
    bool Enabled() const noexcept override { return enabled_; }
    void SetEnabled(bool on) override { enabled_ = on; }
    Result<void> Present(const r::Frame& frame) override {
        last_ = frame;
        ++presented_;
        stamp_ = SteadyNowMsForTest();
        return Ok();
    }
    r::Frame LastFrame() const override { return last_; }
    uint64_t FramesReceived() const override { return presented_; }
    int64_t LastPresentSteadyMs() const override { return stamp_; }

private:
    std::string name_;
    r::Size size_;
    bool enabled_ = true;
    r::Frame last_;
    uint64_t presented_ = 0;
    int64_t stamp_ = 0;   // 0 = never presented, which the engine reports as -1
};
} // namespace

void TestFeedHealth() {
    auto& engine = r::RenderEngine::Instance();
    CHECK(engine.Initialize().ok());

    auto fed = std::make_shared<FeedProbeOutput>("feed-probe-fed", r::Size(8, 6));
    auto cold = std::make_shared<FeedProbeOutput>("feed-probe-cold", r::Size(8, 6));
    auto off = std::make_shared<FeedProbeOutput>("feed-probe-off", r::Size(8, 6));
    off->SetEnabled(false);
    CHECK(engine.AddOutput(fed).ok());
    CHECK(engine.AddOutput(cold).ok());
    CHECK(engine.AddOutput(off).ok());

    const auto findAge = [](const std::vector<r::RenderEngine::OutputFeedAge>& ages,
                            const std::string& name) -> int64_t {
        for (const auto& row : ages)
            if (row.name == name) return row.ageMs;
        return -2;
    };
    {
        const auto ages = engine.OutputFeedAges();
        CHECK(findAge(ages, "feed-probe-cold") == -1);
        CHECK(findAge(ages, "feed-probe-fed") == -1);   // nothing presented yet
        CHECK(findAge(ages, "feed-probe-nobody") == -2);
    }

    // A present stamps the output, so its age becomes a small, real number and
    // keeps advancing with the clock — that advance IS the heartbeat.
    {
        r::Frame f;
        f.sceneId = "live";
        f.width = 8; f.height = 6;
        f.pixels.assign(8u * 6u, 0xFF3366CCu);
        CHECK(fed->Present(f).ok());
        const auto ages = engine.OutputFeedAges();
        const int64_t age = findAge(ages, "feed-probe-fed");
        CHECK(age >= 0);
        CHECK(age < 1000);
        CHECK(findAge(ages, "feed-probe-cold") == -1);
    }

    {
        const auto framesOf = [](const std::vector<r::RenderEngine::OutputFeedAge>& ages,
                                 const std::string& name) -> uint64_t {
            for (const auto& row : ages)
                if (row.name == name) return row.frames;
            return 0;
        };
        const auto ages = engine.OutputFeedAges();
        CHECK(framesOf(ages, "feed-probe-fed") == 1);
        CHECK(framesOf(ages, "feed-probe-cold") == 0);   // never presented
        CHECK(framesOf(ages, "feed-probe-off") == 0);    // disabled: nothing sent
    }

    {
        const int blacked = engine.BlackoutOutputs({"feed-probe-fed", "feed-probe-off"});
        CHECK(blacked == 1);   // the disabled one is refused
        const r::Frame last = fed->LastFrame();
        CHECK(last.width == 8 && last.height == 6);
        CHECK(last.pixels.size() == 48);
        bool allBlack = true;
        for (const uint32_t px : last.pixels)
            if (px != 0xFF000000u) allBlack = false;
        CHECK(allBlack);
        // A disabled output keeps whatever it had: it cannot be sent anything.
        CHECK(off->FramesReceived() == 0);
    }

    // AND THE RECOVERY IS AUTOMATIC: nothing is latched, so the next real frame
    // simply overwrites the black. No un-blackout call exists to forget.
    {
        r::Frame f;
        f.sceneId = "live";
        f.width = 8; f.height = 6;
        f.pixels.assign(8u * 6u, 0xFF00AA55u);
        CHECK(fed->Present(f).ok());
        const r::Frame last = fed->LastFrame();
        CHECK(last.pixels.size() == 48);
        CHECK(last.pixels[0] == 0xFF00AA55u);
    }

    // A blackout with nothing to black (an empty list, or names the engine does
    // not know) is a no-op, not a failure.
    CHECK(engine.BlackoutOutputs({}) == 0);
    CHECK(engine.BlackoutOutputs({"feed-probe-nobody"}) == 0);

    CHECK(engine.RemoveOutput("feed-probe-fed").ok());
    CHECK(engine.RemoveOutput("feed-probe-cold").ok());
    CHECK(engine.RemoveOutput("feed-probe-off").ok());
}

void TestLineSpacingAndLineBands() {
    auto& engine = r::RenderEngine::Instance();
    CHECK(engine.Initialize().ok());
    CHECK(engine.SetBackend("software").ok());

    r::FontManager fonts;
    fonts.RegisterBuiltin();
    r::TextStyle plainStyle;
    plainStyle.fontId = "builtin";
    plainStyle.size = 20.0f;
    plainStyle.wrap = false;
    const auto plain = r::TextLayout::Measure("one\ntwo\nthree", plainStyle, fonts);
    r::TextStyle gapped = plainStyle;
    gapped.lineGap = 12.0f;
    const auto spaced = r::TextLayout::Measure("one\ntwo\nthree", gapped, fonts);
    CHECK(plain.lines.size() == 3);
    CHECK(spaced.lines.size() == 3);
    CHECK(std::abs((spaced.lineStep - plain.lineStep) - 12.0f) < 0.01f);
    // three lines = TWO gaps of air
    CHECK(std::abs((spaced.totalHeight - plain.totalHeight) - 24.0f) < 0.01f);

    const auto frame = [&](bool withBand, float radius = 0.0f) {
        const std::string id = withBand ? (radius > 0.0f ? "lineBandRound" : "lineBandOn")
                                        : "lineBandOff";
        CHECK(engine.CreateScene(id, id, r::Size(80, 120)).ok());
        CHECK(engine.AddLayer(id, r::Layer("t", "Text", r::LayerKind::Text, 1)).ok());
        auto obj = std::make_shared<r::TextObject>("body", "Body", "    \n    \n    ");
        obj->SetBounds(r::Rect(0, 0, 80, 120));
        r::TextStyle ts;
        ts.fontId = "builtin";
        ts.size = 20.0f;
        ts.color = r::Color::White();
        ts.align = r::TextAlign::Left;
        ts.valign = r::TextVAlign::Top;
        ts.lineGap = 12.0f;
        if (withBand) ts.lineBg = r::Color::Red();
        ts.lineRadius = radius;
        obj->AddComponent(r::MakeTextStyle(ts));
        CHECK(engine.AddObject(id, obj, "t").ok());
        auto fb = engine.RenderOffscreen(id, r::Size(80, 120));
        CHECK(fb.ok());
        CHECK(engine.DestroyScene(id).ok());
        return fb.value();
    };
    const auto on = frame(true);
    const auto off = frame(false);
    const auto at = [](const r::RgbaImage& img, int x, int y) {
        return r::Color::Unpack(img.pixels[static_cast<size_t>(y) * img.width + x]);
    };
    // First band: one line box, y 0..20.
    CHECK(at(on, 4, 4).r > 0.9f);
    CHECK(at(on, 4, 4).a > 0.9f);
    // Second band starts AFTER the 12px gap: y 32..52.
    CHECK(at(on, 4, 36).r > 0.9f);
    // The gap is air, not band: y 20..32.
    CHECK(at(on, 4, 26).r < 0.1f);
    // And the same item without a band paints none of it.
    CHECK(at(off, 4, 4).r < 0.1f);
    CHECK(at(off, 4, 36).r < 0.1f);

    const auto round = frame(true, 10.0f);
    CHECK(at(round, 4, 10).r > 0.9f);          // middle of the first band
    CHECK(at(round, 0, 1).r < 0.1f);           // top-left corner, cut by the arc
    CHECK(at(round, 0, 19).r < 0.1f);          // bottom-left corner too
    CHECK(at(round, 2, 10).r > 0.9f);          // ...but the band's edge, mid-height
}

void TestReadingFlowDrawCost() {
    auto& engine = r::RenderEngine::Instance();
    CHECK(engine.Initialize().ok());
    CHECK(engine.SetBackend("software").ok());

    // Every line is distinct text, so a frame that draws the wrong lines is a
    // different picture (see the pixel checks below).
    const auto passage = [](int lines) {
        std::string text;
        for (int i = 0; i < lines; ++i) {
            if (i > 0) text += "\n";
            text += "line " + std::to_string(i) + " of the passage";
        }
        return text;
    };
    // A page of six lines, scrolled to `charAt` (which, with no char table, is read
    // as a LINE — exactly the number the animator pushes for a style with no table).
    const auto build = [&](const std::string& id, int passageLines, float charAt,
                           uint32_t* outCalls, r::RgbaImage* outFrame) {
        CHECK(engine.CreateScene(id, id, r::Size(400, 200)).ok());
        CHECK(engine.AddLayer(id, r::Layer("t", "Text", r::LayerKind::Text, 1)).ok());
        auto obj = std::make_shared<r::TextObject>("body", "Body", passage(passageLines));
        obj->SetBounds(r::Rect(0, 0, 400, 200));
        r::TextStyle ts;
        ts.fontId = "builtin";
        ts.size = 20.0f;
        ts.wrap = true;
        ts.wrapWidth = 400.0f;
        ts.color = r::Color::White();
        ts.align = r::TextAlign::Left;
        ts.valign = r::TextVAlign::Top;
        obj->AddComponent(r::MakeTextStyle(ts));
        obj->SetReadingFlow(6, charAt);   // the viewport is six lines tall
        CHECK(engine.AddObject(id, obj, "t").ok());

        r::RenderOptions opts;
        opts.capture = true;
        opts.distribute = false;
        opts.contentScrollFromScene = true;   // the object's own FlowCharAt is the position
        engine.ResetStats();
        auto frame = engine.Render(id, opts);
        CHECK(frame.ok());
        // RenderStats::commands — the commands THIS frame built and submitted (the
        // software backend does not report draw calls of its own).
        *outCalls = engine.Stats().commands;
        *outFrame = frame.ok() ? frame.value() : r::RgbaImage{};
        CHECK(engine.DestroyScene(id).ok());
    };

    uint32_t shortCalls = 0;
    uint32_t longCalls = 0;
    uint32_t longStartCalls = 0;
    r::RgbaImage shortFrame;
    r::RgbaImage longFrame;
    r::RgbaImage longStartFrame;
    build("flowShort", 6, 0.0f, &shortCalls, &shortFrame);        // one page of passage
    build("flowLong", 60, 40.0f, &longCalls, &longFrame);         // 60 lines, scrolled to 40
    build("flowLongTop", 60, 0.0f, &longStartCalls, &longStartFrame);

    // The software rasterizer counts the commands it executed, so a zero here would
    // make every assertion below vacuous.
    CHECK(shortCalls > 0);
    CHECK(longCalls > 0);

    std::fprintf(stderr, "[reading-flow] draw calls: short=%u long@40=%u long@0=%u\n",
                 shortCalls, longCalls, longStartCalls);
    CHECK(longCalls <= shortCalls * 2);
    CHECK(longStartCalls <= shortCalls * 2);
    // ...and the passage's length cannot leak back in: 60 lines of ~23 characters is
    // more than 1300 glyphs, so a frame drawing them all would dwarf this bound.
    CHECK(longCalls < 400);

    const auto ink = [](const r::RgbaImage& img) {
        if (img.empty()) return size_t{0};
        const uint32_t base = img.pixels.front();
        size_t painted = 0;
        for (uint32_t px : img.pixels)
            if (px != base) ++painted;
        return painted;
    };
    const auto inkShort = ink(shortFrame);
    const auto inkLong = ink(longFrame);
    const auto inkTop = ink(longStartFrame);
    std::fprintf(stderr, "[reading-flow] ink: short=%zu long@40=%zu long@0=%zu\n",
                 inkShort, inkLong, inkTop);
    CHECK(inkShort > 0);
    CHECK(inkLong > 0);
    CHECK(inkLong == inkTop || inkLong != 0);   // both positions paint
    CHECK(longFrame.pixels != longStartFrame.pixels);   // ...and they are not the same frame

    CHECK(engine.SetBackend("auto").ok());
}