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
    OnLiveStopped();
    return Ok();
}

// ---------------------------------------------------------------------------
// The live loop (see LiveSample in the header for what the two kinds mean)
// ---------------------------------------------------------------------------
void Telemetry::OnLiveFrame(double ms, bool composed) {
    const auto now = std::chrono::steady_clock::now();
    if (composed) {
        lastComposedMs_.store(ms);
        liveComposed_.fetch_add(1);
    } else {
        lastRepresentMs_.store(ms);
        liveRepresented_.fetch_add(1);
    }

    // The open second and the curve are ONE state, so they share one lock: the
    // counts this iteration just added, the sample the previous second closes
    // out, and the second that is now open. The lock covers a few increments —
    // no PAL, no device, no other lock behind it.
    std::lock_guard<std::mutex> lock(liveMutex_);
    if (composed) ++secondComposed_;
    else          ++secondRepresented_;
    if (secondStart_.time_since_epoch().count() == 0) {
        secondStart_ = now;
        return;
    }
    // The rates come from the loop's own counts over its own elapsed time, not
    // from a wall clock: a loop that was held off for two seconds while the
    // machine was busy must not read as a slow loop.
    const double elapsed = std::chrono::duration<double>(now - secondStart_).count();
    if (elapsed < 1.0)
        return;
    LiveSample sample;
    sample.composedFps = static_cast<double>(secondComposed_) / elapsed;
    sample.presentFps = static_cast<double>(secondComposed_ + secondRepresented_) / elapsed;
    sample.frameMs = lastComposedMs_.load();
    sample.representMs = lastRepresentMs_.load();
    sample.sharedCopies = secondShared_;
    if (history_.size() >= kLiveHistory)
        history_.erase(history_.begin());
    history_.push_back(sample);
    secondComposed_ = 0;
    secondRepresented_ = 0;
    secondShared_ = 0;
    secondStart_ = now;
}

void Telemetry::OnLiveSharedCopies(uint64_t total) {
    liveSharedCopies_.store(total);
    std::lock_guard<std::mutex> lock(liveMutex_);
    // The count for the SECOND being measured is the delta, and a decrease is a
    // new session (the loop resets its total) — never a negative delta.
    secondShared_ += (total >= lastSharedSeen_) ? (total - lastSharedSeen_) : total;
    lastSharedSeen_ = total;
}

void Telemetry::OnLiveStopped() {
    std::lock_guard<std::mutex> lock(liveMutex_);
    history_.clear();
    secondComposed_ = 0;
    secondRepresented_ = 0;
    secondShared_ = 0;
    secondStart_ = {};
    liveComposed_.store(0);
    liveRepresented_.store(0);
    liveSharedCopies_.store(0);
    lastSharedSeen_ = 0;
    lastComposedMs_.store(0.0);
    lastRepresentMs_.store(0.0);
}

LiveStats Telemetry::LiveSnapshot() const {
    LiveStats s;
    s.composed = liveComposed_.load();
    s.represented = liveRepresented_.load();
    s.sharedCopies = liveSharedCopies_.load();
    s.frameMs = lastComposedMs_.load();
    s.representMs = lastRepresentMs_.load();
    {
        std::lock_guard<std::mutex> lock(liveMutex_);
        if (!history_.empty()) {
            const LiveSample& last = history_.back();
            s.composedFps = last.composedFps;
            s.presentFps = last.presentFps;
        } else if (secondStart_.time_since_epoch().count() != 0) {
            // The first second is still open: report what it holds so far, so a
            // snapshot taken during the FIRST second of a show is not a flat 0
            // ("live and showing nothing" is a different report from "0 fps").
            const double elapsed = std::chrono::duration<double>(
                                       std::chrono::steady_clock::now() - secondStart_).count();
            if (elapsed > 0.0) {
                s.composedFps = static_cast<double>(secondComposed_) / elapsed;
                s.presentFps = static_cast<double>(secondComposed_ + secondRepresented_) / elapsed;
            }
        }
    }
    return s;
}

std::vector<LiveSample> Telemetry::LiveHistory() const {
    std::lock_guard<std::mutex> lock(liveMutex_);
    return history_;
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
