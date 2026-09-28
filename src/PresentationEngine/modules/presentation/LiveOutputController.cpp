#include "modules/presentation/LiveOutputController.hpp"

#include "core/logging/Logger.hpp"
#include "modules/presentation/PresentationDocument.hpp"
#include "modules/presentation/PresentationEngine.hpp"
#include "modules/presentation/PresentationTypes.hpp"
#include "modules/presentation/SceneBuilder.hpp"
#include "modules/rendering/RenderEngine.hpp"
#include "modules/rendering/RenderTypes.hpp"

#include <chrono>
#include <utility>

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

    // THE RUNTIME BINDS A COPY WE OWN: `snapshot` dies at scope end, and the
    // registry mirror churns (every sync PutPresentation refreshes it) — so the
    // runtime must point at the controller's own member, never at either.
    liveContent_ = snapshot;

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

    // Bind + run the real pipeline: our copy -> compile -> prepare -> live.
    // (PutPresentation mirrors the content into the registry for diagnostics;
    // PresentLive binds the RUNTIME to the controller's copy.)
    (void)pres.PutPresentation(liveContent_);
    auto live = pres.PresentLive(liveContent_);
    if (!live.ok())
        return live.error();
    boundPresentationId_ = liveContent_.id;
    lastCompiledCount_ = liveContent_.slides.size();

    // Render loop — one worker thread at the frame target. Detached cleanly:
    // Stop() joins, Kernel shutdown calls Stop().
    running_.store(true);
    worker_ = std::thread([this]() { Loop(); });

    Logger::Instance().Info(
        std::format("live output started: show '{}' ({} slides)", liveContent_.name,
                    liveContent_.slides.size()),
        "LiveOutputController");
    return Ok();
}

