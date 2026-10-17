#include "modules/rendering/RenderEngine.hpp"

#include "core/config/ConfigurationManager.hpp"
#include "core/events/Events.hpp"
#include "core/logging/Logger.hpp"
#include "core/resources/ResourceManager.hpp"
#include "modules/adaptive/AdaptiveRuntime.hpp"
#include "modules/content/ContentManager.hpp"
#include "modules/rendering/D3D11Backend.hpp"   // the GPU backend's factory (no D3D11 type in it)

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

namespace {

// THE DEFAULT RENDER BACKEND: the CPU rasterizer, with the GPU as the user's own
// choice (Settings · General's "Hardware rendering (GPU)", off out of the box).
//
// WHY SOFTWARE IS THE DEFAULT: it draws the same frames on every machine,
// including the laptops with no usable card, so the app's picture never depends
// on what a machine happens to have. The GPU path is no longer the reason it used
// to be, though — see the history below, which is kept because it is what the
// regression test guards.
//
// THE FAULT THAT MADE THIS A STOPGAP (2026-10-16, FIXED): in the sessions that
// ran on the GPU, EVERY output frame came back holding nothing but its clear. The
// evidence is in engine.log for 2026-10-16: the command list of a live styled
// scene is non-empty
//     render dispatch: [..._tpl-3_996ca59675a5f6be] objects=4 cmds=98 glyph=95 shape=2
// while the very same frame reads
//     output frame sanity: '__out_17803898930370586661__' meanLuma=0.0 maxLuma=0 maxAlpha=0 allBlack=yes
// (alpha 255 black for the opaque-#000000 style, alpha 0 for the transparent
// one: the clear landed, the objects did not). The app's own surfaces hide it —
// the monitor tiles and the output window draw their own QML copy of the slide —
// and the NDI feed, which reads ENGINE pixels, was the one surface that showed
// the loss: a black receiver while the operator read "sending - 3134 frames -
// 2 monitor(s)". The same buffer painted before the GPU backend existed
// (02:16 that day: meanLuma=41.2 allBlack=no), and no session since the first
// "render backend: requested 'auto' -> active 'directx11'" at 03:44 has.
//
// ...and the CAUSE was in the backend, not in the readback: Submit unbound the
// render target at the end of every frame (it must not stay attached while it is
// read back) and never bound one itself — EnsureFrame's binding is only redone
// when the frame SIZE changes, which a steady live session never does. So the
// second and every later frame at one size drew nothing at all, while the clear
// (which takes the render target directly) still landed; the first frames after a
// GO LIVE or a screen change were the ones that looked right. Submit now binds
// its own target, and the case is pinned by `rendering · d3d11-stage-frame`, which
// renders the app's own scene shape at 1920x1080 and compares the GPU frame to the
// CPU one TWICE per size (the first frame is the one that always worked). The CPU
// rasterizer stays the default because it works everywhere and the GPU is a
// deliberate opt-in, not because of this.
constexpr const char *kDefaultRenderBackend = "software";

// The GPU path, named the way the setting, the log and the docs all name it.
constexpr const char *kHardwareRenderBackend = "directx11";

// The key the APP writes when a user turns Settings · General's "Hardware
// rendering (GPU)" on or off (SettingsService owns the other end). It is a
// PREFERENCE, not a selection: render.backend below outranks it, which is what
// keeps an explicit profile/developer choice working.
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

    // Where the request comes from is ResolveBackendName's business (one rule
    // for the boot and for every switch after it). Whichever one a run uses is
    // logged, so it is never a guess.
    SelectBackend(ConfiguredBackendName());

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
    // The pair the app reads back goes with the backends: a shutdown that left
    // them behind would have the next Initialize report a name it is not
    // drawing with.
    {
        std::lock_guard<std::mutex> lock(backendNameMutex_);
        requestedBackendName_.clear();
        activeBackendName_.clear();
    }
    initialized_.store(false);
    return Ok();
}

