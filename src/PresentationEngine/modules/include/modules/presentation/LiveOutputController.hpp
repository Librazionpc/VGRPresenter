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

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace bps::presentation {
class PresentationEngine;
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
    Result<void> StopLive();

    bool IsLive() const noexcept { return running_.load(); }
    uint64_t FramesSent() const noexcept { return frames_.load(); }

    // Test seam: render one frame now (the loop body's single iteration).
    Result<void> RenderOnce();

    // The preview output the loop feeds (its name in the RenderEngine's
    // OutputManager). Registered at first StartFromOpenShow and kept (a
    // disabled output receives nothing — empty frames before go-live are
    // fine); the frontend's image provider reads its last frame.
    static constexpr const char *kPreviewName = "__live_preview__";

private:
    LiveOutputController() = default;

    void Loop();
    // Re-sync the runtime with the open show's document (1Hz): rebind if the
    // presentation id changed, rebuild scenes if the compiled set changed.
    Result<void> SyncWithDocument();
    // Scene id of the runtime's current slide (empty when none).
    std::string CurrentSceneId() const;

    std::atomic<bool> running_{false};
    std::atomic<bool> initialized_{false};
    std::atomic<uint64_t> frames_{0};
    std::thread worker_;
    std::mutex control_;   // start/stop vs. the loop's document sync

    // The engine (fetched at Start, held by pointer — it is a Kernel
    // singleton that outlives this controller's worker thread).
    presentation::PresentationEngine* pres_ = nullptr;
    std::string boundPresentationId_;
    size_t lastCompiledCount_ = 0;
    std::shared_ptr<rendering::FrameBufferOutput> preview_;   // the QML feed
};

} // namespace bps::live
