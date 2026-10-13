#include "modules/presentation/CompositorState.hpp"

#include "core/logging/Logger.hpp"

#include <format>

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
