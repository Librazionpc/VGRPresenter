#include "modules/presentation/CompositorState.hpp"

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

void CompositorState::SetMediaFrame(MediaFrame frame) {
    std::lock_guard<std::mutex> lock(mutex_);
    mediaFrame_ = std::move(frame);
}

void CompositorState::ClearMedia() {
    std::lock_guard<std::mutex> lock(mutex_);
    mediaFrame_ = MediaFrame{};
}

CompositorState::MediaFrame CompositorState::GetMediaFrame() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return mediaFrame_;
}

void CompositorState::SetActiveOverlays(std::vector<ActiveOverlay> overlays) {
    std::lock_guard<std::mutex> lock(mutex_);
    activeOverlays_ = std::move(overlays);
}

std::vector<CompositorState::ActiveOverlay> CompositorState::GetActiveOverlays() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return activeOverlays_;
}

} // namespace bps::presentation
