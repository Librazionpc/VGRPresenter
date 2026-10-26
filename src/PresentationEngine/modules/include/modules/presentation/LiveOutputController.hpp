#pragma once

// LiveOutputController (Phase 8 live loop) — drives the engine's real pipeline
// continuously so outputs and telemetry see frames while a show is on air:
// the open show's document -> compile -> prepare -> GoLive, then a timer that
// renders the current slide's scene at the 60Hz target with distribute=true
// (frames flow to every enabled output; OutputManager feeds the Telemetry
// output meter, RenderEngine's frame events feed the rendering meter). The
// controller also advances the runtime playback clock so timeline cues and
// auto-advance fire like they do in the offline tests.
//
// Scene freshness: the show can change while live (the frontend edits through
// the document, and a document edit can move/add slides). The controller
// re-syncs with the document on a slow cadence (1Hz) — rebuilds when the
// compiled slide set changes and re-points the render at the runtime's current
// slide. A single render mutex in RenderEngine serializes frames with any
// ad-hoc render call.

#include "core/common/Common.hpp"
#include "core/services/ServiceManager.hpp"
#include "modules/presentation/PresentationTypes.hpp"
#include "modules/rendering/RenderOutputs.hpp"   // rendering::Frame (the re-present copy)

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace bps::presentation {
class PresentationEngine;
class Presentation;   // full type comes from PresentationTypes.hpp above
}
namespace bps::rendering {
class FrameBufferOutput;
class RenderEngine;
}

namespace bps::live {

class LiveOutputController final : public IService {
public:
    static LiveOutputController& Instance();

    LiveOutputController(const LiveOutputController&) = delete;
    LiveOutputController& operator=(const LiveOutputController&) = delete;

    // --- Lifecycle (engine-wide contract) ---
    Result<void> Initialize() override;
    Result<void> Start() override;
    Result<void> Stop() override;
    Result<void> Shutdown() override;
    Result<void> Reload() override { return Ok(); }
    Result<void> Reset() override;
    HealthReport GetHealth() const override;
    Metrics MetricsSnapshot() const override;
    const char* ServiceName() const noexcept override { return "LiveOutputController"; }

    // --- Program start/stop (the frontend's "go live" for the open show) -----
    // Takes the open show's document (the one ShowService edits), runs it
    // through the real pipeline and starts the render loop. Fails with a
    // clear message when the show has no slides.
    Result<void> StartFromOpenShow();
    // Any-content go-live (FreeShow's setOutput("slide", { id: "temp", ... })):
    // the caller hands over finished slides (scripture verses, a sermon, media
    // items) with a NAME for the on-air title; they become a temporary
    // presentation ("temp:<name>") put through the same pipeline. If a show is
    // already live its slides are REPLACED in place (no state-machine bounce —
    // the loop just re-points at the new compiled set on its next sync).
    Result<void> StartFromSlides(std::string_view name,
                                 const std::vector<presentation::Slide>& slides);
    Result<void> StopLive();
    // NON-BLOCKING stop: StopLive() joins the render worker, and on the GUI
    // thread that froze the whole app whenever the first heavy frame was mid-
    // render (AppHangB1 on a fast GO LIVE -> STOP). The reaper thread does
    // the join; the GUI thread returns immediately. Shutdown()/Reset() still
    // use the synchronous StopLive (teardown must be complete before exit).
    void StopLiveAsync();

    bool IsLive() const noexcept { return running_.load(); }

