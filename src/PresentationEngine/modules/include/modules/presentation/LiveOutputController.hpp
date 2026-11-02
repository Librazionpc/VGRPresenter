#pragma once

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
#include <string_view>
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

    Result<void> StartFromOpenShow();
    Result<void> StartFromSlides(std::string_view name,
                                 const std::vector<presentation::Slide>& slides);
    Result<void> StopLive();
    void StopLiveAsync();

    bool IsLive() const noexcept { return running_.load(); }

    uint64_t ComposedFrames() const noexcept { return composedFrames_.load(); }
    uint64_t RepresentedFrames() const noexcept { return representedFrames_.load(); }
    uint64_t SharedOutputCopies() const noexcept { return sharedOutputCopies_.load(); }
    bool IsLiveContentReplacementPending() const noexcept {
        return liveContentReplacementPending_.load();
    }
    uint64_t FramesSent() const noexcept { return frames_.load(); }
    uint64_t PreviewFrames() const noexcept { return previewFrames_.load(); }

    Result<void> StartIdlePreview();
    void StopIdlePreview();
    bool IsPreviewing() const noexcept { return previewRunning_.load(); }

    // Test seam: render one frame now (the loop body's single iteration).
    Result<void> RenderOnce();

    // ---- THE READING SCROLL (Alt+Down / Alt+Up) ----------------------------
    void SetFlowChar(float chars) { FlowCharPush(chars); }
    // The position the outputs are drawn with: the stored number, and whether it is
    // still moving ("keep composing" — see FlowCharMoving).
    float FlowChar() const { return flowChar_.load(); }

    // ---- A READING PAGE COMMIT IS AN OFFSET UPDATE, NOT A NEW PICK ----------
    Result<void> UpdateReadingSlide(std::string_view name, const presentation::Slide& slide);

    // ---- WHAT IS ON AIR, READ FROM THE INTERFACE THREAD ----------------------
    std::vector<presentation::Slide> OnAirSlidesSnapshot() const;
    bool OnAirSlideSnapshot(presentation::Slide& out) const;
    // The current index and the active set's size, from the same snapshot (what
    // the on-air mirror's title/index/total line reads).
    void OnAirPositionSnapshot(size_t& index, size_t& total) const;
    // The loop's own publish point (caller holds control_): copies the runtime's
    // active slides + current index into the snapshot above.
    void PublishOnAirSnapshot();

    static constexpr const char *kPreviewName = "__live_preview__";
    static constexpr int kFrameBufferWidth = 1920;
    static constexpr int kFrameBufferHeight = 1080;

private:
    LiveOutputController() = default;

    void Loop();
    bool RepresentLastFrame(rendering::RenderEngine& render);
    void ApplyPendingLiveContent();
    void ApplyPendingReadingSlide();
    // The shared off-air cleanup both stop paths run (unbind the runtime,
    // clear per-session state). Caller holds lifecycle_.
    Result<void> StopLiveTail();
    // The idle preview's own loop — see StartIdlePreview. Exits on its own
    // when the open show has been gone for kPreviewIdleTicks ticks.
    void PreviewLoop();
    // Stop the preview worker; callers already hold lifecycle_.
    void StopIdlePreviewLocked();
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
    std::shared_ptr<const rendering::Frame> lastComposedFrame_;
    // What the last iteration did and what it cost, published to Telemetry by
    // Loop() — OUTSIDE control_ (never call across the engine under this lock).
    enum class Phase : int { Skipped = 0, Composed = 1, Represented = 2 };
    std::atomic<int> lastPhase_{static_cast<int>(Phase::Skipped)};
    std::atomic<double> lastPhaseMs_{0.0};
    std::atomic<uint64_t> previewFrames_{0};
    std::atomic<int64_t> replaceStampMs_{0};
    std::thread worker_;
    std::mutex control_;   // start/stop vs. the loop's document sync
    std::mutex lifecycle_;
    std::mutex pendingContentMutex_;
    std::optional<presentation::Presentation> pendingLiveContent_;
    std::atomic<bool> liveContentReplacementPending_{false};
    struct PendingReading {
        std::string name;
        presentation::Slide slide;
    };
    std::mutex pendingReadingMutex_;
    std::optional<PendingReading> pendingReadingSlide_;
    std::atomic<bool> ownsOnAirContent_{false};
    bool readingFlowWindowMoved_ = false;
    std::string lastRenderedScene_;
    uint64_t lastStyleRevision_ = 0;
    rendering::SlideFilterValues lastFilters_;
    std::string composedSceneForCost_;
    std::mutex costMutex_;
    std::chrono::steady_clock::time_point lastRaster_{};
    static constexpr int kHeartbeatMs = 1000;

    void FlowCharPush(float chars);
    bool FlowCharMoving(int64_t nowMs);
    std::atomic<float> flowChar_{0.0f};
    std::atomic<int64_t> flowCharAtMs_{0};
    std::atomic<bool> flowCharSettle_{false};
    static constexpr int kFlowCharSettleMs = 60;

    std::chrono::steady_clock::time_point scrollComposedAt_{};
    static constexpr int kScrollComposeMs = 50;
    float lastComposedFlowChars_ = 0.0f;
    bool hasComposedFlow_ = false;

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
    presentation::Presentation liveContent_;
    std::shared_ptr<rendering::FrameBufferOutput> preview_;   // the QML feed

    void RenderPerOutputPasses(float contentScrollChars = 0.0f);

    // The interface thread's copy of what is on air (see OnAirSlidesSnapshot).
    mutable std::mutex snapshotMutex_;
    std::vector<presentation::Slide> onAirSnapshot_;
    size_t onAirIndexSnapshot_ = 0;
};
} // namespace bps::live