Result<void> RenderEngine::Reload() {
    // Reload re-reads the request (a profile that switches backends applies with
    // a reload, and so does the General screen's hardware switch — SettingsService
    // calls exactly this) and re-installs whatever it names — the same selection
    // the boot ran, so a switch to the GPU and a switch back are one code path.
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

// WHAT THE BACKEND IS ASKED FOR — the rule, in one place, for the boot and for
// every later switch (a boot and a switch that answered differently is the class
// of bug this whole file's comments keep naming):
//
//   1. `render.backend`              an EXPLICIT choice — a config file, a
//                                    profile, a developer, a test. It WINS, and
//                                    that is deliberate: it is the escape hatch
//                                    this engine has always had (and it is how
//                                    every rendering test pins software).
//   2. else `render.hardwareAcceleration`
//                                    the app's own preference — Settings ·
//                                    General's "Hardware rendering (GPU)",
//                                    which SettingsService writes here when the
//                                    user turns it on or off.
//   3. else                         the software rasterizer.
//
// 3 is the default on purpose, and it is NOT the same "auto" that precedes it in
// SelectBackend below: "auto" means "the GPU when a device can really be made",
// and that is what a fresh install of this build must not do — the software
// rasterizer draws the same frames on every machine, including the ones with no
// GPU at all (see kDefaultRenderBackend for the readback fault that decided it).
// Asking for the GPU — by the switch or by render.backend — still FALLS BACK to
// software when no device can be made, and the active name the log prints is what
// says which one came up.
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

// The engine's backend contract, in one place (the live loop, the unit tests
// and the app all go through it):
//
//   "null"            headless accounting (CI, servers, tests)
//   "software"        the CPU rasterizer — always available
//   "directx11"       the GPU rasterizer; if no device can be made the engine
//                     runs the software one and SAYS SO (a machine that cannot
//                     do D3D11 is a fallback case, not an error the app dies on)
//   "auto"            the GPU when a device can be made, software otherwise
//   anything else     Render_BackendUnavailable, as before
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
        // The glyph atlases and layouts the render cache holds carry TextureIds
        // from the backend that created them, so a REAL switch drops them; the
        // registry's own textures are released inside BindBackend above. Both
        // are no-ops on the boot path (there is nothing cached yet).
        cache_.Clear();
    }
    // What the app reads back afterwards (ActiveBackendName / RequestedBackendName),
    // recorded BEFORE the log line so a caller that saw the line in the log and
    // then asked reports the same pair. The lock guards two string assignments
    // and nothing else — no device, no wait (see the header).
    {
        std::lock_guard<std::mutex> lock(backendNameMutex_);
        requestedBackendName_ = name;
        activeBackendName_ = backend_->Capabilities().name;
    }
    // ONE line, on the real switch only: which backend this process is drawing
    // with, so "the live frames are on the GPU" is a fact in the log rather
    // than an assumption about the setting.
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

// The last frame's cost split by element, for the scene the caller names (the
// live loop asks for the scene it composed, not for whichever pass rendered
// last — a per-output pass may have been the most recent render).
FrameCostProfile RenderEngine::CostProfileFor(std::string_view sceneId) const {
    std::lock_guard<std::mutex> lock(renderMutex_);
    const auto it = costProfiles_.find(std::string(sceneId));
    // A scene with no frame of its own reports NOTHING rather than another
    // scene's last profile: the caller is asking which element made THIS scene's
    // frame expensive (the live loop naming its on-air hotspot), and a plausible
    // name borrowed from a different scene is worse than no name at all.
    // LastCostProfile() is the deliberate way to ask for "any scene's last".
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
        if (!up.ok()) {
            // NOT A SILENT RETURN. An atlas that cannot be uploaded takes the
            // WHOLE TEXT OBJECT down with it — every glyph of it — so the frame
            // goes out with its background and nothing else, which is the
            // report "the output became black at 1920x1080 with the GPU on"
            // arriving with no reason in the log at all (the image and video
            // paths above have always said theirs). Logged once per atlas, with
            // the numbers that name the cause.
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

namespace {
// THE CURRENCY OF THE COST PROFILE (see FrameCostProfile): the pixel work one
// command asks for. The rasterizer's cost is dominated by the pixels a command
// touches, so that is what an element is charged for:
//  - a clear, fill or shape pays for the area it covers (clipped to the frame);
//  - a blit or a glyph pays the LARGER of the destination it fills and the
//    source it samples — a minified draw goes through the filter's box taps and
//    pays for the source area, which is why a 4K picture shown small is not
//    cheap;
//  - an effect region pays for its region, and a blur/glow for its region times
//    its radius (the taps are the expensive part).
// The numbers are a SHARE, not a stopwatch: the frame's measured cost is split
// by work, and each element's estimatedMs says so outright.
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

    // The frame ITSELF and the two diagnostics events outlive the lock: the
    // events are published after renderMutex_ is released (a subscriber may
    // re-enter the engine, and this lock is held for a whole frame), so what
    // they carry is read out here and the frame is handed back outside.
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
    // WHERE THE FRAME'S WORK WENT (see FrameCostProfile): the pipeline hands the
    // objects over one at a time, so the commands an object appended are exactly
    // its own and the range between them is what it is charged for.
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
    // THE PASSES ARE BUILT FROM THE OBJECT LIST FIRST: Execute only draws what
    // Distribute put into them, so dropping this call is not "one less step" —
    // it renders an empty pipeline (the frame is the clear alone) and every
    // content assertion downstream reads a black output.
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
        // WHICH BACKEND DREW THIS, named by the backend itself. This read
        // `backend_ == nullBackend_.get() ? "null" : "software"` — so every
        // D3D11 frame in the log was labelled SOFTWARE, which is how a session
        // whose frames came back holding nothing but their clear could be read
        // as a software-rasterizer fault (and how two diagnosis rounds went
        // looking in the wrong backend entirely).
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

    if (auto r = backend_->Submit(cmds); !r.ok()) return r.error();
    if (auto r = backend_->EndFrame(); !r.ok()) return r.error();

    // Readback + distribute to outputs.
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
    lastFrameMs_ = std::chrono::duration<double, std::milli>(frameEnd - frameStart).count();
    frameMs = lastFrameMs_;

    // The cost profile: the frame's work split between the elements that asked
    // for it, heaviest first, each carrying its part of the MEASURED cost. The
    // scene's clear is charged as its own element so a frame whose cost is its
    // background says so instead of looking empty.
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