    // --- What the loop actually did (the live-rate accounting) --------------
    // COMPOSED: a real raster — the on-air scene or its style changed.
    // REPRESENTED: the last composed frame copied to the outputs because nothing
    // changed (the dirty-frame skip; see RenderOnce and RepresentLastFrame).
    // SHARED: a per-output raster that was replaced by a copy of the composed
    // frame, because that output's own scene id WAS the composed one (same
    // style, same size) — the reason the delivered rate stopped falling with
    // every enabled output. Telemetry's live section reports all three; the
    // unit tests assert them without a window (see tests_styles.cpp).
    uint64_t ComposedFrames() const noexcept { return composedFrames_.load(); }
    uint64_t RepresentedFrames() const noexcept { return representedFrames_.load(); }
    uint64_t SharedOutputCopies() const noexcept { return sharedOutputCopies_.load(); }
    bool IsLiveContentReplacementPending() const noexcept {
        return liveContentReplacementPending_.load();
    }
    uint64_t FramesSent() const noexcept { return frames_.load(); }
    // The EARLY publish: the same loop, one step earlier. FramesSent() moves
    // only after the whole iteration (main pass + every per-output pass) has
    // written its buffers — right for a per-output tile, wrong for the LIVE
    // PREVIEW, which the main pass has already written. The UI polls this one
    // so a clicked paragraph appears on the monitor preview without waiting
    // out the (heavier) per-output passes. See the two-phase publish in
    // RenderOnce().
    uint64_t PreviewFrames() const noexcept { return previewFrames_.load(); }

    // --- Idle preview (a show is open but nothing is on air) ---------------
    // Renders the open show's first slide at a light cadence with
    // distribute=false, so the engine keeps producing frames while nothing is
    // on air: the render-path telemetry the Settings meters read stays live
    // instead of sitting at zero, and nothing is ever sent to an output. The
    // LIVE loop's state is untouched — this is its own worker, its own flag,
    // and it never binds the runtime — so a preview can neither put anything
    // on air nor disturb a go-live. Started/stopped by the frontend; the
    // worker also stops itself a few seconds after the open show disappears.
    // Idempotent, and safe to call from the GUI thread.
    Result<void> StartIdlePreview();
    void StopIdlePreview();
    bool IsPreviewing() const noexcept { return previewRunning_.load(); }


    // Test seam: render one frame now (the loop body's single iteration).
    Result<void> RenderOnce();

    // ---- THE READING SCROLL (Alt+Down / Alt+Up) ----------------------------
    //
    // The reading block's flow is slid under its box by ONE number, in line
    // heights of that text (see RenderOptions::contentScrollLines). The monitor
    // tile animates the step in QML; the HDMI window, the NDI feed and the
    // recordings are THIS loop's raster, so the loop has to be told the same
    // number every frame. That is what this is: the interface thread's animator
    // (LiveOutputService::readingScroll) pushes the number, and while it is
    // moving the loop stops skipping unchanged frames so the projector sees a
    // scroll rather than a cut.
    //
    // Safe from the GUI thread: two atomic stores (no lock, no device, no engine
    // call), which is what lets the QML step call it directly.
    void SetLineScroll(float lines) { LineScrollPush(lines); }
    // The offset the outputs are drawn with: the stored number, and whether it is
    // still moving ("keep composing" — see LineScrollMoving).
    float LineScrollLines() const { return lineScroll_.load(); }

    // The preview output the loop feeds (its name in the RenderEngine's
    // OutputManager). Registered at first StartFromOpenShow and kept (a
    // disabled output receives nothing — empty frames before go-live are
    // fine); the frontend's image provider reads its last frame.
    static constexpr const char *kPreviewName = "__live_preview__";
    // Every FrameBufferOutput (the shared preview AND every per-output
    // buffer) renders at this size — was 960x540 (fine for the small
    // in-app preview tile), but a real per-output window
    // (qml/components/OutputWindow.qml, a borderless window covering a
    // real physical/HDMI screen) stretches this to full screen size, where
    // 960x540 reads visibly soft. 1080p is the practical ceiling most
    // presentation displays run at; a specific output wanting sharper than
    // that is a real per-output resolution feature, not scope for this
    // constant.
    static constexpr int kFrameBufferWidth = 1920;
    static constexpr int kFrameBufferHeight = 1080;

private:
    LiveOutputController() = default;

