#include "modules/settings/Telemetry.hpp"

#include "modules/recording/RecordingEngine.hpp"

#include <algorithm>
#include <cmath>

namespace bps::settings {

namespace {
// The live frame budget in ms (60Hz), the same budget RenderEngine counts
// drops against — rendering utilization is "how much of that budget the
// most recent frame used".
constexpr double kBudgetMs = 1000.0 / Telemetry::kTargetFps;
} // namespace

Telemetry& Telemetry::Instance() {
    static Telemetry s;
    return s;
}

Result<void> Telemetry::Initialize() {
    if (!subscriptions_.empty())
        return Ok();   // already wired (Initialize is idempotent by contract)

    // The render path publishes one of these per frame — the feed is the
    // engine's own frame accounting, never sampled on a timer.
    subscriptions_.push_back(EventBus::Instance().Subscribe<events::RenderFrameRendered>(
        [this](const events::RenderFrameRendered& e) { OnFrameRendered(e.frameMs); }));
    subscriptions_.push_back(EventBus::Instance().Subscribe<events::RenderFrameDropped>(
        [this](const events::RenderFrameDropped& e) { OnFrameDropped(e.frameMs); }));
    return Ok();
}

Result<void> Telemetry::Shutdown() {
    for (auto& s : subscriptions_)
        (void)EventBus::Instance().Unsubscribe(s);
    subscriptions_.clear();
    lastFrameMs_.store(0.0);
    {
        std::lock_guard<std::mutex> lock(ringMutex_);
        ring_.fill(0);
        framesSeen_ = 0;
    }
    return Ok();
}

void Telemetry::OnFrameRendered(double frameMs) {
    lastFrameMs_.store(frameMs);
    // Render thread only: expire the slot leaving the 128-frame window, then
    // open a fresh slot for the frame that just arrived. The ring is 2× the
    // window, so the expired slot cannot be the one presents are landing in.
    std::lock_guard<std::mutex> lock(ringMutex_);
    ring_[framesSeen_ % kRing] = 0;
    ++framesSeen_;
}

void Telemetry::OnFrameDropped(double frameMs) {
    // A dropped frame is still a frame whose time the engine spent.
    OnFrameRendered(frameMs);
}

void Telemetry::OnOutputPresented(int count) {
    if (count <= 0)
        return;
    std::lock_guard<std::mutex> lock(ringMutex_);
    if (framesSeen_ == 0)
        return;   // presents before the first frame have no window slot
    ++ring_[(framesSeen_ - 1) % kRing];   // the frame range being distributed
}

Utilization Telemetry::Snapshot() const {
    Utilization u;

    // Rendering: the last frame against the 60Hz budget. 0 until frames flow.
    u.rendering = static_cast<int>(std::lround(std::clamp(
        lastFrameMs_.load() / kBudgetMs * 100.0, 0.0, 100.0)));

    // Encoding: the busiest session that still occupies an encoder — the
    // engine's own max, computed under the recording lock. 0 when idle.
    u.encoding = std::clamp(
        recording::RecordingEngine::Instance().MaxActiveEncoderLoad(), 0, 100);

    // Output: presents across the most recent window of frames vs the frames
    // the window should have carried at the 60Hz target (one present per
    // frame = 100%; several outputs on one frame saturate, not "over").
    uint64_t presents = 0;
    {
        std::lock_guard<std::mutex> lock(ringMutex_);
        const uint64_t windowFrames = std::min<uint64_t>(framesSeen_, kWindow);
        if (windowFrames > 0) {
            for (uint64_t i = framesSeen_ - windowFrames; i < framesSeen_; ++i)
                presents += ring_[i % kRing];
            u.output = static_cast<int>(std::lround(
                std::clamp(static_cast<double>(presents) / static_cast<double>(windowFrames),
                           0.0, 1.0) *
                100.0));
        }
    }
    return u;
}

} // namespace bps::settings
