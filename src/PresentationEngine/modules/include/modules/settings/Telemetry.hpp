#pragma once

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

struct LiveSample {
    double composedFps = 0.0;
    double presentFps = 0.0;
    double frameMs = 0.0;
    double representMs = 0.0;
    uint64_t sharedCopies = 0;   // per-output rasters replaced by a copy

    std::string costElement;     // "Layer · Object", "" when nothing is composed
    std::string costLayer;
    std::string costObject;
    double costSharePct = 0.0;   // its share of the frame's pixel work
    double costMs = 0.0;         // that share of the frame's MEASURED cost
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

    static constexpr size_t kWindow = 128;
    static constexpr size_t kRing = 256;
    static constexpr double kTargetFps = 60.0;

    // Inlet for the one feed that has no event: OutputManager::Distribute
    // reports how many enabled outputs just received a frame.
    void OnOutputPresented(int count);

    void OnLiveFrame(double ms, bool composed);
    void OnLiveSharedCopies(uint64_t total);
    void OnLiveCost(std::string element, std::string layer, std::string object, double sharePct,
                    double ms, double backgroundPct);
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
    LiveSample lastCost_;
    std::vector<LiveSample> history_;   // oldest first, at most kLiveHistory
    mutable std::mutex liveMutex_;
};
} // namespace bps::settings