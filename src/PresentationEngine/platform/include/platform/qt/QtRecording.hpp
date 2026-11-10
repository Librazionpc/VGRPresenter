#pragma once

namespace bps::platform {

// Can this machine's Qt Multimedia backend encode a video file (H.264, or VP9 for WebM)? What the
// Linux and macOS platforms report as PlatformCapabilities::videoRecording. False until a Qt
// application exists to host the backend (the answer is not cached before that).
bool QtVideoRecordingSupported();

} // namespace bps::platform
