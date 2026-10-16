#pragma once

// Telemetry (module/settings): LIVE resource utilization for the Settings →
// General "Resource profile" meters. The allocation (AllocationFor) is the
// profile's *budget*; this is what the engine is actually doing right now:
//   rendering — real frame times from the RenderEngine's frame-budget pass
//               (frameMs / 16.67ms @60Hz, the same line that counts drops)
//   encoding  — the busiest active recording's encoder load (RecordingEngine
//               owns the number; encoders report it through UpdateEncoderLoad)
//   output    — frames actually presented to enabled render outputs, vs the
//               60Hz target (frames that move count; idle outputs read 0)
//
// Where the numbers come from: the render path publishes RenderFrameRendered /
// RenderFrameDropped on the engine's event bus for EVERY frame; RecordingEngine
// sessions carry encoderLoad; OutputManager::Distribute reports its presents.
// This module only SUBSCRIBES and AGGREGATES — it never renders, encodes or
// presents, so idle surfaces read an honest 0 rather than a plausible fake.
//
// Threading: every write arrives on the render thread (Render() publishes the
// frame event and distributes to outputs while holding renderMutex_), so the
// per-frame ring needs no write locking; Snapshot() takes the mutex to read.

#include "core/common/Common.hpp"
#include "core/events/EventBus.hpp"
#include "interfaces/IService.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace bps::settings {

struct Utilization {
    int rendering = 0;   // % of the 60Hz frame budget, last frame
    int encoding = 0;    // busiest active encoder's load, 0 when none
    int output = 0;      // enabled-output presents in the window, % of target
};

// --- The LIVE LOOP, as a rate rather than a percentage ----------------------
// The three meters above answer "how busy is the engine"; none of them answers
// "is the show getting slower as the service goes on", because a live loop that
// composes one frame a second reads as a 2% rendering meter next to a preview
// that repaints at 60Hz. The live controller reports every iteration it takes,
// and there are exactly two kinds:
//
//   COMPOSED     a real raster — the on-air scene or its style changed
//   RE-PRESENTED the last composed frame copied to the outputs (nothing changed)
//
// so the pair (composedFps, presentFps) is what an operator feels as the show
// runs: composedFps falling with the load is the slowdown, and presentFps
// falling means the outputs stopped receiving frames at all. frameMs is the
// cost of the raster itself (the number that grows when a scene gets heavier),
// representMs the cost of the copy that replaced the old per-second re-raster.
// LiveHistory() turns that into a CURVE — one sample per whole second — so a
// slowdown over a service is visible while it happens instead of being mined
// out of the log afterwards.
struct LiveSample {
    double composedFps = 0.0;
    double presentFps = 0.0;
    double frameMs = 0.0;
    double representMs = 0.0;
    uint64_t sharedCopies = 0;   // per-output rasters replaced by a copy

    // WHICH ELEMENT MADE THE FRAME EXPENSIVE — the shape of a slowdown is not
    // enough to act on, so the composed frame's own cost profile (see
    // RenderEngine's FrameCostProfile) names the heaviest layer/element and how
    // much of the frame's work it asked for. Strings on purpose: "Images ·
    // Announcement video" is the report; a number alone would need the scene to
    // be reopened to be useful.
    std::string costElement;     // "Layer · Object", "" when nothing is composed
    std::string costLayer;
    std::string costObject;
    double costSharePct = 0.0;   // its share of the frame's pixel work
    double costMs = 0.0;         // that share of the frame's MEASURED cost
    // How much of the same frame the scene's own background clear took. It is
    // the engine's per-frame baseline rather than a layer anyone authored, so it
    // is not the element NAMED above — but a content share read without it looks
    // like the whole story when a full-frame background is where the pixels went.
    double costBackgroundPct = 0.0;
};

struct LiveStats : LiveSample {
    uint64_t composed = 0;       // totals since the live loop started
    uint64_t represented = 0;
};

class Telemetry final : public IService {
public:
    static Telemetry& Instance();

    // --- IService ---
    const char* ServiceName() const noexcept override { return "Telemetry"; }
    Result<void> Initialize() override;
    Result<void> Shutdown() override;

