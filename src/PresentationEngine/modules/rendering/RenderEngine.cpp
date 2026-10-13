#include "modules/rendering/RenderEngine.hpp"

#include "core/config/ConfigurationManager.hpp"
#include "core/events/Events.hpp"
#include "core/logging/Logger.hpp"
#include "core/resources/ResourceManager.hpp"
#include "modules/adaptive/AdaptiveRuntime.hpp"
#include "modules/content/ContentManager.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <format>
#include <mutex>

namespace bps::rendering {

RenderEngine& RenderEngine::Instance() {
    static RenderEngine instance;
    return instance;
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

Result<void> RenderEngine::Initialize() {
    if (initialized_.load()) return Ok();

    fonts_.RegisterBuiltin();

    // Backends: software is the default; null for headless.
    softwareBackend_ = std::make_unique<SoftwareGraphicsBackend>();
    nullBackend_ = std::make_unique<NullGraphicsBackend>();

    auto& config = ConfigurationManager::Instance();
    const std::string backendName = config.GetString("render.backend", "software");
    if (backendName == "null")
        backend_ = nullBackend_.get();
    else
        backend_ = softwareBackend_.get();

    gpu_.BindBackend(backend_);
    uploader_.BindGpu(&gpu_);

    // Adaptive Runtime integration: texture budget + thread count.
    auto& adaptive = adaptive::AdaptiveRuntime::Instance();
    gpu_.SetTextureBudget(adaptive.GetTextureBudget());

    initialized_.store(true);
    return Ok();
}

Result<void> RenderEngine::Start() {
    if (running_.load()) return Ok();
    WireEvents();
    running_.store(true);
    lastFrameAt_ = std::chrono::steady_clock::now();
    return Ok();
}

Result<void> RenderEngine::Stop() {
    if (!running_.load()) return Ok();
    UnwireEvents();
    running_.store(false);
    return Ok();
}

Result<void> RenderEngine::Shutdown() {
    (void)Stop();
    gpu_.Clear();
    cache_.Clear();
    uploader_.Shutdown();
    outputs_.Clear();
    scenes_.Clear();
    if (backend_) (void)backend_->Shutdown();
    backend_ = nullptr;
    softwareBackend_.reset();
    nullBackend_.reset();
    initialized_.store(false);
    return Ok();
}

Result<void> RenderEngine::Reload() {
    auto& config = ConfigurationManager::Instance();
    const std::string backendName = config.GetString("render.backend", "software");
    if (backendName == "null" && backend_ != nullBackend_.get()) {
        backend_ = nullBackend_.get();
        gpu_.BindBackend(backend_);
        uploader_.BindGpu(&gpu_);
    } else if (backendName != "null" && backend_ != softwareBackend_.get()) {
        backend_ = softwareBackend_.get();
        gpu_.BindBackend(backend_);
        uploader_.BindGpu(&gpu_);
    }
    gpu_.SetTextureBudget(adaptive::AdaptiveRuntime::Instance().GetTextureBudget());
    return Ok();
}

Result<void> RenderEngine::Reset() {
    (void)Stop();
    gpu_.Clear();
    cache_.Clear();
    outputs_.Clear();
    scenes_.Clear();
    frameCounter_.store(0);
    frameDrops_.store(0);
    return Ok();
}

HealthReport RenderEngine::GetHealth() const {
    HealthReport h;
    h.state = (running_.load() && backend_) ? HealthState::Healthy : HealthState::Degraded;
    h.detail = std::format("backend={} frames={}",
                           backend_ ? backend_->Capabilities().name : "none",
                           frameCounter_.load());
    h.errorCount = errorCount_.load();
    return h;
}

Metrics RenderEngine::MetricsSnapshot() const {
    Metrics m;
    m.cpuPct = 0.0;
    m.ramBytes = backend_ ? backend_->TextureMemoryBytes() : 0;
    m.queueLength = uploader_.Pending();
    m.threadCount = 0;
    m.errorCount = errorCount_.load();
    m.health = running_.load() ? HealthState::Healthy : HealthState::Degraded;
    return m;
}

// ---------------------------------------------------------------------------
// Backend
// ---------------------------------------------------------------------------

Result<void> RenderEngine::SetBackend(std::string_view name) {
    if (!initialized_.load()) return Error::Make(Err::InvalidState, "Render", "not initialized");
    if (name == "null")
        backend_ = nullBackend_.get();
    else if (name == "software")
        backend_ = softwareBackend_.get();
    else
        return Error::Make(Err::Render_BackendUnavailable, "Render",
                           "backend '" + std::string(name) + "' unavailable");
    gpu_.BindBackend(backend_);
    uploader_.BindGpu(&gpu_);
    gpu_.SetTextureBudget(adaptive::AdaptiveRuntime::Instance().GetTextureBudget());
    return Ok();
}

const BackendCapabilities& RenderEngine::BackendCaps() const {
    return backend_ ? backend_->Capabilities()
                    : softwareBackend_ ? softwareBackend_->Capabilities()
                                       : nullBackend_->Capabilities();
}

// ---------------------------------------------------------------------------
// Scenes / objects / layers
// ---------------------------------------------------------------------------

Result<std::shared_ptr<Scene>> RenderEngine::CreateScene(std::string_view id,
                                                         std::string_view name, Size size) {
    return scenes_.CreateScene(id, name, size);
}

Result<std::shared_ptr<Scene>> RenderEngine::GetScene(std::string_view id) const {
    return scenes_.GetScene(id);
}

Result<void> RenderEngine::DestroyScene(std::string_view id) {
    return scenes_.DestroyScene(id);
}

Result<RenderObject*> RenderEngine::AddObject(std::string_view sceneId,
                                              std::shared_ptr<RenderObject> obj,
                                              std::string_view layerId) {
    auto scene = scenes_.GetScene(sceneId);
    if (!scene.ok()) return scene.error();
    if (!obj) return Error::Make(Err::InvalidArgument, "Render", "null object");
    obj->SetLayer(std::string(layerId));
    auto node = std::make_shared<SceneNode>(obj->Id(), obj->Name());
    scene.value()->Root()->AddChild(node);
    node->AddComponent(obj);
    return obj.get();
}

Result<void> RenderEngine::RemoveObject(std::string_view sceneId, std::string_view objId) {
    auto scene = scenes_.GetScene(sceneId);
    if (!scene.ok()) return scene.error();
    if (!scene.value()->Root()->RemoveChild(objId))
        return Error::Make(Err::Render_ObjectNotFound, "Render",
                           "object '" + std::string(objId) + "' not found");
    return Ok();
}

Result<RenderObject*> RenderEngine::GetObject(std::string_view sceneId,
                                              std::string_view objId) const {
    auto scene = scenes_.GetScene(sceneId);
    if (!scene.ok()) return scene.error();
    SceneNode* node = scene.value()->FindNode(objId);
    if (!node) return Error::Make(Err::Render_ObjectNotFound, "Render",
                                  "object '" + std::string(objId) + "' not found");
    for (const auto& c : node->Components())
        if (auto* obj = dynamic_cast<RenderObject*>(c.get())) return obj;
    return Error::Make(Err::Render_ObjectNotFound, "Render", "no render object on node");
}

std::vector<RenderObject*> RenderEngine::CollectObjects(std::string_view sceneId) const {
    std::vector<RenderObject*> out;
    auto scene = scenes_.GetScene(sceneId);
    if (!scene.ok()) return out;
    std::function<void(SceneNode*)> walk = [&](SceneNode* n) {
        for (const auto& c : n->Components())
            if (auto* obj = dynamic_cast<RenderObject*>(c.get())) out.push_back(obj);
        for (const auto& child : n->Children()) walk(child.get());
    };
    walk(scene.value()->Root());
    return out;
}

Result<void> RenderEngine::AddLayer(std::string_view sceneId, const Layer& layer) {
    auto scene = scenes_.GetScene(sceneId);
    if (!scene.ok()) return scene.error();
    return scene.value()->AddLayer(layer);
}

// ---------------------------------------------------------------------------
// Outputs
// ---------------------------------------------------------------------------

Result<void> RenderEngine::AddOutput(std::shared_ptr<IRenderOutput> output) {
    return outputs_.Add(std::move(output));
}

Result<std::shared_ptr<IRenderOutput>> RenderEngine::GetOutput(std::string_view name) const {
    return outputs_.Get(name);
}

Result<void> RenderEngine::RemoveOutput(std::string_view name) {
    return outputs_.Remove(name);
}

size_t RenderEngine::OutputCount() const { return outputs_.Count(); }

// ---------------------------------------------------------------------------
// Resources
// ---------------------------------------------------------------------------

Result<TextureId> RenderEngine::UploadTexture(std::string_view name,
                                              const RgbaImage& image) {
    auto id = gpu_.AcquireTexture(name, image);
    if (id.ok()) {
        (void)EventBus::Instance().Publish(events::RenderTextureLoaded{
            std::string(name), image.width, image.height});
    }
    return id;
}

Result<void> RenderEngine::ReleaseTexture(std::string_view name) {
    return gpu_.ReleaseTexture(name);
}

Result<TextureId> RenderEngine::GetTexture(std::string_view name) const {
    return gpu_.GetTexture(name);
}

void RenderEngine::SubmitAsyncUpload(std::string name, std::vector<uint8_t> bytes,
                                     ResourceUploader::Decoder decoder,
                                     ResourceUploader::DoneFn done) {
    uploader_.Submit(std::move(name), std::move(bytes), std::move(decoder),
                     std::move(done));
}

// ---------------------------------------------------------------------------
// Drawing (pipeline pass callback)
// ---------------------------------------------------------------------------

namespace {

// ONE-SHOT per object id: a draw that silently does nothing is how a fully
// composed scene reaches an output as black. Every failure below used to be a
// bare `break` — an image whose texture could not be uploaded (a texture budget
// already spent elsewhere in the app, say) left the frame showing only the
// background fill, which is exactly "the preview is right and NDI/HDMI is
// black". Logged once per id so a live feed cannot spam the log.
void LogDrawFailureOnce(const std::string& objId, const std::string& why) {
    static std::mutex m;
    static std::map<std::string, std::string> seen;
    bool first = false;
    {
        std::lock_guard<std::mutex> lock(m);
        auto [it, inserted] = seen.emplace(objId, why);
        if (inserted || it->second != why) {
            it->second = why;
            first = true;
        }
    }
    if (first)
        Logger::Instance().Warning(std::format("draw skip: object '{}' — {}", objId, why),
                                   "RenderEngine");
}

} // namespace

void RenderEngine::DrawObject(RenderObject* obj, std::vector<DrawCommand>& cmds) {
    if (!obj || !obj->Visible() || obj->EffectiveAlpha() <= 0.0f) return;

    DrawCommand base;
    base.blend = static_cast<int>(BlendMode::Alpha);
    base.color = obj->Tint();

    switch (obj->Kind()) {
        case ObjectKind::Background: {
            auto* b = static_cast<BackgroundObject*>(obj);
            DrawCommand c = base;
            c.type = DrawCommand::Type::FillRect;
            c.rect = obj->Bounds();
            c.color = b->BackgroundColor();
            c.color.a *= obj->Opacity();
            cmds.push_back(c);
            break;
        }
        case ObjectKind::Gradient: {
            // Approximate vertical gradient with two stacked rects (software).
            auto* g = static_cast<GradientObject*>(obj);
            auto* grad = g->Gradient();
            if (!grad) break;
            const Rect r = obj->Bounds();
            DrawCommand c = base;
            c.type = DrawCommand::Type::FillRect;
            c.rect = Rect(r.x, r.y, r.width, r.height * 0.5f);
            c.color = grad->top;
            c.color.a *= obj->Opacity();
            cmds.push_back(c);
            c.rect.y += r.height * 0.5f;
            c.color = grad->bottom;
            c.color.a *= obj->Opacity();
            cmds.push_back(c);
            break;
        }
        case ObjectKind::Image: {
            auto* img = static_cast<ImageObject*>(obj);
            const auto* src = img->Source();
            if (!src || !src->source.loaded || src->source.image.empty()) {
                LogDrawFailureOnce(img->Id(), "no image pixels loaded (and 0 visible in the frame)");
                break;
            }
            auto tex = gpu_.GetTexture(src->source.assetId.empty()
                                           ? img->Id()
                                           : src->source.assetId);
            TextureId tid = tex.ok() ? tex.value() : TextureId{};
            if (!tid.valid()) {
                // Upload lazily (synchronous on first draw).
                auto up = gpu_.AcquireTexture(src->source.assetId.empty()
                                                  ? img->Id()
                                                  : src->source.assetId,
                                              src->source.image);
                if (!up.ok()) {
                    LogDrawFailureOnce(img->Id(), std::format(
                        "texture upload failed: {} (image {}x{} {} bytes, budget {} bytes, {} uploaded)",
                        up.error().message, src->source.image.width, src->source.image.height,
                        src->source.image.ByteSize(), gpu_.TextureBudget(),
                        gpu_.TextureMemoryBytes()));
                    break;
                }
                tid = up.value();
            }
            DrawCommand c = base;
            c.type = DrawCommand::Type::BlitImage;
            c.rect = obj->Bounds();
            c.texture = tid;
            c.srcRect = Rect(0, 0, static_cast<float>(src->source.image.width),
                             static_cast<float>(src->source.image.height));
            c.color = Color(1, 1, 1, obj->Opacity());
            cmds.push_back(c);
            break;
        }
        case ObjectKind::Video: {
            auto* v = static_cast<VideoObject*>(obj);
            if (!v->HasFrame()) break;
            // VERSIONED upload, not AcquireTexture's create-once lookup: a live
            // frame source (media on air, a camera tap) pushes new pixels every
            // tick through this same object id, and texture names dedup — so a
            // plain GetTexture/AcquireTexture pair bound the FIRST frame forever
            // and the real output froze on it (typically the decoder's black
            // opening frame) while the QML preview, reading the live QImage
            // directly, kept moving. UpdateTexture re-uploads in place only when
            // FrameRevision moved.
            auto up = gpu_.UpdateTexture(v->Id(), v->Frame(), v->FrameRevision());
            if (!up.ok()) {
                LogDrawFailureOnce(v->Id(), std::format(
                    "video texture upload failed: {} (frame {}x{} {} bytes, budget {} bytes, {} uploaded)",
                    up.error().message, v->Frame().width, v->Frame().height,
                    v->Frame().ByteSize(), gpu_.TextureBudget(), gpu_.TextureMemoryBytes()));
                break;
            }
            const TextureId tid = up.value();
            DrawCommand c = base;
            c.type = DrawCommand::Type::BlitImage;
            c.rect = obj->Bounds();
            c.texture = tid;
            c.srcRect = Rect(0, 0, static_cast<float>(v->Frame().width),
                             static_cast<float>(v->Frame().height));
            c.color = Color(1, 1, 1, obj->Opacity());
            cmds.push_back(c);
            break;
        }
        case ObjectKind::Shape: {
            auto* s = static_cast<ShapeObject*>(obj);
            const auto* shp = s->Shape();
            if (!shp) break;
            DrawCommand c = base;
            c.type = DrawCommand::Type::DrawShape;
            c.rect = obj->Bounds();
            c.color = shp->fill;
            c.color.a *= obj->Opacity();
            // Rounded corners travel WITH the command (destination pixels,
            // clamped by the rasterizer) — without this a shape's radius never
            // left ShapeComponent, so a template's rounded bands/panels drew
            // square on every engine output while the canvas and previews
            // rounded them (Qt's Rectangle { radius }).
            c.cornerRadius = shp->radius;
            cmds.push_back(c);
            break;
        }
        case ObjectKind::Countdown: {
            auto* cd = static_cast<CountdownObject*>(obj);
            auto* style = cd->FindComponent<TextStyleComponent>();
            std::string text = cd->FormatRemaining(
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::system_clock::now().time_since_epoch())
                    .count());
            TextObject tmp("__cd", "countdown", text);
            if (style) tmp.AddComponent(std::make_shared<TextStyleComponent>(*style));
            tmp.SetBounds(obj->Bounds());
            tmp.SetTint(obj->Tint());
            tmp.SetOpacity(obj->Opacity());
            DrawTextObject(&tmp, cmds);
            break;
        }
        case ObjectKind::Clock: {
            auto* cl = static_cast<ClockObject*>(obj);
            auto* style = cl->FindComponent<TextStyleComponent>();
            auto now = std::chrono::system_clock::now();
            std::time_t t = std::chrono::system_clock::to_time_t(now);
            std::tm tm{};
#if defined(_WIN32)
            localtime_s(&tm, &t);
#else
            localtime_r(&t, &tm);
#endif
            TextObject tmp("__clock", "clock",
                           std::format("{:02}:{:02}:{:02}", tm.tm_hour, tm.tm_min,
                                       tm.tm_sec));
            if (style) tmp.AddComponent(std::make_shared<TextStyleComponent>(*style));
            tmp.SetBounds(obj->Bounds());
            tmp.SetTint(obj->Tint());
            tmp.SetOpacity(obj->Opacity());
            DrawTextObject(&tmp, cmds);
            break;
        }
        case ObjectKind::Text: {
            DrawTextObject(static_cast<TextObject*>(obj), cmds);
            break;
        }
        case ObjectKind::Overlay:
        case ObjectKind::Custom:
        default:
            break;
    }
}

void RenderEngine::DrawTextObject(TextObject* obj, std::vector<DrawCommand>& cmds) {
    if (!obj || obj->Text().empty() || obj->EffectiveAlpha() <= 0.0f) return;
    const auto* styleComp = obj->FindComponent<TextStyleComponent>();
    TextStyle style = styleComp ? styleComp->style : TextStyle{};
    style.color.a *= obj->Opacity();

    auto layoutRes = cache_.Layout(obj->Text(), style, fonts_);
    if (!layoutRes.ok() || layoutRes.value().lines.empty()) return;

    // Real font when style.fontId names one the system can resolve (GDI+,
    // Windows), else the builtin bitmap font — same "try real, degrade
    // gracefully" contract as the cache's own fallback inside this call.
    auto atlas = cache_.GlyphAtlas(style, fonts_);
    if (!atlas.ok()) return;
    const auto& entry = *atlas.value();

    // Ensure the atlas texture is uploaded once. Keyed by font identity too
    // (not just size) — otherwise switching fonts/weights at the same
    // pixel size would keep reading whichever atlas uploaded FIRST under
    // this name, showing the wrong glyphs for every style after the first.
    //
    // The name MUST be built from the SAME size rounding the cache keys on:
    // RenderCache::GlyphAtlas buckets by int(size + 0.5) (round), while this
    // name used to truncate int(size). Two auto-fitted sizes straddling a .5
    // boundary (e.g. 90.4 and 90.6) then landed in DIFFERENT cache entries —
    // each with its own cell geometry (cellPx/cellPy are integerised) — yet
    // produced the SAME texture name, so the second style sampled the first
    // style's atlas with ITS OWN per-glyph source rectangles. The mismatch
    // grows with the cell's column/row (source x = col*(cellPx+gap)), which is
    // exactly why right-hand-column glyphs — a round vowel like 'o' (cell 79,
    // column 15) first among them — came out narrowed/shifted with a foreign
    // tick beside them while early-column letters looked fine. The atlas
    // dimensions are folded in as well so a rebuilt atlas of different
    // geometry can never be read through a stale texture.
    const std::string atlasName = std::format("__font_atlas_{}_{}_{}_{}_{}x{}",
                                              style.fontId,
                                              static_cast<int>(style.size + 0.5f),
                                              style.bold, style.italic,
                                              entry.atlas.width, entry.atlas.height);
    auto tex = gpu_.GetTexture(atlasName);
    TextureId tid = tex.ok() ? tex.value() : TextureId{};
    if (!tid.valid()) {
        auto up = gpu_.AcquireTexture(atlasName, entry.atlas);
        if (!up.ok()) return;
        tid = up.value();
    }

    const auto& result = layoutRes.value();
    // Step lines by the MEASURED per-line advance, not a re-derived estimate:
    // for real fonts Measure used the font's actual baseline step (which
    // differs per font), and painted spacing that disagrees with measured
    // spacing re-spreads the block — un-doing the fit solve (fit says it
    // fits; the paint overflows the box and clips the tail). Falls back to
    // the legacy formula only for a zero-value result (never produced by
    // Measure today, but a safe default if a caller hand-builds one).
    const float lineHeight = result.lineStep > 0.0f ? result.lineStep
                                                    : style.size + style.lineSpacing;
    const Rect bounds = obj->Bounds();

    // Vertical alignment offset.
    float startY = bounds.y;
    if (style.valign == TextVAlign::Middle)
        startY = bounds.y + (bounds.height - result.totalHeight) / 2.0f;
    else if (style.valign == TextVAlign::Bottom)
        startY = bounds.y + (bounds.height - result.totalHeight);

    // Shadow pass (offset, dark).
    if (style.shadowEnabled) {
        DrawCommand c;
        c.type = DrawCommand::Type::FillRect;
        c.blend = static_cast<int>(BlendMode::Alpha);
        float sy = startY;
        for (const auto& line : result.lines) {
            float lx = bounds.x + style.shadowOffset.x;
            if (style.align == TextAlign::Center)
                lx = bounds.x + (bounds.width - line.width) / 2.0f + style.shadowOffset.x;
            else if (style.align == TextAlign::Right)
                lx = bounds.x + (bounds.width - line.width) + style.shadowOffset.x;
            c.rect = Rect(lx, sy + style.shadowOffset.y, line.width, style.size);
            c.color = style.shadowColor;
            c.color.a *= obj->Opacity();
            cmds.push_back(c);
            sy += lineHeight;
        }
    }

    // Glyph pass.
    float ly = startY;
    for (const auto& line : result.lines) {
        float lx = bounds.x;
        if (style.align == TextAlign::Center)
            lx = bounds.x + (bounds.width - line.width) / 2.0f;
        else if (style.align == TextAlign::Right)
            lx = bounds.x + (bounds.width - line.width);
        // RTL: reverse visual order within the line.
        std::string visual = style.rtl ? std::string(line.text.rbegin(), line.text.rend())
                                       : line.text;
        float cx = lx;
        for (char ch : visual) {
            const uint8_t cp = static_cast<uint8_t>(ch);
            const auto gIt = entry.glyphs.find(cp);
            if (gIt == entry.glyphs.end()) {
                // Line text is folded to atlas ASCII by TextLayout (see
                // FoldToAtlasAscii), so a miss here is a stray control byte,
                // not visible punctuation — never repeat the old behavior of
                // burning a 0.6×size blank gap per byte (the "that     s"
                // apostrophe holes). Carriage returns advance nothing; the
                // fallback advance is ~one builtin-font cell, not half a line.
                if (ch == '\r') continue;
                cx += (ch == '\t') ? style.size * 1.2f : style.size * 0.25f;
                continue;
            }
            const FontGlyph& g = gIt->second;
            DrawCommand c;
            c.type = DrawCommand::Type::DrawGlyph;
            c.texture = tid;
            // The glyph's cell is NOT anchored on the pen: a system-font cell
            // is the advance box plus the atlas's own ink pad (see
            // FontManager::BuildSystemAtlas), and its bearing says where the
            // cell's top-left sits relative to the advance origin. Adding it
            // puts the origin back on the pen, so the painted position is
            // unchanged and only the cell's extra margin (ink that overshoots
            // the advance) comes along. The builtin atlas carries bearing 0.
            c.rect = Rect(cx + static_cast<float>(g.bearingX),
                          ly + static_cast<float>(g.bearingY),
                          static_cast<float>(g.width), static_cast<float>(g.height));
            c.srcRect = Rect(static_cast<float>(g.x), static_cast<float>(g.y),
                             static_cast<float>(g.width), static_cast<float>(g.height));
            c.color = style.color;
            c.blend = static_cast<int>(BlendMode::Alpha);
            cmds.push_back(c);
            cx += static_cast<float>(g.advance) + style.letterSpacing;
        }
        ly += lineHeight;
    }
}

namespace {

// Alpha inspection (RenderEngine::SetAlphaInspect): source-over the frame onto
// a checkerboard so a keyed feed's transparent pixels are VISIBLE downstream —
// the real bound output window, the NDI feed and HDMI all read the pixels this
// produced. Fully opaque pixels are skipped, so an opaque scene is unchanged.
// Color::Pack is 0xAABBGGRR (R in the low byte).
void CompositeOverCheckerboard(RgbaImage& img) {
    constexpr int kTile = 16;
    constexpr uint32_t kLight = 0xFFCCCCCCu;
    constexpr uint32_t kDark = 0xFF808080u;
    for (int y = 0; y < img.height; ++y) {
        for (int x = 0; x < img.width; ++x) {
            uint32_t& px = img.pixels[static_cast<size_t>(y) * static_cast<size_t>(img.width) + static_cast<size_t>(x)];
            const uint32_t a = (px >> 24) & 0xFFu;
            if (a == 0xFFu)
                continue;   // opaque: nothing to prove
            const uint32_t bg = ((((x / kTile) + (y / kTile)) & 1) == 0) ? kLight : kDark;
            const uint32_t inv = 255u - a;
            const uint32_t r = ((px & 0xFFu) * a + (bg & 0xFFu) * inv + 127u) / 255u;
            const uint32_t g = (((px >> 8) & 0xFFu) * a + ((bg >> 8) & 0xFFu) * inv + 127u) / 255u;
            const uint32_t b = (((px >> 16) & 0xFFu) * a + ((bg >> 16) & 0xFFu) * inv + 127u) / 255u;
            px = 0xFF000000u | (b << 16) | (g << 8) | r;
        }
    }
}

} // namespace

// ---------------------------------------------------------------------------
// Render
// ---------------------------------------------------------------------------

Result<RgbaImage> RenderEngine::Render(std::string_view sceneId, const RenderOptions& opts) {
    if (!backend_) return Error::Make(Err::Render_BackendUnavailable, "Render", "no backend");
    auto scene = scenes_.GetScene(sceneId);
    if (!scene.ok()) return scene.error();

    // One frame at a time: the backend framebuffer and the lazy texture uploads
    // (DrawObject → gpu_.AcquireTexture) are not safe under concurrent renders.
    std::lock_guard<std::mutex> frameLock(renderMutex_);

    const auto frameStart = std::chrono::steady_clock::now();
    const uint64_t frameNum = frameCounter_.fetch_add(1) + 1;
    scene.value()->BumpFrame();

    // Layout: size the backend to the scene.
    const int w = static_cast<int>(scene.value()->SceneSize().width);
    const int h = static_cast<int>(scene.value()->SceneSize().height);
    if (auto r = backend_->BeginFrame(w, h); !r.ok()) return r.error();

    std::vector<DrawCommand> cmds;
    cmds.reserve(64);
    if (opts.clearBefore) {
        DrawCommand clear;
        clear.type = DrawCommand::Type::Clear;
        clear.color = opts.clearColor;
        // A scene that declares NO opaque background of its own (a style that
        // paints nothing — an explicitly "transparent" template, or one that
        // names no colour at all) clears to alpha 0: the clear is the pixels the
        // scene never paints, and for a keyable overlay those must stay
        // transparent — the monitor tile shows it as transparent and the output
        // window / NDI feed carry the alpha. Scenes that DO paint a full-frame
        // background are untouched: the clear sits under them, so its alpha only
        // ever decides the areas nothing was drawn in.
        if (scene.value()->TransparentBackground())
            clear.color.a = 0.0f;
        cmds.push_back(clear);
    }

    // Collect objects and distribute into pipeline passes. Animations are
    // driven externally (modules call Anim().Update per frame) — the engine
    // owns the animator, modules own the schedule.
    auto objects = CollectObjects(sceneId);
    const auto layers = scene.value()->LayersSorted();
    pipeline_.Distribute(objects, layers);
    pipeline_.Execute([&](PassId pass, const std::vector<RenderObject*>& objs) {
        (void)pass;
        for (auto* obj : objs) DrawObject(obj, cmds);
    });

    // ONE-SHOT PER SCENE PER STATE: what this render actually had to draw. A
    // frame that comes out black is either "the pipeline drew nothing" (no
    // objects collected, or every object dropped for a missing layer — see
    // RenderPipeline::Distribute's layerOk gate) or "objects drew but landed
    // under/over something"; this line separates those two without a debugger,
    // and cmdCount==1 means only the clear command was submitted.
    {
        static std::mutex logMutex;
        static std::map<std::string, std::string> lastState;
        // Command breakdown, and the first few commands' geometry: what the
        // rasterizer was actually asked to draw. "BlitImage count 0" means the
        // draw never got as far as a picture; "BlitImage 1 with a rect that is
        // off the canvas" is a different bug from "the texture was black".
        int nClear = 0, nFill = 0, nBlit = 0, nGlyph = 0, nShape = 0;
        std::string firstCmds;
        for (const DrawCommand& c : cmds) {
            switch (c.type) {
                case DrawCommand::Type::Clear: ++nClear; break;
                case DrawCommand::Type::FillRect: ++nFill; break;
                case DrawCommand::Type::BlitImage: ++nBlit; break;
                case DrawCommand::Type::DrawGlyph: ++nGlyph; break;
                case DrawCommand::Type::DrawShape: ++nShape; break;
            }
            if (firstCmds.size() < 220)
                firstCmds += std::format(" [{}({:.0f},{:.0f},{:.0f}x{:.0f})]",
                                         static_cast<int>(c.type), c.rect.x, c.rect.y,
                                         c.rect.width, c.rect.height);
        }
        const std::string state = std::format(
            "objects={} layers={} cmds={} size={}x{} backend={} clear={} fill={} blit={} glyph={} shape={} |{}",
            objects.size(), layers.size(), cmds.size(), w, h,
            backend_ == nullBackend_.get() ? "null" : "software",
            nClear, nFill, nBlit, nGlyph, nShape, firstCmds);
        bool changed = false;
        {
            std::lock_guard<std::mutex> lock(logMutex);
            std::string& last = lastState[std::string(sceneId)];
            if (last != state) {
                last = state;
                changed = true;
            }
        }
        if (changed)
            Logger::Instance().Info(
                std::format("render dispatch: [{}] {}", sceneId, state), "RenderEngine");
    }

    // Custom graph passes run after the fixed pipeline (extensible effects).
    graph_.ExecutePasses();

    if (auto r = backend_->Submit(cmds); !r.ok()) return r.error();
    if (auto r = backend_->EndFrame(); !r.ok()) return r.error();

    // Readback + distribute to outputs.
    RgbaImage frame;
    if (opts.capture || opts.distribute) {
        auto rb = backend_->Readback();
        if (!rb.ok()) return rb.error();
        frame = rb.value();
        // Transparency inspection (opt-in): make the alpha VISIBLE downstream.
        if (alphaInspect_.load(std::memory_order_relaxed) && !frame.empty())
            CompositeOverCheckerboard(frame);
    }
    if (opts.distribute) {
        Frame out;
        out.sceneId = std::string(sceneId);
        out.frame = frameNum;
        out.width = frame.width;
        out.height = frame.height;
        out.pixels = frame.pixels;
        out.timestampMs = std::chrono::duration<double, std::milli>(
                              std::chrono::system_clock::now().time_since_epoch())
                              .count();
        outputs_.Distribute(out);
    }

    // Diagnostics + frame budget (60 FPS target ⇒ 16.67 ms; drops logged).
    const auto frameEnd = std::chrono::steady_clock::now();
    lastFrameMs_ = std::chrono::duration<double, std::milli>(frameEnd - frameStart).count();
    if (lastFrameMs_ > 16.67) {
        ++frameDrops_;
        (void)EventBus::Instance().Publish(events::RenderFrameDropped{
            std::string(sceneId), lastFrameMs_, 16.67});
    }
    uploader_.Pump();
    frameGraph_.BeginFrame();
    frameGraph_.EndFrame();

    (void)EventBus::Instance().Publish(events::RenderFrameRendered{
        std::string(sceneId), frameNum, lastFrameMs_,
        static_cast<uint32_t>(cmds.size())});
    return frame;
}

Result<RgbaImage> RenderEngine::RenderOffscreen(std::string_view sceneId, Size size,
                                                const RenderOptions& opts) {
    auto scene = scenes_.GetScene(sceneId);
    if (!scene.ok()) return scene.error();
    const Size saved = scene.value()->SceneSize();
    RenderOptions o = opts;
    o.capture = true;
    // Temporarily set the scene size for offscreen rendering.
    scene.value()->SetSize(size);
    auto r = Render(sceneId, o);
    scene.value()->SetSize(saved);
    return r;
}

Result<RgbaImage> RenderEngine::RenderThumbnail(std::string_view sceneId, Size size) {
    return RenderOffscreen(sceneId, size, RenderOptions{});
}

// ---------------------------------------------------------------------------
// Diagnostics
// ---------------------------------------------------------------------------

RenderStats RenderEngine::Stats() const {
    std::lock_guard<std::mutex> lock(renderMutex_);
    RenderStats s;
    s.frames = frameCounter_.load();
    s.frameMs = lastFrameMs_;
    s.frameDrops = frameDrops_.load();
    s.gpuMemoryBytes = backend_ ? backend_->TextureMemoryBytes() : 0;
    s.textureCount = static_cast<uint32_t>(gpu_.TextureCount());
    s.shaderCount = static_cast<uint32_t>(gpu_.ShaderCount());
    s.renderQueueSize = static_cast<uint32_t>(uploader_.Pending());
    const auto now = std::chrono::steady_clock::now();
    const double elapsed = std::chrono::duration<double>(now - lastFrameAt_).count();
    if (elapsed > 0 && s.frames > 0) s.fps = 1.0 / std::max(elapsed, 1e-9);
    lastFrameAt_ = now;
    return s;
}

void RenderEngine::ResetStats() {
    frameCounter_.store(0);
    frameDrops_.store(0);
}

// ---------------------------------------------------------------------------
// Memory pressure + events
// ---------------------------------------------------------------------------

void RenderEngine::OnMemoryPressure(PressureLevel level) {
    pressure_.store(level);
    std::lock_guard<std::mutex> lock(renderMutex_);
    // Render-managed textures (font atlases, lazily uploaded images) keep refs
    // ≥ 1 while in use, so EvictIdle (refs==0 only) cannot reclaim them. Under
    // high pressure we drop the whole GPU cache — the lazy-upload path in
    // DrawObject/DrawTextObject re-uploads what the next frame actually needs.
    cache_.InvalidateGlyphAtlas();
    cache_.InvalidateLayouts();
    if (level >= PressureLevel::High) {
        const size_t evicted = gpu_.TextureCount();
        gpu_.Clear();
        if (evicted > 0)
            Logger::Instance().Info("RenderEngine: cleared " + std::to_string(evicted) +
                                        " GPU textures (" + std::string(ToString(level)) + ")",
                                    "Render");
    } else if (level == PressureLevel::Medium) {
        size_t evicted = gpu_.EvictIdle(gpu_.TextureMemoryBytes() * 3 / 4);
        if (evicted > 0)
            Logger::Instance().Info("RenderEngine: evicted " + std::to_string(evicted) +
                                        " idle textures (" + std::string(ToString(level)) + ")",
                                    "Render");
    }
}

void RenderEngine::WireEvents() {
    auto& bus = EventBus::Instance();
    subscriptions_.push_back(bus.Subscribe<events::ContentAssetLoaded>(
        [this](const events::ContentAssetLoaded&) {}));
    subscriptions_.push_back(bus.Subscribe<events::ContentAssetDeleted>(
        [this](const events::ContentAssetDeleted& e) { OnAssetDeleted(e); }));
    subscriptions_.push_back(bus.Subscribe<events::DisplayChanged>(
        [this](const events::DisplayChanged& e) { OnDisplayChanged(e); }));
    subscriptions_.push_back(bus.Subscribe<events::ConfigHotReload>(
        [this](const events::ConfigHotReload& e) { OnConfigReload(e); }));
    subscriptions_.push_back(bus.Subscribe<events::ResourcePressureChanged>(
        [this](const events::ResourcePressureChanged& e) { OnPressure(e); }));
}

void RenderEngine::UnwireEvents() {
    auto& bus = EventBus::Instance();
    for (auto& s : subscriptions_) (void)bus.Unsubscribe(s);
    subscriptions_.clear();
}

void RenderEngine::OnAssetDeleted(const events::ContentAssetDeleted& e) {
    // Evict textures that referenced the deleted asset.
    (void)gpu_.ReleaseTexture(e.uuid);
    (void)EventBus::Instance().Publish(events::RenderError{
        "asset deleted: texture evicted (" + e.uuid + ")"});
}

void RenderEngine::OnDisplayChanged(const events::DisplayChanged&) {
    // Output routing is owned by Phase 6; refresh display-driven sizes here.
}

void RenderEngine::OnConfigReload(const events::ConfigHotReload&) {
    (void)Reload();
}

void RenderEngine::OnPressure(const events::ResourcePressureChanged& e) {
    if (e.resource == "memory") OnMemoryPressure(e.to);
}

} // namespace bps::rendering
