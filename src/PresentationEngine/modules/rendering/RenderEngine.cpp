#include "modules/rendering/RenderEngine.hpp"

#include "core/config/ConfigurationManager.hpp"
#include "core/events/Events.hpp"
#include "core/logging/Logger.hpp"
#include "core/resources/ResourceManager.hpp"
#include "modules/adaptive/AdaptiveRuntime.hpp"
#include "modules/content/ContentManager.hpp"
#include "modules/rendering/D3D11Backend.hpp"   // the GPU backend's factory (no D3D11 type in it)

#include <algorithm>
#include <chrono>
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

namespace {
constexpr const char *kDefaultRenderBackend = "software";

// The GPU path, named the way the setting, the log and the docs all name it.
constexpr const char *kHardwareRenderBackend = "directx11";

constexpr const char *kHardwarePreferenceKey = "render.hardwareAcceleration";

// The explicit choice, as the configuration spells it.
constexpr const char *kBackendKey = "render.backend";
} // namespace

Result<void> RenderEngine::Initialize() {
    if (initialized_.load()) return Ok();

    fonts_.RegisterBuiltin();

    // Backends: the GPU when there is one, software otherwise, null for headless
    // (see SelectBackend for the contract and the fallback order).
    softwareBackend_ = std::make_unique<SoftwareGraphicsBackend>();
    nullBackend_ = std::make_unique<NullGraphicsBackend>();

    auto selected = SelectBackend(ConfiguredBackendName());
    if (!selected.ok())
        return selected.error();

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
    // The D3D11 device goes with it: a shutdown that left the device alive would
    // hand the next Initialize a stale one (and keep the GPU's memory booked).
    d3d11Backend_.reset();
    softwareBackend_.reset();
    nullBackend_.reset();
    loggedBackendOnce_ = false;
    loggedBackend_.clear();
    {
        std::lock_guard<std::mutex> lock(backendNameMutex_);
        requestedBackendName_.clear();
        activeBackendName_.clear();
    }
    initialized_.store(false);
    return Ok();
}

