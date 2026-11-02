#include "modules/settings/Telemetry.hpp"

#include "modules/recording/RecordingEngine.hpp"

#include <algorithm>
#include <cmath>

namespace bps::settings {
namespace {
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

    std::lock_guard<std::mutex> lock(liveMutex_);
    if (composed) ++secondComposed_;
    else          ++secondRepresented_;
    if (secondStart_.time_since_epoch().count() == 0) {
        secondStart_ = now;
        return;
    }
    const double elapsed = std::chrono::duration<double>(now - secondStart_).count();
    if (elapsed < 1.0)
        return;
    LiveSample sample;
    sample.composedFps = static_cast<double>(secondComposed_) / elapsed;
    sample.presentFps = static_cast<double>(secondComposed_ + secondRepresented_) / elapsed;
    sample.frameMs = lastComposedMs_.load();
    sample.representMs = lastRepresentMs_.load();
    sample.sharedCopies = secondShared_;
    sample.costElement = lastCost_.costElement;
    sample.costLayer = lastCost_.costLayer;
    sample.costObject = lastCost_.costObject;
    sample.costSharePct = lastCost_.costSharePct;
    sample.costMs = lastCost_.costMs;
    sample.costBackgroundPct = lastCost_.costBackgroundPct;
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

void Telemetry::OnLiveCost(std::string element, std::string layer, std::string object, double sharePct,
                          double ms, double backgroundPct) {
    std::lock_guard<std::mutex> lock(liveMutex_);
    lastCost_.costElement = std::move(element);
    lastCost_.costLayer = std::move(layer);
    lastCost_.costObject = std::move(object);
    lastCost_.costSharePct = sharePct;
    lastCost_.costMs = ms;
    lastCost_.costBackgroundPct = backgroundPct;
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
    // The hotspot belonged to the scene that session was showing; naming it
    // after STOP would blame a show that is no longer on air.
    lastCost_ = LiveSample{};
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
        s.costElement = lastCost_.costElement;
        s.costLayer = lastCost_.costLayer;
        s.costObject = lastCost_.costObject;
        s.costSharePct = lastCost_.costSharePct;
        s.costMs = lastCost_.costMs;
        s.costBackgroundPct = lastCost_.costBackgroundPct;
        if (!history_.empty()) {
            const LiveSample& last = history_.back();
            s.composedFps = last.composedFps;
            s.presentFps = last.presentFps;
        } else if (secondStart_.time_since_epoch().count() != 0) {
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