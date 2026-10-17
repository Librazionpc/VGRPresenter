#include "modules/presentation/CompositorState.hpp"

#include "core/logging/Logger.hpp"

#include <algorithm>
#include <format>
#include <iterator>

namespace bps::presentation {

CompositorState& CompositorState::Instance() {
    static CompositorState instance;
    return instance;
}

void CompositorState::SetTakenInput(std::string deviceId, std::string kind) {
    std::lock_guard<std::mutex> lock(mutex_);
    takenInput_.deviceId = std::move(deviceId);
    takenInput_.kind = std::move(kind);
}

void CompositorState::ClearTakenInput() {
    std::lock_guard<std::mutex> lock(mutex_);
    takenInput_ = TakenInput{};
}

CompositorState::TakenInput CompositorState::GetTakenInput() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return takenInput_;
}

// ONE LINE PER TRANSITION, never per frame — a video source hands a frame in at
// its own frame rate, and a per-frame log would bury engine.log. This is the
// ENGINE half of the "media hand-off" diagnostic (the UI half is in
// LiveOutputService::pushMediaFrameToCompositor): a media layer that renders
// black is either a frame the engine never received or one it received and
// mis-drew, and these two lines tell the two apart in one run.
void CompositorState::SetMediaFrame(MediaFrame frame) {
    const bool incomingEmpty = frame.empty();
    const uint32_t width = frame.width;
    const uint32_t height = frame.height;
    const size_t bytes = frame.rgba.size();
    bool flipped = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        flipped = mediaFrame_.empty() != incomingEmpty;
        mediaFrame_ = std::move(frame);
    }
    if (!flipped)
        return;   // same state as before — nothing new to report
    if (incomingEmpty)
        Logger::Instance().Info("media hand-off: engine copy CLEARED — media layer will be blank", "Compositor");
    else
        Logger::Instance().Info(std::format("media hand-off: engine copy RECEIVED {}x{} ({} bytes RGBA)",
                                            width, height, bytes), "Compositor");
}

void CompositorState::SetMediaFit(std::string fit) {
    std::lock_guard<std::mutex> lock(mutex_);
    // No frame, no layout: setting a fit here would be remembered by nothing
    // (the next SetMediaFrame carries its own from the take).
    if (mediaFrame_.empty()) return;
    mediaFrame_.fit = std::move(fit);
}

void CompositorState::ClearMedia() {
    bool hadFrame = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        hadFrame = !mediaFrame_.empty();
        mediaFrame_ = MediaFrame{};
    }
    if (hadFrame)
        Logger::Instance().Info("media hand-off: engine copy CLEARED — media layer will be blank", "Compositor");
}

CompositorState::MediaFrame CompositorState::GetMediaFrame() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return mediaFrame_;
}

bool CompositorState::HasMediaFrame() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return !mediaFrame_.empty();
}

bool CompositorState::MediaFrameIsTransparent() const {
    std::lock_guard<std::mutex> lock(mutex_);
    // A bool read under the lock: no pixel copy, nothing that could wait on a
    // device. The composition calls this on the render worker, on every build.
    return !mediaFrame_.empty() && mediaFrame_.hasAlpha;
}

// ---- Bus video planes ---------------------------------------------------
// One line per CHANGE of the feed list (never per frame): which buses the
// on-air composition asked for, and what feeds them. A bus block that shows an
// empty window on NDI/HDMI is answered from here — either the list was empty
// (the block never reached the compositor) or the bus is listed and its
// device is named (the tap, not the mirror, is what is quiet).
void CompositorState::SetBusFeeds(std::vector<BusFeed> feeds) {
    auto describe = [](const std::vector<BusFeed>& list) {
        std::string out;
        for (const BusFeed& f : list) {
            if (!out.empty()) out += ", ";
            out += std::format("bus{}<{}({})", f.busId, f.deviceId, f.kind);
        }
        return out.empty() ? std::string("none") : out;
    };
    std::string before;
    std::string after;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (feeds.size() == busFeeds_.size()
            && std::equal(feeds.begin(), feeds.end(), busFeeds_.begin(),
                          [](const BusFeed& a, const BusFeed& b) {
                              return a.busId == b.busId && a.deviceId == b.deviceId
                                  && a.kind == b.kind;
                          })) {
            return;   // unchanged — nothing to report and nothing to drop
        }
        before = describe(busFeeds_);
        after = describe(feeds);
        busFeeds_ = std::move(feeds);
        // A bus that left the composition leaves no frame behind: keeping one
        // would paint a picture into a hole on the next slide that names it.
        for (auto it = busFrames_.begin(); it != busFrames_.end();) {
            const bool listed = std::any_of(busFeeds_.begin(), busFeeds_.end(),
                                            [&it](const BusFeed& f) { return f.busId == it->first; });
            it = listed ? std::next(it) : busFrames_.erase(it);
        }
    }
    Logger::Instance().Info(std::format("bus feeds: {} -> {}", before, after), "Compositor");
}

std::vector<CompositorState::BusFeed> CompositorState::GetBusFeeds() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return busFeeds_;
}

void CompositorState::SetBusFrame(int busId, MediaFrame frame) {
    bool flipped = false;
    uint32_t width = 0;
    uint32_t height = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (busId <= 0)
            return;
        MediaFrame& slot = busFrames_[busId];
        flipped = slot.empty() != frame.empty();
        width = frame.width;
        height = frame.height;
        slot = std::move(frame);
    }
    if (!flipped)
        return;   // same state as before — one line per transition, not per frame
    if (width == 0 || height == 0)
        Logger::Instance().Info(std::format("bus video: bus{} frame CLEARED — its plane will be blank", busId),
                                "Compositor");
    else
        Logger::Instance().Info(std::format("bus video: bus{} frame RECEIVED {}x{}", busId, width, height),
                                "Compositor");
}

CompositorState::MediaFrame CompositorState::GetBusFrame(int busId) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = busFrames_.find(busId);
    return it == busFrames_.end() ? MediaFrame{} : it->second;
}

bool CompositorState::HasBusFrames() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return std::any_of(busFrames_.begin(), busFrames_.end(),
                       [](const auto& entry) { return !entry.second.empty(); });
}

void CompositorState::ClearBusFrames() {
    std::lock_guard<std::mutex> lock(mutex_);
    busFrames_.clear();
}

void CompositorState::SetActiveOverlays(std::vector<ActiveOverlay> overlays) {
    std::lock_guard<std::mutex> lock(mutex_);
    activeOverlays_ = std::move(overlays);
}

std::vector<CompositorState::ActiveOverlay> CompositorState::GetActiveOverlays() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return activeOverlays_;
}

bool CompositorState::HasActiveOverlays() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return !activeOverlays_.empty();
}

} // namespace bps::presentation