Result<void> RenderEngine::Reload() {
    (void)SelectBackend(ConfiguredBackendName());
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

//   1. `render.backend`              an EXPLICIT choice — a config file, a
//   2. else `render.hardwareAcceleration`
//   3. else                         the software rasterizer.
std::string RenderEngine::ResolveBackendName(std::string_view explicitBackend, bool hardwareAcceleration) {
    if (!explicitBackend.empty())
        return std::string(explicitBackend);
    return hardwareAcceleration ? kHardwareRenderBackend : kDefaultRenderBackend;
}

std::string RenderEngine::ConfiguredBackendName() {
    auto& config = ConfigurationManager::Instance();
    return ResolveBackendName(config.GetString(kBackendKey, ""),
                              config.GetBool(kHardwarePreferenceKey, false));
}

Result<void> RenderEngine::SelectBackend(std::string_view requested) {
    const std::string name(requested.empty() ? "auto" : requested);
    const bool gpuWanted = (name == kD3D11BackendName || name == "d3d11" || name == "auto");
    IGraphicsBackend* const before = backend_;

    if (gpuWanted && !d3d11Backend_) {
        std::string why;
        d3d11Backend_ = CreateD3D11Backend(&why);
        if (!d3d11Backend_)
            Logger::Instance().Info(
                std::format("no D3D11 device ({}): frames are drawn by the software rasterizer",
                            why.empty() ? "unavailable" : why),
                "RenderEngine");
    }

    if (name == "null") {
        backend_ = nullBackend_.get();
    } else if (name == "software") {
        backend_ = softwareBackend_.get();
    } else if (gpuWanted && d3d11Backend_) {
        backend_ = d3d11Backend_.get();
    } else if (gpuWanted) {
        // The GPU was asked for (explicitly or by "auto") and this machine has
        // none: the software rasterizer is the fallback, never a failed boot.
        backend_ = softwareBackend_.get();
    } else {
        return Error::Make(Err::Render_BackendUnavailable, "Render",
                           "backend '" + name + "' unavailable");
    }

    gpu_.BindBackend(backend_);
    uploader_.BindGpu(&gpu_);
    gpu_.SetTextureBudget(adaptive::AdaptiveRuntime::Instance().GetTextureBudget());
    if (before != backend_) {
        cache_.Clear();
    }
    {
        std::lock_guard<std::mutex> lock(backendNameMutex_);
        requestedBackendName_ = name;
        activeBackendName_ = backend_->Capabilities().name;
    }
    if (loggedBackend_ != name || !loggedBackendOnce_) {
        loggedBackend_ = name;
        loggedBackendOnce_ = true;
        Logger::Instance().Info(
            std::format("render backend: requested '{}' -> active '{}'", name,
                        backend_->Capabilities().name),
            "RenderEngine");
    }
    return Ok();
}

std::string RenderEngine::ActiveBackendName() const {
    std::lock_guard<std::mutex> lock(backendNameMutex_);
    return activeBackendName_;
}

std::string RenderEngine::RequestedBackendName() const {
    std::lock_guard<std::mutex> lock(backendNameMutex_);
    return requestedBackendName_;
}

Result<void> RenderEngine::SetBackend(std::string_view name) {
    if (!initialized_.load()) return Error::Make(Err::InvalidState, "Render", "not initialized");
    return SelectBackend(name);
}

FrameCostProfile RenderEngine::CostProfileFor(std::string_view sceneId) const {
    std::lock_guard<std::mutex> lock(renderMutex_);
    const auto it = costProfiles_.find(std::string(sceneId));
    if (it != costProfiles_.end())
        return it->second;
    return FrameCostProfile{};
}

const ElementCost* FrameCostProfile::HeaviestContent() const {
    // items are sorted heaviest-first, so the first non-background entry is the
    // heaviest element the scene itself drew.
    for (const ElementCost& c : items)
        if (!c.isBackground)
            return &c;
    return nullptr;
}

double FrameCostProfile::BackgroundSharePct() const {
    for (const ElementCost& c : items)
        if (c.isBackground)
            return c.sharePct;
    return 0.0;
}

FrameCostProfile RenderEngine::LastCostProfile() const {
    std::lock_guard<std::mutex> lock(renderMutex_);
    return costLast_;
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
// Feed health — the automatic blackout's two questions
// ---------------------------------------------------------------------------
std::vector<RenderEngine::OutputFeedAge> RenderEngine::OutputFeedAges() const {
    std::vector<OutputFeedAge> out;
    const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
                         std::chrono::steady_clock::now().time_since_epoch())
                         .count();
    for (const auto& output : outputs_.All()) {
        if (!output) continue;
        OutputFeedAge row;
        row.name = output->Name();
        row.enabled = output->Enabled();
        // The output's own delivered-frame count (atomic, no device, no frame
        // copy) — the same fact the NDI pill shows, available for every kind.
        row.frames = output->FramesReceived();
        const int64_t stamp = output->LastPresentSteadyMs();
        row.ageMs = stamp > 0 ? now - stamp : -1;
        out.push_back(std::move(row));
    }
    return out;
}

int RenderEngine::BlackoutOutputs(const std::vector<std::string>& names) {
    if (names.empty()) return 0;
    int blacked = 0;
    for (const auto& name : names) {
        auto found = outputs_.Get(name);
        if (!found.ok() || !found.value()) continue;
        const auto& output = found.value();
        if (!output->Enabled()) continue;
        const Size target = output->TargetSize();
        if (target.width <= 0 || target.height <= 0) continue;
        Frame black;
        black.sceneId = "feed-stalled";
        black.width = static_cast<int>(target.width);
        black.height = static_cast<int>(target.height);
        black.pixels.assign(static_cast<size_t>(black.width) * black.height, 0xFF000000u);
        black.timestampMs = static_cast<double>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                                    std::chrono::steady_clock::now().time_since_epoch())
                                                    .count());
        if (output->Present(black).ok()) ++blacked;
    }
    return blacked;
}

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
    base.blend = static_cast<int>(obj->Blend());
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

