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

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace bps::presentation {
class PresentationEngine;
class Presentation;   // full type comes from PresentationTypes.hpp above
}
namespace bps::rendering {
class FrameBufferOutput;
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
    uint64_t FramesSent() const noexcept { return frames_.load(); }

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
    std::thread worker_;
    std::mutex control_;   // start/stop vs. the loop's document sync
    // START/STOP LIFECYCLE GATE: every entry (StartFrom*, StopLive) holds
    // this for its whole body so a stop can never land inside a still-
    // running start (the fast GO LIVE -> STOP crash: StopLive joined the
    // worker while StartFromSlides was mid-setup and about to assign a NEW
    // std::thread over the still-joinable one — std::terminate), and two
    // rapid stops can't race each other's reaper threads.
    std::mutex lifecycle_;
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
    // LIGHT HEARTBEAT: the wall-clock moment of the last real raster. An
    // otherwise-static on-air scene would stop producing frames entirely — and
    // with them the render-path telemetry the Settings meters read (and the
    // frame-budget accounting). At most one extra frame per kHeartbeatMs keeps
    // those numbers honest without the 60Hz core burn change-driven rendering
    // removed.
    std::chrono::steady_clock::time_point lastRaster_{};
    static constexpr int kHeartbeatMs = 1000;

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
    void RenderPerOutputPasses();
};

} // namespace bps::live
