#pragma once

// Windows PAL backend for the video-capture subsystem (Media Foundation
// device + mode enumeration) plus a REAL Source Reader preview tap per
// previewed camera: Start spawns a drain thread, PreviewFrame returns the
// newest JPEG it published (shared-memory hand-off, lock-held copies only).

#include "platform/IVideo.hpp"

#include <map>
#include <memory>
#include <mutex>

namespace bps::platform {

class WindowsVideo final : public IVideo {
public:
    // Out-of-line ctor AND dtor — the pimpl table owns live preview threads;
    // an in-header cleanup path (an enclosing implicit constructor's
    // exception handler) would need the pimpl's full definition. (Same
    // WindowsAudio pattern.)
    WindowsVideo();
    ~WindowsVideo() override;

    std::vector<VideoDeviceInfo> Enumerate() const override;
    std::vector<WindowInfo> EnumerateWindows() const override;
    std::string Fingerprint() const override;

    Result<void> StartPreview(const std::string &deviceId, const std::string &mode) override;
    Result<void> StopPreview(const std::string &deviceId) override;
    std::vector<uint8_t> PreviewFrame(const std::string &deviceId) override;
    std::vector<std::string> ActivePreviews() const override;
    DecodedFrame PreviewFramePixels(const std::string &deviceId) override;

    Result<void> StartScreenPreview(const std::string &monitorId) override;
    Result<void> StopScreenPreview(const std::string &monitorId) override;

private:
    struct PreviewTap;
    struct PreviewTable;   // the pimpl: mutex + one tap per device id
    // The drain thread body (one per running tap) — a static member so it
    // can touch the private nested type. Takes the table's mutex to publish.
    static void PreviewThread(PreviewTap &tap, std::mutex &publishMutex);
    // Screen tap: BitBlt the monitor into a DIB, downscale, JPEG, publish —
    // same tap table, same preview discipline.
    static void ScreenPreviewThread(PreviewTap &tap, std::mutex &publishMutex);

    std::unique_ptr<PreviewTable> previews_;
};

} // namespace bps::platform