namespace {
bool ClipQuad(const Rect& clip, Rect& dest, Rect& src) {
    const float x0 = std::max(dest.x, clip.x);
    const float y0 = std::max(dest.y, clip.y);
    const float x1 = std::min(dest.Right(), clip.Right());
    const float y1 = std::min(dest.Bottom(), clip.Bottom());
    if (x1 <= x0 || y1 <= y0 || dest.width <= 0.0f || dest.height <= 0.0f)
        return false;
    if (src.width > 0.0f && src.height > 0.0f) {
        const float sx = src.width / dest.width;
        const float sy = src.height / dest.height;
        src = Rect(src.x + (x0 - dest.x) * sx, src.y + (y0 - dest.y) * sy,
                   (x1 - x0) * sx, (y1 - y0) * sy);
    }
    dest = Rect(x0, y0, x1 - x0, y1 - y0);
    return true;
}
} // namespace

void RenderEngine::DrawTextObject(TextObject* obj, std::vector<DrawCommand>& cmds) {
    if (!obj || obj->Text().empty() || obj->EffectiveAlpha() <= 0.0f) return;
    const auto* styleComp = obj->FindComponent<TextStyleComponent>();
    TextStyle style = styleComp ? styleComp->style : TextStyle{};
    style.color.a *= obj->Opacity();

    auto layoutRes = cache_.Layout(obj->Text(), style, fonts_);
    if (!layoutRes.ok() || layoutRes.value().lines.empty()) return;

    auto atlas = cache_.GlyphAtlas(style, fonts_);
    if (!atlas.ok()) return;
    const auto& entry = *atlas.value();

    const std::string substitutedFamily = fonts_.SubstituteFamily(style.fontId);
    const bool useSystemFont = !substitutedFamily.empty()
                               && fonts_.HasSystemFont(substitutedFamily);
    const int atlasSizeKey = useSystemFont ? FontManager::QtPixelSize(style.size)
                                           : static_cast<int>(style.size + 0.5f);
    const QFont::Weight atlasFontWeight = style.bold ? QFont::Bold : style.fontWeight;
    const std::string atlasName = std::format("__font_atlas_{}_{}_{}_{}_{}_{}_{}x{}",
                                              substitutedFamily.empty() ? style.fontId
                                                                        : substitutedFamily,
                                              atlasSizeKey,
                                              static_cast<int>(atlasFontWeight), style.italic,
                                              style.underline, style.strikeout,
                                              entry.atlas.width, entry.atlas.height);
    auto tex = gpu_.GetTexture(atlasName);
    TextureId tid = tex.ok() ? tex.value() : TextureId{};
    if (!tid.valid()) {
        auto up = gpu_.AcquireTexture(atlasName, entry.atlas);
        if (!up.ok()) {
            LogDrawFailureOnce(atlasName, std::format(
                "glyph atlas upload failed: {} (atlas {}x{} {} bytes, budget {} bytes, {} uploaded) "
                "- every glyph of this style draws nothing, so the frame carries only its background",
                up.error().message, entry.atlas.width, entry.atlas.height,
                entry.atlas.ByteSize(), gpu_.TextureBudget(), gpu_.TextureMemoryBytes()));
            return;
        }
        tid = up.value();
    }

    const auto& result = layoutRes.value();
    const float lineHeight = result.lineStep > 0.0f ? result.lineStep
                                                    : style.size + style.lineSpacing + style.lineGap;
    const Rect bounds = obj->Bounds();

    const bool flowing = obj->HasReadingFlow();
    // A BOOK PAGE SHOWS WHOLE LINES. Scroll's column fills its text area, but only down to the last line
    // that fits entirely: the next line peeking in half-cut at the bottom made a tall box (Main Output)
    // show the next paragraph's first words while a shorter one (the Lower Third) did not.
    const float wholeLinesHeight = lineHeight > 0.0f
        ? std::max(lineHeight, std::floor(bounds.height / lineHeight + 0.02f) * lineHeight)
        : bounds.height;
    const float alignHeight = flowing
        ? (obj->FlowFillsBox() ? wholeLinesHeight
                               : static_cast<float>(obj->FlowPageLines()) * lineHeight)
        : result.totalHeight;
    // The scroll itself, in the text's OWN line heights — the same advance the
    // lines below step by, so one line of the number is one line of the picture.
    const float scrollChars = !flowing ? 0.0f
                             : contentScrollFromScene_ ? obj->FlowCharAt()
                                                       : contentScrollChars_;
    const float scrollLines = flowing ? obj->FlowLineForChar(scrollChars) : 0.0f;
    const float scrollPx = scrollLines * lineHeight;

    // WHERE THE PAGE SITS — the top edge of the reading window.
    float pageTop = bounds.y;
    if (style.valign == TextVAlign::Middle)
        pageTop = bounds.y + (bounds.height - alignHeight) / 2.0f;
    else if (style.valign == TextVAlign::Bottom)
        pageTop = bounds.y + (bounds.height - alignHeight);

    const Rect clipRect = flowing
        ? Rect(bounds.x, pageTop, bounds.width, alignHeight)
        : bounds;

    const auto justifiedExtraPerSpace = [&style](const LineLayout& line) {
        if (style.align != TextAlign::Justify || line.paragraphEnd
            || style.wrapWidth <= line.width)
            return 0.0f;
        const auto spaces = std::count(line.text.begin(), line.text.end(), ' ');
        return spaces > 0
            ? (style.wrapWidth - line.width) / static_cast<float>(spaces)
            : 0.0f;
    };
    const auto paintedLineWidth = [&justifiedExtraPerSpace](const LineLayout& line) {
        const auto spaces = std::count(line.text.begin(), line.text.end(), ' ');
        return line.width + justifiedExtraPerSpace(line) * static_cast<float>(spaces);
    };

    // Vertical alignment offset.
    const float startY = pageTop - scrollPx;

    int firstLine = 0;
    int lastLine = static_cast<int>(result.lines.size());
    if (flowing && lineHeight > 0.0f) {
        const float top = clipRect.y;
        const float bottom = clipRect.Bottom();
        firstLine = std::max(0, static_cast<int>(std::floor((top - startY) / lineHeight)) - 1);
        lastLine = std::min(lastLine,
                            static_cast<int>(std::ceil((bottom - startY) / lineHeight)) + 1);
        if (lastLine <= firstLine)
            return;   // the viewport holds none of the passage (an offset past its end)
    }

    if (style.lineBg.a > 0.0f) {
        const float bandHeight = result.lineBox > 0.0f ? result.lineBox
                                                       : style.size + style.lineSpacing;
        for (int lineIndex = firstLine; lineIndex < lastLine; ++lineIndex) {
            const auto& line = result.lines[static_cast<size_t>(lineIndex)];
            const float by = startY + static_cast<float>(lineIndex) * lineHeight;
            float lx = bounds.x;
            const float lineWidth = paintedLineWidth(line);
            if (style.align == TextAlign::Center)
                lx = bounds.x + (bounds.width - lineWidth) / 2.0f;
            else if (style.align == TextAlign::Right)
                lx = bounds.x + (bounds.width - lineWidth);
            DrawCommand c;
            c.type = style.lineRadius > 0.0f ? DrawCommand::Type::DrawShape
                                            : DrawCommand::Type::FillRect;
            c.blend = static_cast<int>(BlendMode::Alpha);
            c.rect = Rect(lx, by, lineWidth, bandHeight);
            c.cornerRadius = style.lineRadius > 0.0f
                                 ? std::min(style.lineRadius, bandHeight * 0.5f)
                                 : 0.0f;
            c.color = style.lineBg;
            c.color.a *= obj->Opacity();
            // A band rides the same clip as its words: a scrolled band that
            // stayed whole would band air below the box.
            Rect src;
            if (!flowing || ClipQuad(clipRect, c.rect, src))
                cmds.push_back(c);
        }
    }

    // Shadow pass (offset, dark).
    if (style.shadowEnabled) {
        DrawCommand c;
        c.type = DrawCommand::Type::FillRect;
        c.blend = static_cast<int>(BlendMode::Alpha);
        for (int lineIndex = firstLine; lineIndex < lastLine; ++lineIndex) {
            const auto& line = result.lines[static_cast<size_t>(lineIndex)];
            const float sy = startY + static_cast<float>(lineIndex) * lineHeight;
            float lx = bounds.x + style.shadowOffset.x;
            const float lineWidth = paintedLineWidth(line);
            if (style.align == TextAlign::Center)
                lx = bounds.x + (bounds.width - lineWidth) / 2.0f + style.shadowOffset.x;
            else if (style.align == TextAlign::Right)
                lx = bounds.x + (bounds.width - lineWidth) + style.shadowOffset.x;
            c.rect = Rect(lx, sy + style.shadowOffset.y, lineWidth, style.size);
            c.color = style.shadowColor;
            c.color.a *= obj->Opacity();
            Rect src;
            if (!flowing || ClipQuad(clipRect, c.rect, src))
                cmds.push_back(c);
        }
    }

    // Glyph pass.
    for (int lineIndex = firstLine; lineIndex < lastLine; ++lineIndex) {
        const auto& line = result.lines[static_cast<size_t>(lineIndex)];
        const float ly = startY + static_cast<float>(lineIndex) * lineHeight;
        float lx = bounds.x;
        const float lineWidth = paintedLineWidth(line);
        if (style.align == TextAlign::Center)
            lx = bounds.x + (bounds.width - lineWidth) / 2.0f;
        else if (style.align == TextAlign::Right)
            lx = bounds.x + (bounds.width - lineWidth);
        std::string reversed;
        const std::string& visual = style.rtl
            ? (reversed.assign(line.text.rbegin(), line.text.rend()), reversed)
            : line.text;
        float cx = lx;
        for (char ch : visual) {
            const uint8_t cp = static_cast<uint8_t>(ch);
            const auto gIt = entry.glyphs.find(cp);
            if (gIt == entry.glyphs.end()) {
                if (ch == '\r') continue;
                cx += (ch == '\t') ? style.size * 1.2f : style.size * 0.25f;
                continue;
            }
            const FontGlyph& g = gIt->second;
            DrawCommand c;
            c.type = DrawCommand::Type::DrawGlyph;
            c.texture = tid;
            c.rect = Rect(cx + static_cast<float>(g.bearingX),
                          ly + static_cast<float>(g.bearingY),
                          static_cast<float>(g.width), static_cast<float>(g.height));
            c.srcRect = Rect(static_cast<float>(g.x), static_cast<float>(g.y),
                             static_cast<float>(g.width), static_cast<float>(g.height));
            c.color = style.color;
            c.blend = static_cast<int>(obj->Blend());
            if (flowing && !ClipQuad(clipRect, c.rect, c.srcRect))
                continue;
            cmds.push_back(c);
            cx += g.advance + style.letterSpacing;
            if (ch == ' ')
                cx += style.wordSpacing + justifiedExtraPerSpace(line);
        }
    }
}