    // --- Reads (thread-safe; a small copy under the ring mutex) ---
    Utilization Snapshot() const;
    // The live loop's own counters (see LiveSample): the current second's rates
    // plus the running totals.
    LiveStats LiveSnapshot() const;
    // The curve: one sample per whole second of live loop, oldest first, at most
    // kLiveHistory of them. Empty until the loop has run a whole second.
    std::vector<LiveSample> LiveHistory() const;
    // How many whole-second samples LiveHistory() can hold before the oldest is
    // dropped (60 s of curve).
    static constexpr size_t kLiveHistory = 60;

    // Window shape: the output-rate window is kWindow recent frames
    // (128 ≈ 2.1s at the 60Hz target the % is measured against). The ring is
    // 2× the window so a frame's slot and the slot being expired never collide.
    static constexpr size_t kWindow = 128;
    static constexpr size_t kRing = 256;
    static constexpr double kTargetFps = 60.0;

    // Inlet for the one feed that has no event: OutputManager::Distribute
    // reports how many enabled outputs just received a frame.
    void OnOutputPresented(int count);

    // --- Inlets for the live loop (called on the render/loop thread) ---
    // One call per iteration: `composed` true for a raster, false for a
    // re-present of the last composed frame. `ms` is what that iteration cost.
    void OnLiveFrame(double ms, bool composed);
    // The live loop's RUNNING TOTAL of per-output rasters it skipped because the
    // output's scene was the composed one (same style, same size) — the copy is
    // why its frame rate stopped falling with the output count. A total rather
    // than a per-copy call, because the loop reports it outside its own lock.
    void OnLiveSharedCopies(uint64_t total);
    // The heaviest element of the frame just composed (RenderEngine's cost
    // profile): what the frame's cost went into, named, plus the share the
    // scene's own background clear took. Published on composed iterations only,
    // because a re-present draws nothing and would otherwise re-report the
    // previous frame's culprit as if it were the copy's.
    void OnLiveCost(std::string element, std::string layer, std::string object, double sharePct,
                    double ms, double backgroundPct);
    // The live loop stopped: totals and the curve belong to that session, so the
    // next one starts from zero (a curve that keeps climbing after STOP reads as
    // a load that is still there).
    void OnLiveStopped();

private:
    Telemetry() = default;

    void OnFrameRendered(double frameMs);
    void OnFrameDropped(double frameMs);

    std::vector<Subscription> subscriptions_;

    // Rendering: frame time of the most recent frame (0 when none yet).
    std::atomic<double> lastFrameMs_{0.0};

    // Output: presents per frame, indexed by frame % kRing. On frame N the
    // slot of frame N-kRing is expired (it left the window 128 frames ago).
    std::array<uint32_t, kRing> ring_{};
    uint64_t framesSeen_ = 0;          // render-thread owned
    mutable std::mutex ringMutex_;     // guards ring_ + framesSeen_ for Snapshot

    // --- Live loop (the loop thread writes; Snapshot reads under liveMutex_) --
    std::atomic<double> lastComposedMs_{0.0};
    std::atomic<double> lastRepresentMs_{0.0};
    std::atomic<uint64_t> liveComposed_{0};
    std::atomic<uint64_t> liveRepresented_{0};
    std::atomic<uint64_t> liveSharedCopies_{0};      // running total (the inlet's source)
    uint64_t lastSharedSeen_ = 0;                    // guarded by liveMutex_
    // The second being measured, closed out by the first live frame after it
    // ends. Loop-thread owned; liveMutex_ guards it for the readers.
    uint64_t secondComposed_ = 0;
    uint64_t secondRepresented_ = 0;
    uint64_t secondShared_ = 0;
    std::chrono::steady_clock::time_point secondStart_{};
    // The last hotspot reported (see OnLiveCost). Carried into each whole-second
    // sample, so the curve keeps which element was expensive at the time rather
    // than only what the last frame happened to be.
    LiveSample lastCost_;
    std::vector<LiveSample> history_;   // oldest first, at most kLiveHistory
    mutable std::mutex liveMutex_;
};

} // namespace bps::settings
