#include "modules/media/FrameSource.hpp"

namespace bps::media {

// One decoder per OS family, both behind IFrameSource. Windows keeps Media Foundation + WIC (+ the shell
// preview) — the proven, hardware-friendly path there. Linux and macOS share ONE Qt implementation
// (platform/qt/QtFrameSource.cpp: QImageReader for stills, QMediaPlayer for video posters and audio
// cover art), so a fix to picture decoding is made once and reaches both. Forward-declared here, not
// included: the engine's production code never includes a Qt header (see check_platform_boundary.mjs).
#ifdef _WIN32
std::shared_ptr<IFrameSource> MakeWindowsFrameSource();   // modules/media/WindowsFrameSource.cpp
#else
std::shared_ptr<IFrameSource> MakeQtFrameSource();        // platform/qt/QtFrameSource.cpp
#endif

std::shared_ptr<IFrameSource> MakePlatformFrameSource() {
#ifdef _WIN32
    return MakeWindowsFrameSource();
#else
    return MakeQtFrameSource();
#endif
}

} // namespace bps::media