    void Loop();
    // The dirty-frame path's second half: hand the last COMPOSED frame to the
    // outputs again (a scaled copy per output, no raster). False when nothing has
    // been composed yet, which is the caller's cue to compose instead.
    bool RepresentLastFrame(rendering::RenderEngine& render);
    void ApplyPendingLiveContent();
    // The shared off-air cleanup both stop paths run (unbind the runtime,
    // clear per-session state). Caller holds lifecycle_.
    Result<void> StopLiveTail();
    // The idle preview's own loop — see StartIdlePreview. Exits on its own
    // when the open show has been gone for kPreviewIdleTicks ticks.
    void PreviewLoop();
    // Stop the preview worker; callers already hold lifecycle_.
    void StopIdlePreviewLocked();
    // Re-sync the runtime with the open show's document (1Hz): rebind if the
    // presentation id changed, rebuild scenes if the compiled set changed.
    // Skipped entirely while temp content is on air (StartFromSlides).
    Result<void> SyncWithDocument();
    // The runtime's state machine is in Live? (guards the swap-vs-open choice.)
    bool smIsLive() const;
    // Scene id of the runtime's current slide (empty when none).
    std::string CurrentSceneId() const;

    std::atomic<bool> running_{false};
    std::atomic<bool> initialized_{false};
    std::atomic<uint64_t> frames_{0};
    // See ComposedFrames()/RepresentedFrames()/SharedOutputCopies().
    std::atomic<uint64_t> composedFrames_{0};
    std::atomic<uint64_t> representedFrames_{0};
    std::atomic<uint64_t> sharedOutputCopies_{0};
    // The last COMPOSED frame: re-presented on the dirty-frame path, and handed
    // once to a per-output buffer that has just been switched to the shared
    // composed frame (so it takes the copy THIS frame instead of next).
    std::shared_ptr<const rendering::Frame> lastComposedFrame_;
    // What the last iteration did and what it cost, published to Telemetry by
    // Loop() — OUTSIDE control_ (never call across the engine under this lock).
    enum class Phase : int { Skipped = 0, Composed = 1, Represented = 2 };
    std::atomic<int> lastPhase_{static_cast<int>(Phase::Skipped)};
    std::atomic<double> lastPhaseMs_{0.0};
    // Bumped by the MAIN pass, before the per-output passes run (see
    // PreviewFrames()). Never used as a frame count — it is a "the preview is
    // one step newer" signal for the UI's image refetch.
    std::atomic<uint64_t> previewFrames_{0};
    // Steady-clock ms of the last live content REPLACE (0 = none waiting to be
    // reported). Written by the GUI thread that swapped the content, read by
    // the loop, reported once — it is the operator-visible latency the click
    // pays ("it delays before it updates on the monitor preview"), measured
    // instead of guessed.
    std::atomic<int64_t> replaceStampMs_{0};
    std::thread worker_;
    std::mutex control_;   // start/stop vs. the loop's document sync
    // START/STOP LIFECYCLE GATE: every entry (StartFrom*, StopLive) holds
    // this for its whole body so a stop can never land inside a still-
    // running start (the fast GO LIVE -> STOP crash: StopLive joined the
    // worker while StartFromSlides was mid-setup and about to assign a NEW
    // std::thread over the still-joinable one — std::terminate), and two
    // rapid stops can't race each other's reaper threads.
    std::mutex lifecycle_;
    // Live picks are produced by the UI thread, but replacing a presentation
    // recompiles scenes for every output. Queue the newest pick here so the
    // render worker can apply it without making the UI wait on control_.
    std::mutex pendingContentMutex_;
    std::optional<presentation::Presentation> pendingLiveContent_;
    std::atomic<bool> liveContentReplacementPending_{false};
    // The stop reaper: joins the render worker OFF the GUI thread (StopLive
    // keeps its synchronous form for Shutdown/Reset, which must finish
    // before the process ends). Started detached per async stop; lifecycle_
    // keeps concurrent reapers serialized.
    // START/STOP LIFECYCLE GATE: every entry (StartFrom*, StopLive) holds
    // this for its whole body so a stop can never land inside a still-
    // running start (the fast GO LIVE -> STOP crash: StopLive joined the
    // worker while StartFromSlides was mid-setup and about to assign a NEW
    // std::thread over the still-joinable one — std::terminate). Also keeps
    // two rapid stops from racing each other's reaper.
    // Change-driven rendering: the loop only rasterizes when the on-air scene
    // actually changed (slide/style move). A static verse re-rendered at
    // 60Hz burned a whole core and dragged the whole app down with it.
    std::string lastRenderedScene_;
    uint64_t lastStyleRevision_ = 0;
    // The filters the last composed frame was rendered with. They never touch a
    // scene id or the style revision (they run on the finished frame), so this
    // is what lets a filter edit break the change-driven skip instead of leaving
    // the previous picture on air until something else moves.
    rendering::SlideFilterValues lastFilters_;
    // WHICH SCENE THE LAST COMPOSE WAS OF, so the loop can ask the renderer for
    // that scene's cost profile AFTER RenderOnce returns. The name is a small
    // string under its own mutex on purpose: the loop must not call across the
    // engine (RenderEngine::CostProfileFor) while holding control_ — the
    // lock-scope guard exists for exactly that shape — and control_ is also the
    // lock the GUI thread's stop/golive path waits on. A dedicated mutex keeps
    // the profile fetch out of that path entirely.
    std::string composedSceneForCost_;
    std::mutex costMutex_;
    // LIGHT HEARTBEAT: the wall-clock moment of the last real raster. An
    // otherwise-static on-air scene would stop producing frames entirely — and
    // with them the render-path telemetry the Settings meters read (and the
    // frame-budget accounting). At most one extra frame per kHeartbeatMs keeps
    // those numbers honest without the 60Hz core burn change-driven rendering
    // removed.
    std::chrono::steady_clock::time_point lastRaster_{};
    static constexpr int kHeartbeatMs = 1000;

