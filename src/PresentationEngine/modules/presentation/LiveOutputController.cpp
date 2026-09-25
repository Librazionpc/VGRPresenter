#include "modules/presentation/LiveOutputController.hpp"

#include "core/logging/Logger.hpp"
#include "modules/presentation/PresentationDocument.hpp"
#include "modules/presentation/PresentationEngine.hpp"
#include "modules/presentation/PresentationTypes.hpp"
#include "modules/presentation/SceneBuilder.hpp"
#include "modules/rendering/RenderEngine.hpp"
#include "modules/rendering/RenderTypes.hpp"

#include <chrono>

namespace bps::live {

namespace presentation = bps::presentation;
namespace rendering = bps::rendering;

namespace {
// The 60Hz render target — the same budget the RenderEngine counts drops
// against and the Telemetry module assumes.
constexpr double kFrameSeconds = 1.0 / 60.0;
// Slow re-sync with the open show's document (edits while live).
constexpr double kSyncSeconds = 1.0;
} // namespace

LiveOutputController& LiveOutputController::Instance() {
    static LiveOutputController instance;
    return instance;
}

Result<void> LiveOutputController::Initialize() {
    initialized_.store(true);
    return Ok();
}

Result<void> LiveOutputController::Start() {
    return Ok();   // the loop starts on StartFromOpenShow, not at boot
}

Result<void> LiveOutputController::Stop() {
    (void)StopLive();
    return Ok();
}

Result<void> LiveOutputController::Shutdown() {
    (void)StopLive();
    initialized_.store(false);
    return Ok();
}

Result<void> LiveOutputController::Reset() {
    (void)StopLive();
    frames_.store(0);
    return Ok();
}

HealthReport LiveOutputController::GetHealth() const {
    HealthReport r;
    r.state = HealthState::Healthy;
    r.detail = std::format("live={} frames={}", running_.load(), frames_.load());
    return r;
}

Metrics LiveOutputController::MetricsSnapshot() const {
    Metrics m;
    m.errorCount = 0;
    m.threadCount = running_.load() ? 1 : 0;
    m.health = HealthState::Healthy;
    return m;
}

// ---------------------------------------------------------------------------
// Program start/stop
// ---------------------------------------------------------------------------
Result<void> LiveOutputController::StartFromOpenShow() {
    std::lock_guard<std::mutex> lock(control_);
    if (running_.load()) return Ok();   // already live

    presentation::PresentationEngine& pres = presentation::PresentationEngine::Instance();
    auto doc = pres.Document();
    if (!doc || !doc->HasDocument())
        return Error::Make(Err::InvalidState, "LiveOutputController",
                           "no show is open; open a show before going live");

    const presentation::Presentation snapshot = doc->Snapshot();
    if (snapshot.slides.empty())
        return Error::Make(Err::InvalidState, "LiveOutputController",
                           "the open show has no slides to put on air");

    pres_ = &pres;
    boundPresentationId_.clear();
    lastCompiledCount_ = 0;

    // The preview feed: a FrameBufferOutput registered once with the render
    // engine. Every distributed frame lands here (scaled to its target) and
    // the frontend's image provider reads it back for the Show screen tile.
    if (!preview_) {
        auto existing = rendering::RenderEngine::Instance().GetOutput(kPreviewName);
        if (existing.ok())
            preview_ = std::static_pointer_cast<rendering::FrameBufferOutput>(existing.value());
        else
            preview_ = std::make_shared<rendering::FrameBufferOutput>(
                rendering::OutputKind::Preview, kPreviewName,
                rendering::Size(960, 540));
        (void)rendering::RenderEngine::Instance().AddOutput(preview_);
    }
    preview_->SetEnabled(true);

    // Bind + run the real pipeline: registry copy -> compile -> prepare -> live.
    auto open = pres.Open(snapshot.id);
    if (!open.ok())
        return open.error();
    auto compile = pres.Compile(snapshot.id);
    if (!compile.ok())
        return compile.error();
    auto prepare = pres.Prepare(snapshot.id);
    if (!prepare.ok())
        return prepare.error();
    auto live = pres.GoLive();
    if (!live.ok())
        return live.error();
    boundPresentationId_ = snapshot.id;
    lastCompiledCount_ = snapshot.slides.size();

    // Render loop — one worker thread at the frame target. Detached cleanly:
    // Stop() joins, Kernel shutdown calls Stop().
    running_.store(true);
    worker_ = std::thread([this]() { Loop(); });

    Logger::Instance().Info(
        std::format("live output started: show '{}' ({} slides)", snapshot.name,
                    snapshot.slides.size()),
        "LiveOutputController");
    return Ok();
}

Result<void> LiveOutputController::StopLive() {
    bool wasRunning = running_.exchange(false);
    if (wasRunning) {
        if (worker_.joinable())
            worker_.join();
        Logger::Instance().Info("live output stopped", "LiveOutputController");
    }
    std::lock_guard<std::mutex> lock(control_);
    if (pres_) {
        (void)pres_->StopPlayback();
        pres_ = nullptr;
    }
    boundPresentationId_.clear();
    lastCompiledCount_ = 0;
    return Ok();
}

// ---------------------------------------------------------------------------
// The loop
// ---------------------------------------------------------------------------
void LiveOutputController::Loop() {
    auto last = std::chrono::steady_clock::now();
    double syncAccum = 0.0;
    uint64_t styleRevision = pres_ ? pres_->Runtime().StyleRevision() : 0;
    while (running_.load()) {
        const auto now = std::chrono::steady_clock::now();
        double dt = std::chrono::duration<double>(now - last).count();
        last = now;

        // Style changes land within one frame (FreeShow's reactive
        // output.style): the UI pushes a spec, PresentationEngine bumps the
        // runtime's revision, and the rebuild happens here on the loop
        // thread — legal while LIVE (no state-machine bounce).
        if (pres_ && pres_->Runtime().StyleRevision() != styleRevision) {
            styleRevision = pres_->Runtime().StyleRevision();
            (void)pres_->Runtime().RebuildScenes(pres_->Builder(),
                                                 rendering::RenderEngine::Instance(),
                                                 pres_->ActiveOutputStyle());
        }

        syncAccum += dt;
        if (syncAccum >= kSyncSeconds) {
            syncAccum = 0.0;
            (void)SyncWithDocument();   // best-effort; a failing sync skips a beat
        }

        (void)RenderOnce();

        // Pace to the 60Hz target (RenderOnce cost is inside the sleep calc).
        const auto frameEnd = std::chrono::steady_clock::now();
        double spent = std::chrono::duration<double>(frameEnd - now).count();
        double sleep = kFrameSeconds - spent;
        if (sleep > 0.0)
            std::this_thread::sleep_for(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::duration<double>(sleep)));
    }
}

