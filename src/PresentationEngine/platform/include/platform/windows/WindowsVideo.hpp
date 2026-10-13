#pragma once

// Windows PAL backend for the video-capture subsystem (Media Foundation
// device + mode enumeration) plus a REAL Source Reader preview tap per
// previewed camera: Start spawns a drain thread, PreviewFrame returns the
// newest JPEG it published (shared-memory hand-off, lock-held copies only).

#include "platform/IVideo.hpp"

#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <vector>

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
    // Targeted walk (see IVideo::EnumerateFor): cameras the caller did not name
    // are never activated and never walked for modes — the costly half.
    std::vector<VideoDeviceInfo> EnumerateFor(
        const std::vector<std::string> &names) const override;
    std::vector<WindowInfo> EnumerateWindows() const override;
    std::string Fingerprint() const override;

    Result<void> StartPreview(const std::string &deviceId, const std::string &mode,
                              const std::string &deviceName = {}) override;
    Result<void> StopPreview(const std::string &deviceId) override;
    std::vector<uint8_t> PreviewFrame(const std::string &deviceId) override;
    std::string PreviewFailure(const std::string &deviceId) const override;
    std::vector<std::string> ActivePreviews() const override;
    DecodedFrame PreviewFramePixels(const std::string &deviceId) override;

    Result<void> StartScreenPreview(const std::string &monitorId) override;
    Result<void> StopScreenPreview(const std::string &monitorId) override;

private:
    // The one walk Enumerate()/EnumerateFor() share (`only` = the targeted set).
    std::vector<VideoDeviceInfo> EnumerateFiltered(const std::set<std::string> *only) const;
    struct PreviewTap;
    struct PreviewTable;   // the pimpl: mutex + one tap per device id
    // The camera drain thread body (one per running tap) — a static member so
    // it can touch the private nested type. It no longer PUBLISHES (the async
    // Source Reader callback does, straight into the tap's shared frame slot)
    // and no longer needs the table's mutex to do it.
    static void PreviewThread(PreviewTap &tap);
    // Screen/window tap: BitBlt a monitor (or PrintWindow a "win:<hwnd>"
    // window) into a DIB, downscale, JPEG, publish — same tap table, same
    // preview discipline, and CANCELLABLE like the camera tap: a window is
    // probed with a bounded SendMessageTimeout before any PrintWindow, and
    // every wait between grabs is stop-aware, so the worker always returns.
    static void ScreenPreviewThread(PreviewTap &tap);

    // Shared (not unique) so the table's mutex outlives any in-flight reaper:
    // ~WindowsVideo calls Clear(), which waits for every capture worker to exit
    // before the table is dropped (see PreviewTable::Clear()).
    std::shared_ptr<PreviewTable> previews_;
};

} // namespace bps::platform
