#include "modules/presentation/LiveOutputController.hpp"

#include "core/logging/Logger.hpp"
#include "modules/presentation/CompositorState.hpp"
#include "modules/presentation/PresentationDocument.hpp"
#include "modules/presentation/PresentationEngine.hpp"
#include "modules/presentation/PresentationTypes.hpp"
#include "modules/presentation/SceneBuilder.hpp"
#include "modules/rendering/RenderEngine.hpp"
#include "modules/rendering/RenderTypes.hpp"

#include <algorithm>
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

// The PAUSED rebind's engine work: unbind the stale runtime, then open,
// compile and prepare the new presentation. Deliberately a free function with
// no controller lock in scope — SyncWithDocument RELEASES control_ for it,
// because closing/opening the runtime and compiling every scene is real work
// that must not run under the controller lock (lock-scope guard,
// tools/check_lock_scope.mjs).
Result<void> RebindPausedRuntime(presentation::PresentationEngine& pres,
                                 const std::string& presentationId) {
    // StopPlayback only transitions Live/Paused -> Stopped; the runtime stays
    // BOUND to the (now stale) presentation, so a later Open fails AlreadyOpen.
    // Close() is the real unbind: null-safe, idempotent, and it clears the
    // stale compiled set — without it the next go-live dies at "already open"
    // and the UI keeps rendering the previous content's last frame.
    (void)pres.Close();
    auto open = pres.Open(presentationId);
    if (!open.ok()) return open.error();
    auto compile = pres.Compile(presentationId);
    if (!compile.ok()) return compile.error();
    auto prepare = pres.Prepare(presentationId);
    if (!prepare.ok()) return prepare.error();
    return Ok();
}

// A content fingerprint for StartFromSlides' generated slide ids — see its
// own comment: a POSITION-only id ("temp:<name>-<i+1>") collides whenever the
// same presentation NAME is reused across different single-slide pushes,
// which is exactly what a double-click on a verse row does (ShowCenter.qml's
// stageSlides(root.item.name, [slide]) — the song's name, not the verse's own
// identity, every time). SceneBuilder/RenderEngine cache scenes by this
// fingerprinted id, so a collision serves the FIRST verse's cached scene
// forever on every later verse of the SAME song — reported live as "the real
// output window freezes on an older verse, only stop+go-live fixes it"
// (stop tears the presentation down, so the next go-live's id happens to
// miss the stale cache; nothing in between ever did). Folding the slide's
// own content into the id keeps the "reuse the cache for a genuine re-pick
// of the identical passage" intent the position-only scheme was going for,
// while guaranteeing two DIFFERENT verses never share one.
uint64_t HashSlideContent(const bps::presentation::Slide& s) {
    uint64_t h = 1469598103934665603ull;   // FNV offset basis
    const auto mix = [&h](uint64_t v) {
        h ^= v;
        h *= 1099511628211ull;             // FNV prime
    };
    const auto mixStr = [&h, &mix](std::string_view sv) {
        mix(sv.size());
        for (const char c : sv) mix(static_cast<unsigned char>(c));
    };
    mixStr(s.id);
    mixStr(s.title);
    mixStr(s.text);
    mix(s.blocks.size());
    for (const auto& b : s.blocks) {
        mixStr(b.id);
        mixStr(b.kind);
        mixStr(b.text);
        mixStr(b.bind);
    }
    return h;
}
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
    StopIdlePreview();
    return Ok();
}

Result<void> LiveOutputController::Shutdown() {
    (void)StopLive();
    StopIdlePreview();
    initialized_.store(false);
    return Ok();
}