namespace {
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

namespace {
uint64_t ClippedArea(const Rect& r, int frameW, int frameH) {
    const float x0 = std::clamp(r.x, 0.0f, static_cast<float>(frameW));
    const float y0 = std::clamp(r.y, 0.0f, static_cast<float>(frameH));
    const float x1 = std::clamp(r.Right(), 0.0f, static_cast<float>(frameW));
    const float y1 = std::clamp(r.Bottom(), 0.0f, static_cast<float>(frameH));
    if (x1 <= x0 || y1 <= y0)
        return 0;
    return static_cast<uint64_t>((x1 - x0) * (y1 - y0));
}

uint64_t CommandWork(const DrawCommand& cmd, int frameW, int frameH) {
    switch (cmd.type) {
        case DrawCommand::Type::Clear:
            return static_cast<uint64_t>(frameW) * static_cast<uint64_t>(frameH);
        case DrawCommand::Type::FillRect:
        case DrawCommand::Type::DrawShape:
            return ClippedArea(cmd.rect, frameW, frameH);
        case DrawCommand::Type::BlitImage:
        case DrawCommand::Type::DrawGlyph: {
            const uint64_t dest = ClippedArea(cmd.rect, frameW, frameH);
            const uint64_t src = ClippedArea(cmd.srcRect, frameW, frameH);
            return src > dest ? src : dest;
        }
        case DrawCommand::Type::PushEffect:
        case DrawCommand::Type::PopEffect: {
            const uint64_t region = ClippedArea(cmd.rect, frameW, frameH);
            const double radius = std::max(1.0, static_cast<double>(cmd.effectParam));
            const double taps = (cmd.effect == 0 || cmd.effect == 1) ? radius * radius : 1.0;
            return static_cast<uint64_t>(static_cast<double>(region) * taps);
        }
        case DrawCommand::Type::SetBlend:
            return 0;
    }
    return 0;
}
} // namespace

Result<RgbaImage> RenderEngine::Render(std::string_view sceneId, const RenderOptions& opts) {
    if (!backend_) return Error::Make(Err::Render_BackendUnavailable, "Render", "no backend");
    auto scene = scenes_.GetScene(sceneId);
    if (!scene.ok()) return scene.error();

    RgbaImage frame;
    double frameMs = 0.0;
    bool dropped = false;
    uint32_t commandCount = 0;
    uint64_t frameNumber = 0;
    // One frame at a time: the backend framebuffer and the lazy texture uploads
    // (DrawObject → gpu_.AcquireTexture) are not safe under concurrent renders.
    {
    std::lock_guard<std::mutex> frameLock(renderMutex_);

    const auto frameStart = std::chrono::steady_clock::now();
    frameNumber = frameCounter_.fetch_add(1) + 1;
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
        if (scene.value()->TransparentBackground())
            clear.color.a = 0.0f;
        cmds.push_back(clear);
    }

    auto objects = CollectObjects(sceneId);
    contentScrollChars_ = opts.contentScrollChars;
    contentScrollFromScene_ = opts.contentScrollFromScene;
    const auto layers = scene.value()->LayersSorted();
    std::vector<ElementCost> costs;
    uint64_t totalWork = 0;
    // The clear is the scene's own background, and it is real work — charged to
    // a named element so the shares add up to the whole frame.
    if (!cmds.empty() && cmds.front().type == DrawCommand::Type::Clear)
        totalWork += CommandWork(cmds.front(), w, h);
    auto layerNameFor = [&layers](const std::string& id) -> std::string {
        for (const auto& l : layers)
            if (l.id == id)
                return l.name;
        return id;
    };
    pipeline_.Distribute(objects, layers);
    pipeline_.Execute([&](PassId pass, const std::vector<RenderObject*>& objs) {
        (void)pass;
        for (auto* obj : objs) {
            const size_t before = cmds.size();
            DrawObject(obj, cmds);
            ElementCost cost;
            cost.layerId = obj->LayerId();
            cost.layerName = layerNameFor(cost.layerId);
            cost.objectId = obj->Id();
            cost.objectName = obj->Name();
            for (size_t i = before; i < cmds.size(); ++i) {
                cost.pixels += CommandWork(cmds[i], w, h);
                ++cost.commands;
            }
            totalWork += cost.pixels;
            costs.push_back(std::move(cost));
        }
    });
    // The scroll is over for this frame: nothing outside the draw loop may see a
    // scene drawn at an offset (see the member's comment in the header).
    contentScrollChars_ = 0.0f;
    contentScrollFromScene_ = false;

    {
        static std::mutex logMutex;
        static std::map<std::string, std::string> lastState;
        int nClear = 0, nFill = 0, nBlit = 0, nGlyph = 0, nShape = 0;
        std::string firstCmds;
        for (const DrawCommand& c : cmds) {
            switch (c.type) {
                case DrawCommand::Type::Clear: ++nClear; break;
                case DrawCommand::Type::FillRect: ++nFill; break;
                case DrawCommand::Type::BlitImage: ++nBlit; break;
                case DrawCommand::Type::DrawGlyph: ++nGlyph; break;
                case DrawCommand::Type::DrawShape: ++nShape; break;
                // Effects and blend changes are state commands, not draw
                // primitives counted by this dispatch summary.
                case DrawCommand::Type::PushEffect:
                case DrawCommand::Type::PopEffect:
                case DrawCommand::Type::SetBlend: break;
            }
            if (firstCmds.size() < 220)
                firstCmds += std::format(" [{}({:.0f},{:.0f},{:.0f}x{:.0f})]",
                                         static_cast<int>(c.type), c.rect.x, c.rect.y,
                                         c.rect.width, c.rect.height);
        }
        const std::string state = std::format(
            "objects={} layers={} cmds={} size={}x{} backend={} clear={} fill={} blit={} glyph={} shape={} |{}",
            objects.size(), layers.size(), cmds.size(), w, h,
            backend_ ? backend_->Capabilities().name : "none",
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

    lastCommandCount_.store(static_cast<uint32_t>(cmds.size()));
    if (auto r = backend_->Submit(cmds); !r.ok()) return r.error();
    if (auto r = backend_->EndFrame(); !r.ok()) return r.error();

    // Readback + distribute to outputs.
    if (opts.capture || opts.distribute) {
        auto rb = backend_->Readback();
        if (!rb.ok()) return rb.error();
        frame = rb.value();
        if (!frame.empty())
            ApplyFilters(frame, opts.filters);
        // Transparency inspection (opt-in): make the alpha VISIBLE downstream.
        if (alphaInspect_.load(std::memory_order_relaxed) && !frame.empty())
            CompositeOverCheckerboard(frame);
    }
    if (opts.distribute) {
        Frame out;
        out.sceneId = std::string(sceneId);
        out.frame = frameNumber;
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
    lastFrameMs_ = std::chrono::duration<double, std::milli>(
                       frameEnd.time_since_epoch() - frameStart.time_since_epoch())
                       .count();
    frameMs = lastFrameMs_;

    {
        if (!cmds.empty() && cmds.front().type == DrawCommand::Type::Clear) {
            ElementCost clear;
            clear.objectId = "scene:background";
            clear.objectName = "Background clear";
            clear.isBackground = true;   // accounted for, never the one NAMED
            clear.pixels = CommandWork(cmds.front(), w, h);
            clear.commands = 1;
            costs.push_back(std::move(clear));
        }
        std::sort(costs.begin(), costs.end(),
                  [](const ElementCost& a, const ElementCost& b) { return a.pixels > b.pixels; });
        if (costs.size() > FrameCostProfile::kProfileItems)
            costs.resize(FrameCostProfile::kProfileItems);
        for (ElementCost& c : costs) {
            c.sharePct = totalWork > 0 ? 100.0 * static_cast<double>(c.pixels)
                                             / static_cast<double>(totalWork)
                                       : 0.0;
            c.estimatedMs = frameMs * c.sharePct / 100.0;
        }
        FrameCostProfile profile;
        profile.frame = frameNumber;
        profile.frameMs = frameMs;
        profile.totalPixels = totalWork;
        profile.items = std::move(costs);
        costProfiles_[std::string(sceneId)] = profile;
        costLast_ = profile;
    }
    dropped = frameMs > 16.67;
    if (dropped) ++frameDrops_;
    uploader_.Pump();
    frameGraph_.BeginFrame();
    frameGraph_.EndFrame();
    commandCount = static_cast<uint32_t>(cmds.size());
    }   // renderMutex_ released here

    // Published with the render lock RELEASED (see the note where it is taken).
    if (dropped)
        (void)EventBus::Instance().Publish(events::RenderFrameDropped{
            std::string(sceneId), frameMs, 16.67});
    (void)EventBus::Instance().Publish(events::RenderFrameRendered{
        std::string(sceneId), frameNumber, frameMs, commandCount});
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
    s.commands = lastCommandCount_.load();
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