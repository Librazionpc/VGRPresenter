#include "modules/presentation/LiveOutputController.hpp"

#include "core/logging/Logger.hpp"
#include "modules/presentation/CompositorState.hpp"
#include "modules/presentation/PresentationDocument.hpp"
#include "modules/presentation/PresentationEngine.hpp"
#include "modules/presentation/PresentationTypes.hpp"
#include "modules/presentation/SceneBuilder.hpp"
#include "modules/rendering/RenderEngine.hpp"
#include "modules/rendering/RenderTypes.hpp"
#include "modules/settings/Telemetry.hpp"   // the live rate/frame-cost curve

#include <algorithm>
#include <chrono>
#include <algorithm>  // std::max, std::clamp — the glide's pace
#include <cmath>      // std::pow — the glide's curves
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

Result<void> RebindPausedRuntime(presentation::PresentationEngine& pres,
                                 const std::string& presentationId) {
    (void)pres.Close();
    auto open = pres.Open(presentationId);
    if (!open.ok()) return open.error();
    auto compile = pres.Compile(presentationId);
    if (!compile.ok()) return compile.error();
    auto prepare = pres.Prepare(presentationId);
    if (!prepare.ok()) return prepare.error();
    return Ok();
}

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
    mix(s.clearStage ? 1u : 0u);
    return h;
}

bool SamePickApartFromReading(const bps::presentation::Slide& a,
                              const bps::presentation::Slide& b) {
    return a.title == b.title && a.text == b.text && a.notes == b.notes
        && a.tags == b.tags && a.sections == b.sections && a.assetIds == b.assetIds
        && a.hidden == b.hidden && a.transitionIn == b.transitionIn
        && a.transitionMs == b.transitionMs && a.durationMs == b.durationMs
        && a.blocks == b.blocks && a.background == b.background
        && a.filters == b.filters && a.metaJson == b.metaJson
        && a.categoryId == b.categoryId && a.clearStage == b.clearStage;
}

