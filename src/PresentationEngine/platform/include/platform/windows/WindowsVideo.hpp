#pragma once

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
    uint64_t PreviewFrameSeq(const std::string &deviceId) override;
    void SetPreviewLive(const std::string &deviceId, bool live) override;

    Result<void> StartScreenPreview(const std::string &monitorId) override;
    Result<void> StopScreenPreview(const std::string &monitorId) override;

private:
    // The one walk Enumerate()/EnumerateFor() share (`only` = the targeted set).
    std::vector<VideoDeviceInfo> EnumerateFiltered(const std::set<std::string> *only) const;
    struct PreviewTap;
    struct PreviewTable;   // the pimpl: mutex + one tap per device id
    static void PreviewThread(PreviewTap &tap);
    static void ScreenPreviewThread(PreviewTap &tap);

    std::shared_ptr<PreviewTable> previews_;
};
} // namespace bps::platform