Result<void> LiveOutputController::Reset() {
    (void)StopLive();
    StopIdlePreview();
    frames_.store(0);
    previewFrames_.store(0);
    replaceStampMs_.store(0);
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
    // Resolved BEFORE the two gates below: RenderEngine::Instance() is a plain
    // static getter, and a cross-engine call under a held lock is what the
    // lock-scope guard forbids (tools/check_lock_scope.mjs).
    auto& render = rendering::RenderEngine::Instance();

    // LIFECYCLE GATE (see the header): a stop landing inside this start used
    // to join the worker mid-setup and then assign a new std::thread over a
    // still-joinable handle — std::terminate (the fast GO LIVE -> STOP
    // crash). Held for the WHOLE body; the loop's own syncs use control_ as
    // before, so rendering is never blocked by this gate.
    std::lock_guard<std::mutex> lifecycleLock(lifecycle_);
    std::lock_guard<std::mutex> lock(control_);
    if (running_.load()) return Ok();   // already live

    // The live loop owns the pipeline from here on; the idle preview would
    // only compete with it for the render engine.
    StopIdlePreviewLocked();

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
        auto existing = render.GetOutput(kPreviewName);
        if (existing.ok())
            preview_ = std::static_pointer_cast<rendering::FrameBufferOutput>(existing.value());
        else
            preview_ = std::make_shared<rendering::FrameBufferOutput>(
                rendering::OutputKind::Preview, kPreviewName,
                rendering::Size(kFrameBufferWidth, kFrameBufferHeight));
        (void)render.AddOutput(preview_);
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
    // Hoisted out of both gates below (plain singleton getter) — see
    // StartFromOpenShow's note and tools/check_lock_scope.mjs.
    auto& render = rendering::RenderEngine::Instance();

    if (slides.empty())
        return Error::Make(Err::InvalidState, "LiveOutputController",
                           "nothing selected to put on air");
    // Same lifecycle gate as StartFromOpenShow (the fast GO LIVE -> STOP
    // crash) — held for the whole body.
    std::lock_guard<std::mutex> lifecycleLock(lifecycle_);
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
        // Slide ids fold in the slide's OWN content (see HashSlideContent):
        // a genuine re-pick of the identical passage still lands on the same
        // id (the scene cache stays warm, the original intent here), but two
        // DIFFERENT verses of the SAME song — pushed one at a time under the
        // SAME presentation name, exactly what a double-click on a verse row
        // in ShowCenter.qml does — never collide onto one cached scene.
        content.slides[i].id = std::format("{}-{}-{:016x}", content.id, i + 1,
                                           HashSlideContent(content.slides[i]));
    }
    content.createdAt = content.modifiedAt = std::chrono::system_clock::now();

    presentation::PresentationEngine& pres = presentation::PresentationEngine::Instance();

    if (running_.load()) {
        // Replacing a live pick recompiles scenes for every configured output.
        // Queue it for the render worker instead of waiting on control_ here:
        // with HDMI and NDI active, that wait can freeze the UI long enough
        // for Windows to report AppHangB1. The newest pick supersedes any
        // not-yet-applied pick, matching the latest-wins output pipeline.
        std::lock_guard<std::mutex> pendingLock(pendingContentMutex_);
        pendingLiveContent_ = std::move(content);
        liveContentReplacementPending_.store(true);
        return Ok();
    }

    // Live takes priority over the idle preview (see StartFromOpenShow).
    StopIdlePreviewLocked();

    if (!running_.load()) {
        std::lock_guard<std::mutex> lock(control_);
        pres_ = &pres;

        // The preview feed (same registration as StartFromOpenShow).
        if (!preview_) {
            auto existing = render.GetOutput(kPreviewName);
            if (existing.ok())
                preview_ = std::static_pointer_cast<rendering::FrameBufferOutput>(existing.value());
            else
                preview_ = std::make_shared<rendering::FrameBufferOutput>(
                    rendering::OutputKind::Preview, kPreviewName,
                    rendering::Size(kFrameBufferWidth, kFrameBufferHeight));
            (void)render.AddOutput(preview_);
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
    // Start the operator-visible clock: the loop reports how long this replace
    // took to reach the preview (see the publish in RenderOnce).
    replaceStampMs_.store(std::chrono::duration_cast<std::chrono::milliseconds>(
                              std::chrono::steady_clock::now().time_since_epoch()).count());
    Logger::Instance().Info(
        std::format("live output replaced with content: '{}' ({} slides)", liveContent_.name,
                    liveContent_.slides.size()),
        "LiveOutputController");
    return Ok();
}

Result<void> LiveOutputController::StopLive() {
    // LIFECYCLE GATE: serializes against StartFrom* so a stop can never join
    // a half-built start (or let a start assign over a joinable thread).
    std::lock_guard<std::mutex> lifecycleLock(lifecycle_);
    bool wasRunning = running_.exchange(false);
    if (wasRunning) {
        if (worker_.joinable())
            worker_.join();
        Logger::Instance().Info("live output stopped", "LiveOutputController");
    }

    // ---- THREAD LIFECYCLE MOP-UP (the stop-then-start crash's other half):
    // the reaper below detaches itself, so the handle it was given may need
    // releasing here if IT won the race to flip running_ but hadn't detached
    // yet. If joinable, THIS (serialized) caller does the join — exactly one
    // owner, no double-join, no thread-over-thread assignment.
    if (worker_.joinable())
        worker_.join();
    return StopLiveTail();
}

// The GUI-thread-safe stop: returns immediately, the join happens on a
// detached reaper. Used by LiveOutputService::stop() (the GO LIVE/STOP
// button's path) — joining on the GUI thread froze the whole app whenever
// the first heavy frame was mid-render (AppHangB1, "press go live and
// immediately unpress"), because the worker can be inside its longest
// single iteration (first RenderOnce: style load + scene build + full-
// frame rasterize + distribute) when running_ flips.
void LiveOutputController::StopLiveAsync() {
    std::lock_guard<std::mutex> lifecycleLock(lifecycle_);
    if (!running_.exchange(false)) {
        // Nothing running: still run the tail once — a stop arriving while
        // StartFrom* HELD the gate (queued behind it) must clean up whatever
        // that start half-built. lifecycle_ is held, so this is serialized.
        (void)StopLiveTail();
        return;
    }
    // Take the worker handle by moving it out under the gate — StopLive()'s
    // mop-up (or a later start) can then never see a joinable stale handle.
    std::thread doomed = std::move(worker_);
    worker_ = std::thread();
    std::thread reaper([this, doomed = std::move(doomed)]() mutable {
        if (doomed.joinable())
            doomed.join();
        Logger::Instance().Info("live output stopped", "LiveOutputController");
    });
    reaper.detach();   // lifecycle_ (held above) serializes any concurrent stop
}

// The shared off-air cleanup both stop paths run: unbind the runtime, clear
// the per-session state. Caller holds lifecycle_; takes control_ itself.
Result<void> LiveOutputController::StopLiveTail() {
    {
        std::lock_guard<std::mutex> pendingLock(pendingContentMutex_);
        pendingLiveContent_.reset();
        liveContentReplacementPending_.store(false);
    }
    // Take the runtime pointer and clear the session state under control_, then
    // run the unbind with the lock RELEASED: Close() tears down the compiled
    // set and stops playback, which is real work the lock-scope guard (rightly)
    // refuses to see under a lock. While it runs the controller is already off
    // air — pres_ is null, so the render loop's early-out applies.
    presentation::PresentationEngine* toClose = nullptr;
    {
        std::lock_guard<std::mutex> lock(control_);
        toClose = pres_;
        pres_ = nullptr;
        boundPresentationId_.clear();
        lastCompiledCount_ = 0;
        liveContent_ = presentation::Presentation{};   // the runtime no longer points here
        lastRenderedScene_.clear();
        lastStyleRevision_ = 0;
    }
    if (toClose) (void)toClose->Close();
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

        ApplyPendingLiveContent();

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

void LiveOutputController::ApplyPendingLiveContent() {
    std::optional<presentation::Presentation> pending;
    {
        std::lock_guard<std::mutex> pendingLock(pendingContentMutex_);
        if (!pendingLiveContent_)
            return;
        pending = std::move(pendingLiveContent_);
        pendingLiveContent_.reset();
    }

    std::lock_guard<std::mutex> lock(control_);
    if (!running_.load() || !pres_)
        return;

    // Keep the current presentation alive until the runtime has rebound to
    // the new member contents; it stores a pointer to liveContent_.
    presentation::Presentation previous = std::move(liveContent_);
    liveContent_ = std::move(*pending);
    auto swap = pres_->SwapLiveContent(liveContent_);
    if (!swap.ok()) {
        liveContent_ = std::move(previous);
        auto rollback = pres_->SwapLiveContent(liveContent_);
        Logger::Instance().Warning(
            "LiveOutputController: queued live content replacement failed: " + swap.error().message
                + (rollback.ok() ? "" : "; previous content restore failed: " + rollback.error().message),
            "LiveOutputController");
        std::lock_guard<std::mutex> pendingLock(pendingContentMutex_);
        liveContentReplacementPending_.store(pendingLiveContent_.has_value());
        return;
    }

    boundPresentationId_.clear();
    lastCompiledCount_ = liveContent_.slides.size();
    lastRenderedScene_.clear();
    lastStyleRevision_ = 0;
    Logger::Instance().Info(
        std::format("live output replaced with content: '{}' ({} slides)", liveContent_.name,
                    liveContent_.slides.size()),
        "LiveOutputController");
    {
        std::lock_guard<std::mutex> pendingLock(pendingContentMutex_);
        liveContentReplacementPending_.store(pendingLiveContent_.has_value());
    }
}

Result<void> LiveOutputController::SyncWithDocument() {
    // unique_lock (not lock_guard): the paused rebind below RELEASES the
    // controller lock while RebindPausedRuntime closes/re-opens the runtime.
    std::unique_lock<std::mutex> lock(control_);
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
                // Paused: the runtime is still bound to the STALE presentation
                // (PresentationRuntime::Open refuses AlreadyOpen otherwise —
                // see StopLiveTail's comment above for the same lesson). The
                // unbind + open + compile + prepare run with control_ RELEASED:
                // compiling every scene is the expensive half of a rebind, and
                // holding the controller lock across it is what the lock-scope
                // guard forbids. Controller state is re-validated on relock.
                presentation::PresentationEngine* const target = pres_;
                lock.unlock();
                Result<void> rebind = RebindPausedRuntime(*target, snapshot.id);
                lock.lock();
                if (!pres_ || pres_ != target) return Ok();   // stopped mid-rebind
                if (!rebind.ok()) return rebind.error();
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
    // Hoisted before control_ (plain singleton getter — lock-scope guard).
    auto& render = rendering::RenderEngine::Instance();
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

    // Refresh this scene's compositor layers (taken camera/screen, media on
    // air, overlays) EVERY tick, same as RenderPerOutputPasses already does
    // for the other outputs' styled scenes below — BuildSlideScene's cache
    // hit re-runs AddCompositorLayers on an already-built sceneId instead of
    // rebuilding the slide's text/shapes. Without this, this MAIN pass's
    // compositor layers were only ever baked once (at Prepare/RebuildScenes
    // time) and a taken camera or playing media froze on its first frame for
    // the rest of the live session on the primary output.
    if (const presentation::Slide* slide = pres_->CurrentSlide())
        (void)pres_->Builder().BuildSlideScene(*active, *slide, pres_->ActiveOutputStyle(),
                                               render);

    // CHANGE-DRIVEN: skip the frame when nothing visibly moved (same scene
    // and no style rebuild since the last raster). The frame in the preview
    // buffer stays exactly as it was — re-drawing an unchanged verse at the
    // frame cadence just starved the GUI thread (the app-wide lag report)
    // for zero visual gain. Playback animations would tick via the style
    // revision/clock below; cues that mutate the scene bump the revision.
    // Live compositor content is the one exception: a taken camera/screen,
    // media on air, or an active overlay changes every frame without ever
    // touching sceneId/styleRev, so it must bypass the skip (and the
    // heartbeat throttle below) the same way a style/content change does.
    const auto& compositor = presentation::CompositorState::Instance();
    const bool hasLiveCompositorContent = !compositor.GetTakenInput().deviceId.empty()
        || compositor.HasMediaFrame() || compositor.HasActiveOverlays();
    const uint64_t styleRev = pres_->Runtime().StyleRevision();
    if (!hasLiveCompositorContent && sceneId == lastRenderedScene_ && styleRev == lastStyleRevision_) {
        // ...but a LIGHT HEARTBEAT: a fully static scene would otherwise stop
        // producing frames, so the render-path telemetry the Settings meters
        // read never advances (they sit at their last value, or at 0 if the
        // engine has not rasterized once since boot). At most one extra frame
        // per kHeartbeatMs keeps the numbers honest without the 60Hz re-raster
        // that starved the GUI thread.
        if (std::chrono::steady_clock::now() - lastRaster_
            < std::chrono::milliseconds(kHeartbeatMs))
            return Ok();
    }

    rendering::RenderOptions opts;
    opts.distribute = true;   // -> every enabled output -> Telemetry output meter
    auto frame = render.Render(sceneId, opts);
    if (frame.ok()) {
        lastRenderedScene_ = sceneId;
        lastStyleRevision_ = styleRev;
    }
    // Measured from the attempt (not only a success) so a scene that keeps
    // failing to rasterize is retried on the heartbeat, not every frame.
    lastRaster_ = std::chrono::steady_clock::now();

    // TWO-PHASE PUBLISH. The live preview (and every output the main pass
    // distributes to) is on the wire NOW — bump before the per-output passes
    // so the operator sees a clicked paragraph immediately instead of waiting
    // out the heavier per-output work below (reported live: "when I click a
    // paragraph it delays before it updates on the monitor preview").
    //
    // Safe to publish early OR LATE, because the two write different buffers:
    // a per-output-pass buffer turns AcceptsDistributedFrames() OFF, so the
    // main pass never touches it (see OutputManager::Distribute) — nothing the
    // per-output passes own can be fetched half-written here. Their own bump
    // below is what tells the per-output tiles to refetch once their styled
    // frame has landed (that race is why the late bump has to keep existing:
    // a tile that fetched before its pass ran would otherwise sit on a 1x1
    // placeholder until the NEXT content change).
    if (frame.ok()) {
        previewFrames_.fetch_add(1);
        // ...and report the click-to-preview latency ONCE per replace. This is
        // the number to read when the operator says the preview lags: it is
        // measured from the UI-thread replace to the frame being on the wire,
        // so it excludes the poll's own (bounded, 100 ms) tick.
        const int64_t stamp = replaceStampMs_.exchange(0);
        if (stamp > 0) {
            const int64_t nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                                      std::chrono::steady_clock::now().time_since_epoch()).count();
            Logger::Instance().Info(
                std::format("live preview published {} ms after the content replace",
                            nowMs - stamp),
                "LiveOutputController");
        }
    }
    // THE OTHER LIVE OUTPUTS: one gated pass per entry of the engine's
    // live-output style set (best-effort — a failure in one pass never
    // kills the main deck's frame).
    RenderPerOutputPasses();

    // frames_ is what LiveOutputService::pollTick() polls to decide whether
    // to bump frameRev (the QML image providers' cache-buster) — and
    // frameRev only bumps ONCE per change (change-driven, see above), so a
    // bump the UI catches before a slower per-output pass above has actually
    // Present()'d ITS buffer is a race the UI loses PERMANENTLY: nothing
    // ever tells it to refetch that buffer again until the NEXT content
    // change. A real OutputWindow's own per-output pass (heavier — GDI+ text
    // layout/atlas rebuilds for a family template) can legitimately take
    // longer than the main pass above it, so incrementing frames_ before
    // RenderPerOutputPasses() ran let the poll's 100ms tick fetch a
    // per-output buffer that wasn't written yet (a 1x1 placeholder, stuck
    // forever) — reported live as "the real output window shows the idle
    // background and never updates" for heavier (family-templated) content,
    // while lighter content occasionally won the race and looked fine.
    // Bumping it here, after every per-output buffer for this frame has
    // ALSO landed, closes that window.
    if (frame.ok())
        frames_.fetch_add(1);
    return Ok();
}

void LiveOutputController::RenderPerOutputPasses() {
    if (!pres_) return;
    std::vector<std::string> buffers;
    std::vector<std::pair<int, int>> sizes;
    const std::vector<presentation::OutputStyleSpec> specs = pres_->LiveOutputStyles(&buffers, &sizes);
    const presentation::Presentation* active = pres_->Runtime().ActivePresentation();
    const presentation::Slide* slide = pres_->CurrentSlide();
    if (!active || !slide) return;

    rendering::RenderEngine &engine = rendering::RenderEngine::Instance();
    for (size_t i = 0; i < specs.size() && i < buffers.size(); ++i) {
        auto out = engine.GetOutput(buffers[i]);
        if (!out.ok()) {
            // Register on first use (named per-output frame buffer). Sized to
            // THIS output's own configured resolution (Settings · Outputs'
            // Resolution field) when it pushed one — an NDI sender, a real
            // OutputWindow and a styled per-output pass all read this SAME
            // buffer, so a 4K-configured NDI output now actually sends 4K
            // instead of being silently downscaled to the fixed default; a
            // missing/unparsed entry (size {0,0}, or no sizes pushed at all —
            // every caller before this feature) falls back to it exactly as
            // before.
            const bool hasSize = i < sizes.size() && sizes[i].first > 0 && sizes[i].second > 0;
            const rendering::Size bufSize = hasSize
                ? rendering::Size(sizes[i].first, sizes[i].second)
                : rendering::Size(kFrameBufferWidth, kFrameBufferHeight);
            auto created = std::make_shared<rendering::FrameBufferOutput>(
                rendering::OutputKind::Audience, buffers[i], bufSize);
            if (!engine.AddOutput(created).ok()) continue;
            out = engine.GetOutput(buffers[i]);
            if (!out.ok()) continue;
        }
        auto *fb = dynamic_cast<rendering::FrameBufferOutput *>(out.value().get());
        if (!fb || !fb->Enabled()) continue;

        // Resolution changed (the user edited Settings · Outputs' Resolution
        // field) while this output was already registered: re-target it NOW
        // instead of only honoring the new size on a fresh GetOutput() miss —
        // a mid-session resolution edit must actually resize the frame it
        // pushes, not just the next time the buffer happens to be re-created.
        if (i < sizes.size() && sizes[i].first > 0 && sizes[i].second > 0) {
            const rendering::Size want(sizes[i].first, sizes[i].second);
            const rendering::Size have = fb->TargetSize();
            if (have.width != want.width || have.height != want.height)
                fb->SetTargetSize(want);
        }

        // THE SIZE THIS OUTPUT ACTUALLY PUSHES. The scene is built AT this
        // size (not at the 1920x1080 default and then resampled): the render
        // backend sizes itself to the SCENE (RenderEngine::Render's
        // BeginFrame(w, h)), so this is what makes an NDI/HDMI output at its
        // configured resolution a real, natively-laid-out frame — text is
        // rasterized at the target resolution instead of being a nearest-
        // neighbour blow-up of a 1080p layout (the "NDI is not following the
        // Resolution" report: the frame WAS resized to the target, its layout
        // was not). The styled scene id carries this size (see
        // StyledSceneIdFor), so two outputs at different resolutions never
        // share one cached scene.
        const rendering::Size bufSize = fb->TargetSize();
        const rendering::Size sceneSize =
            (bufSize.width > 0.0f && bufSize.height > 0.0f)
                ? bufSize
                : rendering::Size(kFrameBufferWidth, kFrameBufferHeight);

        // Build (or reuse) the GATED scene for THIS spec — each output's own
        // rules decide content vs background-only. Style-fingerprinted ids
        // keep the cache honest across edits and gate flips.
        auto sceneId = pres_->Builder().BuildGatedSlideScene(*active, *slide, specs[i], engine,
                                                             sceneSize);
        if (!sceneId.ok()) {
            // No scene of its own this frame (build refused/errored): let the
            // main pass's program frame feed this buffer again, so the output
            // shows the program feed instead of freezing on its last frame.
            fb->SetAcceptsDistributedFrames(true);
            continue;
        }
        // THIS PASS OWNS THIS BUFFER. The main pass distributes its scene to
        // every enabled output; without this the buffer was written twice per
        // frame (program scene first, then this one) and the NDI sender / real
        // output window — both reading snapshots on their own cadence — caught
        // whichever write landed last: the live report of an output "glitching
        // between the two templates". One writer per frame now.
        fb->SetAcceptsDistributedFrames(false);
        // Raster it (distribute off — this pass IS the distribution), then hand
        // the pixels straight to this output's buffer. Rendered at the buffer's
        // own size, so Present() takes its same-size path and resamples
        // nothing.
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

// ---------------------------------------------------------------------------
// Idle preview (a show is open but nothing is on air)
// ---------------------------------------------------------------------------
Result<void> LiveOutputController::StartIdlePreview() {
    if (running_.load()) return Ok();             // on air: the live loop owns it
    if (previewRunning_.load()) return Ok();      // already previewing
    std::lock_guard<std::mutex> gate(lifecycle_);
    if (running_.load() || previewRunning_.load()) return Ok();
    // Reap a worker that stopped itself (its own "no open show" exit).
    if (previewWorker_.joinable()) previewWorker_.join();
    presPreview_ = &presentation::PresentationEngine::Instance();
    previewRunning_.store(true);
    previewWorker_ = std::thread([this]() { PreviewLoop(); });
    return Ok();
}

void LiveOutputController::StopIdlePreview() {
    std::lock_guard<std::mutex> gate(lifecycle_);
    StopIdlePreviewLocked();
}

void LiveOutputController::StopIdlePreviewLocked() {
    previewRunning_.store(false);
    if (previewWorker_.joinable()) previewWorker_.join();
}

// One frame a second of the open show's first slide, off the live path
// entirely: nothing is distributed, nothing is read back, and nothing is put
// on air. The point is the FRAME — RenderEngine publishes RenderFrameRendered
// for every render, which is the feed the Settings meters read, so the engine
// no longer reads as idle just because a show is not on air.
void LiveOutputController::PreviewLoop() {
    int emptyTicks = 0;
    while (previewRunning_.load()) {
        bool rendered = false;
        if (presPreview_) {
            auto doc = presPreview_->Document();
            if (doc && doc->HasDocument()) {
                const presentation::Presentation show = doc->Snapshot();
                if (!show.slides.empty()) {
                    rendering::RenderEngine &engine = rendering::RenderEngine::Instance();
                    // The builder caches by scene id, so this rebuilds only
                    // when the slide itself changed.
                    auto sceneId = presPreview_->Builder().BuildSlideScene(
                        show, show.slides.front(), engine,
                        rendering::Size(kFrameBufferWidth, kFrameBufferHeight));
                    if (sceneId.ok()) {
                        rendering::RenderOptions opts;
                        opts.distribute = false;   // nothing goes on air
                        opts.capture = false;      // no readback: the frame is the point
                        (void)engine.Render(sceneId.value(), opts);
                        rendered = true;
                    }
                }
            }
        }
        if (rendered) {
            emptyTicks = 0;
        } else if (++emptyTicks >= kPreviewIdleTicks) {
            previewRunning_.store(false);   // no open show: give the thread back
            break;
        }
        // Sleep in short steps so a stop is felt promptly.
        for (int slept = 0; slept < kPreviewIntervalMs && previewRunning_.load(); slept += 50)
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
}

} // namespace bps::live