bps::presentation::Presentation TempContent(const std::string& name,
                                            const std::vector<bps::presentation::Slide>& slides) {
    bps::presentation::Presentation content;
    content.id = "temp:" + name;
    content.name = name;
    content.slides = slides;
    for (size_t i = 0; i < content.slides.size(); ++i) {
        content.slides[i].id = std::format("{}-{}-{:016x}", content.id, i + 1,
                                           HashSlideContent(content.slides[i]));
    }
    content.createdAt = content.modifiedAt = std::chrono::system_clock::now();
    return content;
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
    auto& render = rendering::RenderEngine::Instance();

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
    ownsOnAirContent_.store(false);   // a show's document is not ours to rewrite

    liveContent_ = snapshot;

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

    (void)pres.PutPresentation(liveContent_);
    auto live = pres.PresentLive(liveContent_);
    if (!live.ok())
        return live.error();
    boundPresentationId_ = liveContent_.id;
    lastCompiledCount_ = liveContent_.slides.size();
    // THE FIRST PICTURE IS ON AIR NOW: publish it for the interface thread (see
    // OnAirSlidesSnapshot) before the loop starts reading from it.
    PublishOnAirSnapshot();

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
    presentation::Presentation content = TempContent(std::string(name), slides);

    presentation::PresentationEngine& pres = presentation::PresentationEngine::Instance();

    if (running_.load()) {
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

        liveContent_ = std::move(content);
        (void)pres.PutPresentation(liveContent_);
        auto live = pres.PresentLive(liveContent_);
        if (!live.ok()) {
            liveContent_ = presentation::Presentation{};
            return live.error();
        }

        boundPresentationId_.clear();   // not the document show; the sync stays out
        ownsOnAirContent_.store(true);  // ours to re-write (see UpdateReadingSlide)
        lastCompiledCount_ = liveContent_.slides.size();
        lastRenderedScene_.clear();
        lastStyleRevision_ = 0;
        SeedFlowCharFromContent();
        // THE FIRST PICTURE IS ON AIR (see OnAirSlidesSnapshot): the interface
        // thread reads THIS, never the runtime's raw pointer the loop owns.
        PublishOnAirSnapshot();

        running_.store(true);
        worker_ = std::thread([this]() { Loop(); });
        Logger::Instance().Info(
            std::format("live output started from content: '{}' ({} slides)", liveContent_.name,
                        liveContent_.slides.size()),
            "LiveOutputController");
        return Ok();
    }

    std::lock_guard<std::mutex> lock(control_);
    liveContent_ = std::move(content);
    auto swap = pres.SwapLiveContent(liveContent_);
    if (!swap.ok()) {
        liveContent_ = presentation::Presentation{};
        return swap.error();
    }
    ownsOnAirContent_.store(true);   // ours to re-write (see UpdateReadingSlide)
    lastCompiledCount_ = liveContent_.slides.size();
    lastRenderedScene_.clear();
    lastStyleRevision_ = 0;
    SeedFlowCharFromContent();
    PublishOnAirSnapshot();   // the slides the interface thread may now read
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

Result<void> LiveOutputController::UpdateReadingSlide(std::string_view name,
                                                     const presentation::Slide& slide) {
    if (!running_.load())
        return Error::Make(Err::InvalidState, "LiveOutputController",
                           "no reading pick is on air");
    std::lock_guard<std::mutex> pendingLock(pendingReadingMutex_);
    pendingReadingSlide_ = PendingReading{std::string(name), slide};
    return Ok();
}

void LiveOutputController::ApplyPendingReadingSlide() {
    std::optional<PendingReading> pending;
    {
        std::lock_guard<std::mutex> pendingLock(pendingReadingMutex_);
        if (!pendingReadingSlide_)
            return;
        pending = std::move(*pendingReadingSlide_);
        pendingReadingSlide_.reset();
    }
    // True while the commit turns out to need a REAL re-commit instead.
    bool commitWhole = true;
    {
        std::lock_guard<std::mutex> lock(control_);
        if (!running_.load() || !pres_)
            return;
        const presentation::Presentation* active = pres_->Runtime().ActivePresentation();
        const presentation::Slide* current = pres_->CurrentSlide();
        const bool readingWindowChanged =
            current && current->flowText != pending->slide.flowText;
        if (readingWindowChanged)
            readingFlowWindowMoved_ = true;
        if (ownsOnAirContent_.load() && current && active && active == &liveContent_) {
            presentation::Slide* onAir = nullptr;
            for (size_t i = 0; i < liveContent_.slides.size(); ++i) {
                if (&liveContent_.slides[i] == current) {
                    onAir = &liveContent_.slides[i];
                    break;
                }
            }
            if (onAir && SamePickApartFromReading(*onAir, pending->slide)) {
                onAir->flowText = pending->slide.flowText;
                onAir->flowChar = pending->slide.flowChar;
                onAir->flowBreaks = pending->slide.flowBreaks;   // cut together with the text
                lastRenderedScene_.clear();
                commitWhole = false;
                PublishOnAirSnapshot();
                Logger::Instance().Info(
                    std::format("reading page kept on air: window {} ({} char(s)), resting at "
                                "{:.2f} — no content replacement",
                                readingWindowChanged ? "moved" : "same", onAir->flowText.size(),
                                onAir->flowChar),
                    "LiveOutputController");
            }
        }
    }
    if (!commitWhole)
        return;
    {
        std::lock_guard<std::mutex> pendingLock(pendingContentMutex_);
        pendingLiveContent_ = TempContent(pending->name, {pending->slide});
        liveContentReplacementPending_.store(true);
    }
    Logger::Instance().Info(
        std::format("reading commit needs a new pick ('{}'): queued as a content replacement",
                    pending->name),
        "LiveOutputController");
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
    if (worker_.joinable())
        worker_.join();
    return StopLiveTail();   // lock-ok: the lifecycle gate is what orders it against a concurrent start (the crash it prevents)
}

void LiveOutputController::StopLiveAsync() {
    std::lock_guard<std::mutex> lifecycleLock(lifecycle_);
    if (!running_.exchange(false)) {
        (void)StopLiveTail();   // lock-ok: the lifecycle gate is what orders it against a concurrent start (see StopLive)
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
    {
        std::lock_guard<std::mutex> pendingLock(pendingReadingMutex_);
        pendingReadingSlide_.reset();
    }
    ownsOnAirContent_.store(false);   // nothing of ours is on air any more
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
        // OFF AIR: the interface thread's copy goes with it, or a surface would
        // keep composing the session that just ended.
        PublishOnAirSnapshot();
    }
    if (toClose) (void)toClose->Close();
    return Ok();
}

// ---------------------------------------------------------------------------
// The loop
// ---------------------------------------------------------------------------
void LiveOutputController::Loop() {
    auto& telemetry = settings::Telemetry::Instance();
    auto last = std::chrono::steady_clock::now();
    double syncAccum = 0.0;
    uint64_t styleRevision = pres_ ? pres_->Runtime().StyleRevision() : 0;
    while (running_.load()) {
        const auto now = std::chrono::steady_clock::now();
        double dt = std::chrono::duration<double>(now - last).count();
        last = now;

        ApplyPendingLiveContent();   // a whole new pick always wins over a page commit

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

        const int phase = lastPhase_.load();
        if (phase != static_cast<int>(Phase::Skipped)) {
            telemetry.OnLiveFrame(lastPhaseMs_.load(), phase == static_cast<int>(Phase::Composed));
            telemetry.OnLiveSharedCopies(sharedOutputCopies_.load());
        }

        if (phase == static_cast<int>(Phase::Composed)) {
            std::string scene;
            {
                std::lock_guard<std::mutex> lock(costMutex_);
                scene = composedSceneForCost_;
            }
            if (!scene.empty()) {
                const rendering::FrameCostProfile cost =
                    rendering::RenderEngine::Instance().CostProfileFor(scene);
                const rendering::ElementCost* heaviest = cost.HeaviestContent();
                if (!heaviest)
                    heaviest = cost.Heaviest();
                if (heaviest)
                    telemetry.OnLiveCost(heaviest->Label(), heaviest->layerName,
                                         heaviest->objectName, heaviest->sharePct,
                                         heaviest->estimatedMs, cost.BackgroundSharePct());
            }
        }

        // Pace to the 60Hz target (RenderOnce cost is inside the sleep calc).
        const auto frameEnd = std::chrono::steady_clock::now();
        double spent = std::chrono::duration<double>(frameEnd - now).count();
        double sleep = kFrameSeconds - spent;
        if (sleep > 0.0)
            std::this_thread::sleep_for(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::duration<double>(sleep)));
    }

    {
        std::lock_guard<std::mutex> lock(control_);
        composedFrames_.store(0);
        representedFrames_.store(0);
        sharedOutputCopies_.store(0);
        lastComposedFrame_.reset();
        lastPhase_.store(static_cast<int>(Phase::Skipped));
        lastPhaseMs_.store(0.0);
        // A NEW SESSION IS NOT MID-MOVE: the pacing gate must not inherit the last
        // session's stamp and skip the first frame of a fresh scroll.
        scrollComposedAt_ = std::chrono::steady_clock::time_point{};
        // Nor is it 'already composed': the first frame of the new session must
        // rasterise whatever position it starts from.
        hasComposedFlow_ = false;
        lastComposedFlowChars_ = 0.0f;
    }
    {
        // The scene the hotspot came from is over with the session.
        std::lock_guard<std::mutex> lock(costMutex_);
        composedSceneForCost_.clear();
    }
    telemetry.OnLiveStopped();
}

