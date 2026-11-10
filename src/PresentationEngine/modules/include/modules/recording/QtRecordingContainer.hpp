#pragma once

#include "modules/recording/RecordingPlugin.hpp"

#include <memory>
#include <vector>

namespace bps::recording {

// The recording containers Qt Multimedia can ENCODE on this machine, beyond the portable WAV writer:
// the video containers (MP4, MKV, MOV, WebM) and the compressed audio-only ones (M4A, MP3, FLAC) —
// whatever the Qt backend (FFmpeg on Linux and macOS) was built with. Empty when no Qt application is
// running to host the media backend (a command-line tool, a bare test process).
std::vector<ContainerKind> QtSupportedContainers();

// One recording file written through QMediaRecorder, fed frame by frame: ONE implementation for Linux and
// macOS (the Windows build keeps Media Foundation). The Qt types stay inside platform/qt/.
std::shared_ptr<IRecordingContainer> MakeQtRecordingContainer(ContainerKind kind);

} // namespace bps::recording