    // THE READING SCROLL (see SetLineScroll): the offset itself, when it last
    // moved, and the store that keeps the two together. While the offset is
    // within kLineScrollSettleMs of its last move the loop composes every tick
    // (a scroll in flight must not be skipped as "nothing changed"), and one
    // frame past that it composes once more so the LAST value of the move lands
    // instead of the second-to-last.
    void LineScrollPush(float lines);
    bool LineScrollMoving(int64_t nowMs);
    std::atomic<float> lineScroll_{0.0f};
    std::atomic<int64_t> lineScrollAtMs_{0};
    std::atomic<bool> lineScrollSettle_{false};
    static constexpr int kLineScrollSettleMs = 60;

    // IDLE PREVIEW: its own worker + flag, kept out of the live path's state
    // on purpose (see StartIdlePreview). One frame a second — the same light
    // cadence as the live heartbeat; a 60Hz idle re-raster is exactly the core
    // burn the live loop's change-skip exists to prevent.
    std::atomic<bool> previewRunning_{false};
    std::thread previewWorker_;
    presentation::PresentationEngine* presPreview_ = nullptr;
    static constexpr int kPreviewIntervalMs = 1000;
    // How many empty preview ticks in a row (no open show) before the worker
    // gives its thread back.
    static constexpr int kPreviewIdleTicks = 3;

    // The engine (fetched at Start, held by pointer — it is a Kernel
    // singleton that outlives this controller's worker thread).
    presentation::PresentationEngine* pres_ = nullptr;
    std::string boundPresentationId_;
    size_t lastCompiledCount_ = 0;
    // The on-air content the controller OWNS when StartFromSlides put it there
    // (the runtime binds a raw pointer — this member keeps it alive). Empty
    // while the working show is on air (StartFromOpenShow).
    presentation::Presentation liveContent_;
    std::shared_ptr<rendering::FrameBufferOutput> preview_;   // the QML feed

    // PER-OUTPUT PASSES: every frame, RenderOnce renders ONE EXTRA PASS per
    // entry of PresentationEngine's live-output style set — each into its own
    // named FrameBufferOutput, each composed under THAT output's spec (its
    // gates decide which families render content vs background-only). The
    // monitor tiles read their output's buffer; the main pass keeps feeding
    // the active output + preview. Buffers register on first use and are
    // re-resolved by name every frame (a push can add/rename entries).
    // `contentScrollLines` is the reading scroll for this frame (see
    // SetLineScroll): an output with its OWN styled scene rasterises it
    // separately, so it needs the same offset or the scroll would reach only the
    // outputs that copy the program frame.
    void RenderPerOutputPasses(float contentScrollLines = 0.0f, bool contentScrollFromScene = false);
};

} // namespace bps::live