Result<void> LiveOutputController::SyncWithDocument() {
    std::lock_guard<std::mutex> lock(control_);
    if (!pres_ || !running_.load()) return Ok();

    auto doc = pres_->Document();
    if (!doc || !doc->HasDocument()) return Ok();   // show closed; keep last state

    const presentation::Presentation snapshot = doc->Snapshot();
    const bool rebound = snapshot.id != boundPresentationId_;
    bool rebuilt = snapshot.slides.size() != lastCompiledCount_;

    if (rebound) {
        auto open = pres_->Open(snapshot.id);
        if (!open.ok()) return open.error();
        boundPresentationId_ = snapshot.id;
        lastCompiledCount_ = snapshot.slides.size();
        rebuilt = true;
    }
    if (rebuilt) {
        // Slides were added/removed while live: rebuild compiled scenes. The
        // SceneBuilder caches by scene id; the count change is the cheap
        // trigger (per-slide re-render of text still needs a scene destroy —
        // handled by JumpById below re-preparing).
        auto compile = pres_->Compile(snapshot.id);
        if (!compile.ok()) return compile.error();
        auto prepare = pres_->Prepare(snapshot.id);
        if (!prepare.ok()) return prepare.error();
        lastCompiledCount_ = snapshot.slides.size();
    }
    return Ok();
}

std::string LiveOutputController::CurrentSceneId() const {
    if (!pres_) return {};
    const presentation::Slide* slide = pres_->CurrentSlide();
    if (!slide) return {};
    // The COMPILED slide's sceneId — the one Prepare/RebuildScenes wrote
    // (it carries the output-style fingerprint). Deriving the plain id here
    // instead would ignore the style entirely (and, since styled scenes are
    // fingerprinted, point at a scene that was never built).
    for (const auto& cs : pres_->Runtime().Compiled().slides)
        if (cs.slideId == slide->id)
            return cs.sceneId;
    return {};
}

Result<void> LiveOutputController::RenderOnce() {
    std::lock_guard<std::mutex> lock(control_);
    if (!pres_ || !running_.load()) return Ok();

    // Advance playback (cues, auto-advance) by the real frame time.
    (void)pres_->Runtime().Tick(kFrameSeconds);

    // Render the current slide's scene. The compiled set maps the current
    // slide to its PREPARED scene id (style fingerprint included); a slide
    // missing from the compiled set (hidden, or edited in mid-show before the
    // next sync) simply renders nothing this frame — same as before.
    const presentation::Presentation* active = pres_->Runtime().ActivePresentation();
    if (!active) return Ok();
    const size_t idx = pres_->CurrentIndex();
    if (idx >= pres_->Runtime().Compiled().slides.size()) return Ok();
    const std::string sceneId = pres_->Runtime().Compiled().slides[idx].sceneId;

    rendering::RenderOptions opts;
    opts.distribute = true;   // -> every enabled output -> Telemetry output meter
    auto frame = rendering::RenderEngine::Instance().Render(sceneId, opts);
    if (frame.ok())
        frames_.fetch_add(1);
    return Ok();
}

} // namespace bps::live
