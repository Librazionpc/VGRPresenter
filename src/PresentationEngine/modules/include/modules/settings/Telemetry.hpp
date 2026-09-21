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
#include <cstdint>
#include <mutex>
#include <vector>

namespace bps::settings {

struct Utilization {
    int rendering = 0;   // % of the 60Hz frame budget, last frame
    int encoding = 0;    // busiest active encoder's load, 0 when none
    int output = 0;      // enabled-output presents in the window, % of target
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

    // Window shape: the output-rate window is kWindow recent frames
    // (128 ≈ 2.1s at the 60Hz target the % is measured against). The ring is
    // 2× the window so a frame's slot and the slot being expired never collide.
    static constexpr size_t kWindow = 128;
    static constexpr size_t kRing = 256;
    static constexpr double kTargetFps = 60.0;

    // Inlet for the one feed that has no event: OutputManager::Distribute
    // reports how many enabled outputs just received a frame.
    void OnOutputPresented(int count);

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
};

} // namespace bps::settings