void LiveOutputController::PublishOnAirSnapshot() {
    std::vector<presentation::Slide> slides;
    size_t index = 0;
    if (pres_) {
        if (const presentation::Presentation* active = pres_->Runtime().ActivePresentation())
            slides = active->slides;
        index = pres_->CurrentIndex();
    }
    std::lock_guard<std::mutex> lock(snapshotMutex_);
    onAirSnapshot_ = std::move(slides);
    onAirIndexSnapshot_ = index;
}

std::vector<presentation::Slide> LiveOutputController::OnAirSlidesSnapshot() const {
    std::lock_guard<std::mutex> lock(snapshotMutex_);
    return onAirSnapshot_;
}

bool LiveOutputController::OnAirSlideSnapshot(presentation::Slide& out) const {
    std::lock_guard<std::mutex> lock(snapshotMutex_);
    if (onAirIndexSnapshot_ >= onAirSnapshot_.size())
        return false;
    out = onAirSnapshot_[onAirIndexSnapshot_];
    return true;
}

void LiveOutputController::OnAirPositionSnapshot(size_t& index, size_t& total) const {
    std::lock_guard<std::mutex> lock(snapshotMutex_);
    index = onAirIndexSnapshot_;
    total = onAirSnapshot_.size();
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
    ownsOnAirContent_.store(true);   // a queued pick, same ownership as a direct one
    lastCompiledCount_ = liveContent_.slides.size();
    lastRenderedScene_.clear();
    lastStyleRevision_ = 0;
    SeedFlowCharFromContent();
    readingFlowWindowMoved_ = true;
    // THE OLD SLIDES DIE WITH THIS SWAP. The interface thread reads them through
    // the snapshot, so it has to be refreshed here, before the lock is released.
    PublishOnAirSnapshot();
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

    if (!boundPresentationId_.empty()) {
        auto doc = pres_->Document();
        if (!doc || !doc->HasDocument()) return Ok();   // show closed; keep last state

        const presentation::Presentation snapshot = doc->Snapshot();
        const bool rebound = snapshot.id != boundPresentationId_;
        bool rebuilt = snapshot.slides.size() != lastCompiledCount_;

        if (rebound) {
            auto put = pres_->PutPresentation(snapshot);
            if (!put.ok()) return put.error();
            if (smIsLive()) {
                auto swap = pres_->SwapLiveContent(snapshot);
                if (!swap.ok()) return swap.error();
            } else {
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
    for (const auto& cs : pres_->Runtime().Compiled().slides)
        if (cs.slideId == slide->id)
            return cs.sceneId;
    return {};
}

// A NEW PICK rests where ITS OWN SLIDE says. The scroll position the outputs draw is the
// pushed one (FlowCharPush), which only moves while the reader steps: left alone, a pick
// made after a walk drew its passage at the previous walk's place, a different paragraph
// from the one the monitors showed. (Caller holds control_ or is the only thread yet.)
void LiveOutputController::SeedFlowCharFromContent() {
    const float rest = liveContent_.slides.empty()
        ? 0.0f : std::max(0.0f, liveContent_.slides.front().flowChar);
    flowChar_.store(rest);
    hasComposedFlow_ = false;
}

void LiveOutputController::FlowCharPush(float chars) {
    flowChar_.store(chars);
    flowCharAtMs_.store(std::chrono::duration_cast<std::chrono::milliseconds>(
                              std::chrono::steady_clock::now().time_since_epoch()).count());
    flowCharSettle_.store(true);
}

bool LiveOutputController::FlowCharMoving(int64_t nowMs) {
    const int64_t at = flowCharAtMs_.load();
    if (at != 0 && nowMs - at < kFlowCharSettleMs)
        return true;
    return flowCharSettle_.exchange(false);
}

Result<void> LiveOutputController::RenderOnce() {
    // Hoisted before control_ (plain singleton getter — lock-scope guard).
    auto& render = rendering::RenderEngine::Instance();
    ApplyPendingReadingSlide();
    std::lock_guard<std::mutex> lock(control_);
    if (!pres_ || !running_.load()) return Ok();

    // Advance playback (cues, auto-advance) by the real frame time.
    (void)pres_->Runtime().Tick(kFrameSeconds);

    const presentation::Presentation* active = pres_->Runtime().ActivePresentation();
    if (!active) return Ok();
    const size_t idx = pres_->CurrentIndex();
    if (idx >= pres_->Runtime().Compiled().slides.size()) return Ok();
    const std::string sceneId = pres_->Runtime().Compiled().slides[idx].sceneId;

    const presentation::Slide* slide = pres_->CurrentSlide();
    if (readingFlowWindowMoved_ && slide && render.GetScene(sceneId).ok())
        (void)pres_->Builder().RefreshReadingFlow(render, sceneId, *slide);

    if (slide)
        (void)pres_->Builder().BuildSlideScene(*active, *slide, pres_->ActiveOutputStyle(),
                                               render);

    const auto& compositor = presentation::CompositorState::Instance();
    const bool hasLiveCompositorContent = !compositor.GetTakenInput().deviceId.empty()
        || compositor.HasMediaFrame() || compositor.HasActiveOverlays()
        || compositor.HasBusFrames();
    const uint64_t styleRev = pres_->Runtime().StyleRevision();
    const rendering::SlideFilterValues activeFilters =
        pres_->CurrentSlide() ? pres_->CurrentSlide()->filters : rendering::SlideFilterValues{};
    const bool filtersChanged = !(activeFilters == lastFilters_);
    const int64_t nowSteadyMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                                    std::chrono::steady_clock::now().time_since_epoch()).count();
    const float scrollChars = flowChar_.load();
    const bool scrollMoving = FlowCharMoving(nowSteadyMs);
    // ---- THE SCROLL'S FRAME BUDGET ---------------------------------------
    // Composing per tick measured 0.4 s a frame — never motion. The budget is not a
    // fixed rate either: it is whatever one frame of this move actually cost, so the loop
    // spends at most half its time rasterising and the press arrives on a thread with
    // cores left.
    if (scrollMoving && scrollComposedAt_.time_since_epoch().count() != 0) {
        const auto since = std::chrono::steady_clock::now() - scrollComposedAt_;
        const auto budget = std::chrono::milliseconds(std::max<int64_t>(
            kScrollComposeMs, static_cast<int64_t>(scrollFrameMs_)));
        if (since < budget)
            return Ok();   // a frame of this move was already composed this budget
    }
    const bool freshScene = sceneId != lastRenderedScene_;
    const bool scrollUncomposed = !hasComposedFlow_ || scrollChars != lastComposedFlowChars_;
    if (!scrollMoving && !scrollUncomposed && !hasLiveCompositorContent && !filtersChanged
        && !freshScene && styleRev == lastStyleRevision_) {
        if (std::chrono::steady_clock::now() - lastRaster_
            < std::chrono::milliseconds(kHeartbeatMs))
            return Ok();
        // ---- DIRTY-FRAME SKIP: RE-PRESENT, DO NOT RE-RASTERISE ---------------
        const auto presentStart = std::chrono::steady_clock::now();
        if (RepresentLastFrame(render)) {
            RenderPerOutputPasses(scrollChars);   // lock-ok: control_ is what keeps the runtime's compiled set stable under this pass
            const double ms = std::chrono::duration<double, std::milli>(
                                  std::chrono::steady_clock::now() - presentStart).count();
            representedFrames_.fetch_add(1);
            lastRaster_ = presentStart;   // the outputs are fed: that is what this gate times
            lastPhaseMs_.store(ms);
            lastPhase_.store(static_cast<int>(Phase::Represented));
            return Ok();
        }
        // Nothing composed yet this session: fall through and compose (the gate
        // above is the only thing standing between here and the first frame).
    }

    const auto composeStart = std::chrono::steady_clock::now();
    rendering::RenderOptions opts;
    opts.distribute = true;   // -> every enabled output -> Telemetry output meter
    opts.filters = activeFilters;   // slide-wide; applied to the composed frame
    // The reading scroll for THIS frame: how far the reading block's flow sits
    // into its passage, in its own line heights. Nothing else in the scene moves.
    opts.contentScrollChars = scrollChars;
    opts.contentScrollFromScene = freshScene;
    auto frame = render.Render(sceneId, opts);
    if (frame.ok()) {
        lastRenderedScene_ = sceneId;
        lastStyleRevision_ = styleRev;
        lastFilters_ = activeFilters;
        // ...and WHICH POSITION this frame is of: what makes the settle frame (and
        // any frame the budget skipped) recognisable as still owing a compose.
        lastComposedFlowChars_ = scrollChars;
        hasComposedFlow_ = true;
        // ...and which scene this compose was OF, for the loop's cost publish
        // once RenderOnce has let go of control_ (see the member's comment).
        {
            std::lock_guard<std::mutex> lock(costMutex_);
            composedSceneForCost_ = sceneId;
        }
    }
    // Measured from the attempt (not only a success) so a scene that keeps
    // failing to rasterize is retried on the heartbeat, not every frame.
    lastRaster_ = std::chrono::steady_clock::now();

    if (frame.ok()) {
        previewFrames_.fetch_add(1);
        {
            auto composed = std::make_shared<rendering::Frame>();
            composed->sceneId = sceneId;
            composed->frame = frames_.load();
            composed->width = frame.value().width;
            composed->height = frame.value().height;
            composed->pixels = frame.value().pixels;   // the readback this pass already paid for
            composed->timestampMs = std::chrono::duration<double, std::milli>(
                                        std::chrono::system_clock::now().time_since_epoch()).count();
            lastComposedFrame_ = std::move(composed);
        }
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
    RenderPerOutputPasses(scrollChars);   // lock-ok: control_ is what keeps the runtime's compiled set stable under this pass
    // What THIS frame of the move cost — the compose plus every per-output pass — is the
    // budget the next one is paced by (see the scroll gate above). Measured from the
    // attempt, so a scene that keeps failing to rasterize paces too.
    scrollFrameMs_ = std::chrono::duration<double, std::milli>(
                         std::chrono::steady_clock::now() - composeStart).count();
    // ...and it is stamped from the END of the frame, so the budget below is time the
    // loop is free rather than time it is already busy rasterising.
    if (scrollMoving)
        scrollComposedAt_ = std::chrono::steady_clock::now();

    if (frame.ok()) {
        frames_.fetch_add(1);
        composedFrames_.fetch_add(1);
        const double ms = std::chrono::duration<double, std::milli>(
                              std::chrono::steady_clock::now() - composeStart).count();
        lastPhaseMs_.store(ms);
        lastPhase_.store(static_cast<int>(Phase::Composed));
    }
    if (pres_) {
        bool stale = false;
        {
            std::lock_guard<std::mutex> lock(snapshotMutex_);
            stale = onAirSnapshot_.empty() || pres_->CurrentIndex() != onAirIndexSnapshot_;
        }
        if (stale)
            PublishOnAirSnapshot();
    }
    return Ok();
}

// See the header: the copy that replaced the heartbeat's per-second re-raster.
bool LiveOutputController::RepresentLastFrame(rendering::RenderEngine& render) {
    const std::shared_ptr<const rendering::Frame> frame = lastComposedFrame_;
    if (!frame || frame->empty())
        return false;
    render.Distribute(*frame);
    return true;
}

void LiveOutputController::RenderPerOutputPasses(float contentScrollChars) {
    if (!pres_) return;
    const bool refreshReadingFlow = readingFlowWindowMoved_;
    readingFlowWindowMoved_ = false;
    std::vector<std::string> buffers;
    std::vector<std::pair<int, int>> sizes;
    const std::vector<presentation::OutputStyleSpec> specs = pres_->LiveOutputStyles(&buffers, &sizes);
    const presentation::Presentation* active = pres_->Runtime().ActivePresentation();
    const presentation::Slide* slide = pres_->CurrentSlide();

    rendering::RenderEngine &engine = rendering::RenderEngine::Instance();
    for (size_t i = 0; i < specs.size() && i < buffers.size(); ++i) {
        auto out = engine.GetOutput(buffers[i]);
        if (!out.ok()) {
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

        if (i < sizes.size() && sizes[i].first > 0 && sizes[i].second > 0) {
            const rendering::Size want(sizes[i].first, sizes[i].second);
            const rendering::Size have = fb->TargetSize();
            if (have.width != want.width || have.height != want.height)
                fb->SetTargetSize(want);
        }

        const rendering::Size bufSize = fb->TargetSize();
        const rendering::Size sceneSize =
            (bufSize.width > 0.0f && bufSize.height > 0.0f)
                ? bufSize
                : rendering::Size(kFrameBufferWidth, kFrameBufferHeight);

        if (!active || !slide)
            continue;   // a program pass with nothing on air: nothing to compose
        auto sceneId = pres_->Builder().BuildGatedSlideScene(
            *active, *slide, specs[i], engine, sceneSize);
        if (!sceneId.ok()) {
            fb->SetAcceptsDistributedFrames(true);
            continue;
        }
        // ---- SHARED COMPOSED FRAME: COPY, DO NOT RASTERIZE AGAIN -----------
        if (refreshReadingFlow)
            (void)pres_->Builder().RefreshReadingFlow(engine, sceneId.value(), *slide);
        if (sceneId.value() == lastRenderedScene_) {
            const bool ownedByItsOwnPass = !fb->AcceptsDistributedFrames();
            fb->SetAcceptsDistributedFrames(true);
            if (ownedByItsOwnPass && lastComposedFrame_ && !lastComposedFrame_->empty())
                (void)fb->Present(*lastComposedFrame_);
            sharedOutputCopies_.fetch_add(1);
            continue;
        }
        fb->SetAcceptsDistributedFrames(false);
        rendering::RenderOptions passOpts;
        passOpts.distribute = false;
        passOpts.capture = true;
        passOpts.filters = slide->filters;
        passOpts.contentScrollChars = contentScrollChars;
        passOpts.contentScrollFromScene = false;
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

void LiveOutputController::PreviewLoop() {
    int emptyTicks = 0;
    presentation::Slide carrier;
    bool hadLayer = false;
    while (previewRunning_.load()) {
        bool rendered = false;
        if (presPreview_) {
            presentation::Presentation show;
            bool haveSlide = false;
            auto doc = presPreview_->Document();
            if (doc && doc->HasDocument()) {
                show = doc->Snapshot();
                haveSlide = !show.slides.empty();
            }
            const auto &compositor = presentation::CompositorState::Instance();
            const bool takenLayer = !compositor.GetTakenInput().deviceId.empty()
                                    || compositor.HasMediaFrame()
                                    || compositor.HasActiveOverlays()
                                    || compositor.HasBusFrames();
            const bool paintTheClear = !haveSlide && !takenLayer && hadLayer;
            if (haveSlide || takenLayer || paintTheClear) {
                // Nothing open to stand on: the carrier (no blocks, no title,
                // transparent) carries the compositor layers and nothing else.
                if (!haveSlide)
                    show.slides.push_back(carrier);
                rendering::RenderEngine &engine = rendering::RenderEngine::Instance();
                auto sceneId = presPreview_->Builder().BuildSlideScene(
                    show, show.slides.front(), presPreview_->ActiveOutputStyle(), engine,
                    rendering::Size(kFrameBufferWidth, kFrameBufferHeight));
                if (sceneId.ok()) {
                    rendering::RenderOptions opts;
                    opts.distribute = false;   // nothing goes on air
                    opts.capture = false;      // no readback: nobody reads this frame
                    auto frame = engine.Render(sceneId.value(), opts);
                    rendered = frame.ok();
                    hadLayer = takenLayer;
                }
            }
        }
        if (rendered) {
            emptyTicks = 0;
        } else if (++emptyTicks >= kPreviewIdleTicks) {
            previewRunning_.store(false);   // no open show: give the thread back
            break;
        }
        for (int slept = 0; slept < kPreviewIntervalMs && previewRunning_.load(); slept += 50)
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
}
} // namespace bps::live