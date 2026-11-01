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
        // Packed 0xAABBGGRR (Color::Pack): opaque black. A stalled feed must be
        // black, not transparent — a keyer/compositor behind it would otherwise
        // fill the hole with whatever it likes, and a frozen last picture must
        // never be what an audience keeps looking at.
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
    // THE OBJECT'S OWN BLEND, not a hardcoded Alpha: the Edit screen's MEDIA tab
    // writes a blend mode onto the item, SceneBuilder puts it on the object, and
    // every command this object emits carries it into the rasterizer.
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
            // THE READING SCROLL is DrawTextObject's own business: a block that
            // carries a flow slides its layout under its box by the frame's
            // contentScrollChars_ and clips to the box (see that function). An
            // ordinary block has no flow and draws exactly as it always did.
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

// A QUAD CUT TO A CLIP RECT — destination AND source together, so the piece that
// survives still samples the texels that belong to it. Returns false when
// nothing is left (the command is dropped entirely).
//
// This is how a scrolled reading line leaves its box: the glyph row at the edge
// loses just the slice that crossed the boundary, instead of the whole line
// vanishing at once — the visible difference between scrolling and stepping.
// A command with no source (a fill, a band, a shadow) passes a zero-size src and
// only its destination is cut.
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

    // Real font when style.fontId names one Qt can resolve, else the builtin
    // bitmap font — same "try real, degrade
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
    // The SUBSTITUTED family, matching the atlas the cache just handed back
    // (FontManager::SubstituteFamily): two styles naming different missing fonts
    // resolve to the same installed face, and naming the texture after the raw
    // request would upload the same atlas twice under two names.
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
    // for real fonts Measure used the font's actual line pitch (which
    // matches the preview's natural line box), and painted spacing that disagrees with measured
    // spacing re-spreads the block — un-doing the fit solve (fit says it
    // fits; the paint overflows the box and clips the tail). Falls back to
    // the legacy formula only for a zero-value result (never produced by
    // Measure today, but a safe default if a caller hand-builds one).
    const float lineHeight = result.lineStep > 0.0f ? result.lineStep
                                                    : style.size + style.lineSpacing + style.lineGap;
    const Rect bounds = obj->Bounds();

    // A BLOCK CARRYING A READING FLOW is a VIEWPORT: its text is a continuous
    // passage (see TextObject::SetReadingFlow), the frame's scroll slides that
    // layout under the box, and the box clips it. Everything else about the
    // geometry is untouched — the box, the fitted size and the alignment are the
    // PAGE's, so the page sits exactly where the design put it and only the
    // words travel. `flowing` false is the ordinary text object, which draws
    // byte-for-byte what it always drew.
    const bool flowing = obj->HasReadingFlow();
    // THE POSITION, then the STYLE'S OWN ANSWER. What arrives is a character
    // offset into the passage (from the scene's slide, or from the animator's
    // per-frame push) — never a line, because a line number means a different
    // word in every style. `FlowLineForChar` turns it into THIS style's line
    // index (plus the fraction still in flight), using the table the scene built
    // for this object, so two outputs of different type sizes put the SAME words
    // at the top of their boxes.
    // ALIGNMENT MEASURES THE PAGE, not the whole passage: the fit solver sized
    // the box to hold one page, so one page is what it centres (or bottoms), and
    // the rest of the flow lives above and below that.
    const float alignHeight = flowing
        ? static_cast<float>(obj->FlowPageLines()) * lineHeight
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

    // THE VIEWPORT IS THE PAGE'S OWN RECTANGLE, not the block's box. A design's
    // box can be taller than the page it was fitted for (a shrink-mode page
    // leaves slack at the bottom), and clipping to the BOX would leak the lines
    // below into that slack — a passage would show the next page's text on a
    // slide nobody has stepped yet. Clipping to the page's rectangle means
    // offset 0 IS the page, exactly as the operator built it, and a step moves
    // the words inside that window and nothing else.
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

    // WHICH LINES THE VIEWPORT CAN SEE — the cull that makes a scroll cost what
    // the BOX costs instead of what the PASSAGE costs.
    //
    // The three passes below (bands, shadow, glyphs) used to walk `result.lines`
    // — the layout of the FLOW TEXT, which is the whole run a reading walk is
    // drawing, hundreds of lines for a sermon — and build one DrawCommand per
    // line (and one per GLYPH for the text), then throw almost every command
    // away inside ClipQuad because the line sat above or below the page. The
    // work was done and discarded at 60 Hz: the reading column measured 37% of
    // a composed frame (≈150–400 ms, Settings' own hotspot telemetry), which is
    // exactly the "scrolling the passage saturates the CPU" report. A line the
    // viewport cannot see now costs one index, not a line's glyphs.
    //
    // A line is visible when its own box [ly, ly + lineHeight) meets the clip
    // rect; one line of slack either side keeps a half-covered line's clip
    // exact (the fractional scroll offset means the edge line is usually cut).
    // A block with no flow keeps the full range — it is one page by construction.
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

    // PER-LINE BACKGROUND BANDS (FreeShow's lineBg) — drawn UNDER the glyphs,
    // one rectangle per visual line, each as wide as that line's own ink. This
    // is deliberately not the item's background: that one fill covers the whole
    // block, while this follows the text line by line (a verse highlighted as
    // it is read).
    //
    // The band is one LINE BOX tall — the measured `lineBox`, the font's own
    // ascent+descent — while the lines STEP by the line height. The two differ
    // on purpose, and the difference is the air between bands: the canvas and
    // the tile previews band the same box (Qt paints a rich-text span's
    // background at the natural line box), so a taller band here would make the
    // output's highlight fatter than the one approved on screen, and folding
    // lineGap into it would make neighbouring bands touch.
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
            // A RADIUS turns the band into a rounded shape; without one the plain
            // FillRect is both cheaper and byte-for-byte what earlier builds drew,
            // so an item that never asked for rounding cannot change.
            c.type = style.lineRadius > 0.0f ? DrawCommand::Type::DrawShape
                                            : DrawCommand::Type::FillRect;
            c.blend = static_cast<int>(BlendMode::Alpha);
            c.rect = Rect(lx, by, lineWidth, bandHeight);
            // Past half the band's height a radius is a stadium and nothing else;
            // clamping here keeps the software and D3D11 rasterizers handed the
            // same number (each clamps too, but the command is what the GPU path
            // uploads).
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
        // RTL: reverse visual order within the line. An LTR line is walked in
        // place — a per-line std::string copy at 60 Hz was one more allocation
        // per visible line, every frame, for a picture that never changes.
        std::string reversed;
        const std::string& visual = style.rtl
            ? (reversed.assign(line.text.rbegin(), line.text.rend()), reversed)
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
            // The glyphs are the text object's own marks, so a blended text
            // object blends its letters (its highlight bands and drop shadow
            // stay plain alpha — they sit UNDER the words, not in them).
            c.blend = static_cast<int>(obj->Blend());
            // A glyph the viewport's edge only half covers is cut, source and
            // all, so the top line of a scrolling block leaves the box a pixel
            // at a time instead of disappearing a whole line at a time.
            if (flowing && !ClipQuad(clipRect, c.rect, c.srcRect))
                continue;
            cmds.push_back(c);
            // A SPACE also carries the word spacing, exactly as TextLayout's
            // width math counts it — the drawn line and the measured line are
            // the same sum, so a wrapped block can never drift from its box.
            cx += g.advance + style.letterSpacing;
            if (ch == ' ')
                cx += style.wordSpacing + justifiedExtraPerSpace(line);
        }
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
    // THE READING POSITION for this frame (see RenderOptions::contentScrollChars):
    // one number — a character offset into the passage — read by DrawTextObject
    // when it meets the block that carries a reading flow. It lives under
    // renderMutex_ for the length of this render (the block below holds it) and is
    // cleared as the frame finishes, so a frame can never leave the shared scene
    // slid.
    contentScrollChars_ = opts.contentScrollChars;
    contentScrollFromScene_ = opts.contentScrollFromScene;
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
    // The scroll is over for this frame: nothing outside the draw loop may see a
    // scene drawn at an offset (see the member's comment in the header).
    contentScrollChars_ = 0.0f;
    contentScrollFromScene_ = false;

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

    // Published for the diagnostics and the draw-cost suite (RenderStats::commands):
    // "how many commands this frame built" is the number a scrolling frame's cost is
    // argued from, and it is only observable here.
    lastCommandCount_.store(static_cast<uint32_t>(cmds.size()));
    if (auto r = backend_->Submit(cmds); !r.ok()) return r.error();
    if (auto r = backend_->EndFrame(); !r.ok()) return r.error();

    // Readback + distribute to outputs.
    if (opts.capture || opts.distribute) {
        auto rb = backend_->Readback();
        if (!rb.ok()) return rb.error();
        frame = rb.value();
        // Slide-wide filters (opts.filters, from the slide being shown) run on
        // the FINISHED frame — after the scene has been composited, before any
        // output or capture reads a pixel. Deliberately here and not in the
        // caller: Distribute below feeds every enabled output from this one
        // frame, so filtering at this point is what keeps the program output,
        // the per-output passes and the previews showing one identical picture.
        // A no-op for an unfiltered slide (ApplyFilters checks IsIdentity).
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