// ---------------------------------------------------------------------------
// Any-content start (scripture verses, sermons, media items -> on air)
// ---------------------------------------------------------------------------
Result<void> LiveOutputController::StartFromSlides(std::string_view name,
                                                   const std::vector<presentation::Slide>& slides) {
    if (slides.empty())
        return Error::Make(Err::InvalidState, "LiveOutputController",
                           "nothing selected to put on air");

    // THE CONTROLLER OWNS THE ON-AIR CONTENT: the runtime binds a raw pointer,
    // so the copy must outlive the run. A stable member keyed by content name;
    // each call REPLACES it (old slides die with the swap, and the runtime is
    // rebound in the same critical section, so no window where the pointer
    // dangles).
    presentation::Presentation content;
    content.id = std::string("temp:") + std::string(name);
    content.name = std::string(name);
    content.slides = slides;
    for (size_t i = 0; i < content.slides.size(); ++i) {
        // Slide ids stable per position: a re-pick of the same passage re-uses
        // the same scene ids, so the scene cache stays warm.
        content.slides[i].id = std::format("{}-{}", content.id, i + 1);
    }
    content.createdAt = content.modifiedAt = std::chrono::system_clock::now();

    presentation::PresentationEngine& pres = presentation::PresentationEngine::Instance();

    if (!running_.load()) {
        std::lock_guard<std::mutex> lock(control_);
        pres_ = &pres;

        // The preview feed (same registration as StartFromOpenShow).
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

        // OWN THE CONTENT BEFORE BINDING: the runtime takes a raw pointer, so
        // it must point at the member that outlives the run — never at this
        // function's local (moving or returning would leave it dangling).
        liveContent_ = std::move(content);
        (void)pres.PutPresentation(liveContent_);
        auto live = pres.PresentLive(liveContent_);
        if (!live.ok()) {
            liveContent_ = presentation::Presentation{};
            return live.error();
        }

        boundPresentationId_.clear();   // not the document show; the sync stays out
        lastCompiledCount_ = liveContent_.slides.size();
        // Fresh content: force the loop's next RenderOnce to actually rasterize
        // (scene ids are reused across picks — temp:<name>-1 every time — so
        // id equality alone can't tell new content from the old frame).
        lastRenderedScene_.clear();
        lastStyleRevision_ = 0;

        running_.store(true);
        worker_ = std::thread([this]() { Loop(); });
        Logger::Instance().Info(
            std::format("live output started from content: '{}' ({} slides)", liveContent_.name,
                        liveContent_.slides.size()),
            "LiveOutputController");
        return Ok();
    }

    // LIVE REPLACE: swap the runtime's content in place (state stays Live) and
    // rebuild scenes; the loop picks the new compiled set up the very next
    // frame. Same ownership rule as the first start: the member is assigned
    // BEFORE the runtime rebinds, so the raw pointer lands on stable storage.
    std::lock_guard<std::mutex> lock(control_);
    liveContent_ = std::move(content);
    auto swap = pres.SwapLiveContent(liveContent_);
    if (!swap.ok()) {
        liveContent_ = presentation::Presentation{};
        return swap.error();
    }
    lastCompiledCount_ = liveContent_.slides.size();
    // LIVE REPLACE: the picked paragraph changed under the SAME scene id
    // ("temp:<sermon>-1") — without this force the output would freeze on the
    // previous verse's last frame.
    lastRenderedScene_.clear();
    lastStyleRevision_ = 0;
    Logger::Instance().Info(
        std::format("live output replaced with content: '{}' ({} slides)", liveContent_.name,
                    liveContent_.slides.size()),
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
        // StopPlayback only transitions Live/Paused -> Stopped; the runtime
        // stays BOUND to the (now stale) presentation, so every later Open —
        // even for a perfectly good show — failed with AlreadyOpen. Close()
        // is the real unbind: null-safe, idempotent, and it clears the stale
        // compiled set. Without it the next go-live died at "already open"
        // and the UI kept rendering the previous content's last frame.
        (void)pres_->Close();
        pres_ = nullptr;
    }
    boundPresentationId_.clear();
    lastCompiledCount_ = 0;
    liveContent_ = presentation::Presentation{};   // the runtime no longer points here
    lastRenderedScene_.clear();
    lastStyleRevision_ = 0;
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

    // ON-AIR TEMP CONTENT (StartFromSlides): the runtime binds the controller's
    // own copy, not the working show. Any document sync here would YANK the
    // output back to the document — exactly what a scripture pick must not do
    // one second after it goes on air. Temp content only rebuilds when the
    // controller itself swaps it (SwapLiveContent).
    if (!boundPresentationId_.empty()) {
        auto doc = pres_->Document();
        if (!doc || !doc->HasDocument()) return Ok();   // show closed; keep last state

        const presentation::Presentation snapshot = doc->Snapshot();
        const bool rebound = snapshot.id != boundPresentationId_;
        bool rebuilt = snapshot.slides.size() != lastCompiledCount_;

        if (rebound) {
            // A DIFFERENT show took the document while we were live: adopt it.
            // Put first (the registry copy may predate this document), then the
            // full rebind — Open is refused while live, so the swap path runs.
            auto put = pres_->PutPresentation(snapshot);
            if (!put.ok()) return put.error();
            if (smIsLive()) {
                auto swap = pres_->SwapLiveContent(snapshot);
                if (!swap.ok()) return swap.error();
            } else {
                auto open = pres_->Open(snapshot.id);
                if (!open.ok()) return open.error();
                auto compile = pres_->Compile(snapshot.id);
                if (!compile.ok()) return compile.error();
                auto prepare = pres_->Prepare(snapshot.id);
                if (!prepare.ok()) return prepare.error();
            }
            boundPresentationId_ = snapshot.id;
            lastCompiledCount_ = snapshot.slides.size();
            rebuilt = false;
            lastRenderedScene_.clear();   // new binding: force a re-raster
        }
        if (rebuilt) {
            // Slides were added/removed while live: refresh the registry copy
            // and swap the runtime's content in place (no state transitions).
            auto put = pres_->PutPresentation(snapshot);
            if (!put.ok()) return put.error();
            if (smIsLive()) {
                auto swap = pres_->SwapLiveContent(snapshot);
                if (!swap.ok()) return swap.error();
            } else {
                auto compile = pres_->Compile(snapshot.id);
                if (!compile.ok()) return compile.error();
                auto prepare = pres_->Prepare(snapshot.id);
                if (!prepare.ok()) return prepare.error();
            }
            lastCompiledCount_ = snapshot.slides.size();
            lastRenderedScene_.clear();   // content swapped in place: re-raster
        }
    }
    return Ok();
}

bool LiveOutputController::smIsLive() const {
    return pres_ && pres_->State() == presentation::PresentationState::Live;
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

    // CHANGE-DRIVEN: skip the frame when nothing visibly moved (same scene
    // and no style rebuild since the last raster). The frame in the preview
    // buffer stays exactly as it was — re-drawing an unchanged verse at the
    // frame cadence just starved the GUI thread (the app-wide lag report)
    // for zero visual gain. Playback animations would tick via the style
    // revision/clock below; cues that mutate the scene bump the revision.
    const uint64_t styleRev = pres_->Runtime().StyleRevision();
    if (sceneId == lastRenderedScene_ && styleRev == lastStyleRevision_)
        return Ok();

    rendering::RenderOptions opts;
    opts.distribute = true;   // -> every enabled output -> Telemetry output meter
    auto frame = rendering::RenderEngine::Instance().Render(sceneId, opts);
    if (frame.ok()) {
        frames_.fetch_add(1);
        lastRenderedScene_ = sceneId;
        lastStyleRevision_ = styleRev;
    }

    // THE OTHER LIVE OUTPUTS: one gated pass per entry of the engine's
    // live-output style set (best-effort — a failure in one pass never
    // kills the main deck's frame).
    RenderPerOutputPasses();
    return Ok();
}

void LiveOutputController::RenderPerOutputPasses() {
    if (!pres_) return;
    std::vector<std::string> buffers;
    const std::vector<presentation::OutputStyleSpec> specs = pres_->LiveOutputStyles(&buffers);
    const presentation::Presentation* active = pres_->Runtime().ActivePresentation();
    const presentation::Slide* slide = pres_->CurrentSlide();
    if (!active || !slide) return;

    rendering::RenderEngine &engine = rendering::RenderEngine::Instance();
    for (size_t i = 0; i < specs.size() && i < buffers.size(); ++i) {
        auto out = engine.GetOutput(buffers[i]);
        if (!out.ok()) {
            // Register on first use (named per-output frame buffer, preview-
            // sized; the QML tile scales whatever it reads).
            auto created = std::make_shared<rendering::FrameBufferOutput>(
                rendering::OutputKind::Audience, buffers[i],
                rendering::Size(960, 540));
            if (!engine.AddOutput(created).ok()) continue;
            out = engine.GetOutput(buffers[i]);
            if (!out.ok()) continue;
        }
        auto *fb = dynamic_cast<rendering::FrameBufferOutput *>(out.value().get());
        if (!fb || !fb->Enabled()) continue;

        // Build (or reuse) the GATED scene for THIS spec — each output's own
        // rules decide content vs background-only. Style-fingerprinted ids
        // keep the cache honest across edits and gate flips.
        auto sceneId = pres_->Builder().BuildGatedSlideScene(*active, *slide, specs[i], engine);
        if (!sceneId.ok()) continue;
        // Raster WITHOUT distributing (the main pass already fed every output
        // — a second distribute would double-feed), then hand the pixels
        // straight to this output's buffer. The buffer scales to its target.
        rendering::RenderOptions passOpts;
        passOpts.distribute = false;
        passOpts.capture = true;
        if (auto img = engine.Render(sceneId.value(), passOpts); img.ok() && !img.value().empty()) {
            rendering::Frame f;
            f.sceneId = sceneId.value();
            f.width = img.value().width;
            f.height = img.value().height;
            f.pixels = std::move(img.value().pixels);
            f.timestampMs = std::chrono::duration<double, std::milli>(
                                std::chrono::system_clock::now().time_since_epoch()).count();
            (void)fb->Present(f);
        }
    }
}

} // namespace bps::live
