#pragma once

#include "core/common/Common.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace bps::platform {
// One capture mode: a frame size with the rates the device reports for it.
struct VideoModeInfo {
    uint32_t width = 0;
    uint32_t height = 0;
    std::vector<uint32_t> fpsRates; // discrete rates (fps, rounded) for this size
};

struct VideoDeviceInfo {
    std::string id;       // stable id (symbolic-link derived on Windows)
    std::string name;     // friendly name
    bool isCamera = true; // vidcap device (cameras + capture cards)
    std::vector<VideoModeInfo> modes;
    // Device-wide ceiling: the fastest rate ANY mode reports. 0 = unknown
    // (enumeration failed) — consumers must treat 0 as "no gate".
    uint32_t maxFps = 0;
};

// One capturable TOP-LEVEL WINDOW (screen-source option, OBS-style
// "Window Capture"): hwnd-derived stable key + the visible title.
struct WindowInfo {
    std::string id;    // "win:<hwnd>" — stable while the window lives
    std::string title; // the window's visible title
};

class IVideo {
public:
    virtual ~IVideo() = default;

    // All active video-capture devices visible to the OS (best effort; may
    // be empty on hosts without a capture stack).
    virtual std::vector<VideoDeviceInfo> Enumerate() const = 0;

    virtual std::vector<VideoDeviceInfo> EnumerateFor(
        const std::vector<std::string> &names) const
    {
        std::vector<VideoDeviceInfo> out;
        if (names.empty())
            return out;
        for (const VideoDeviceInfo &d : Enumerate()) {
            for (const std::string &want : names) {
                if (d.name == want) {
                    out.push_back(d);
                    break;
                }
            }
        }
        return out;
    }

    virtual std::vector<WindowInfo> EnumerateWindows() const { return {}; }

    // Stable summary of the current device set (ids) — the Kernel's platform
    // watcher compares successive values for capture hot-plug events.
    virtual std::string Fingerprint() const = 0;

    // ---- Preview taps ---------------------------------------------------
    // deviceId is the device's own id (VideoDeviceInfo::id — the symbolic
    // link string on Windows, NOT a positional index: cameras hot-plug).

    virtual Result<void> StartPreview(const std::string &deviceId, const std::string &mode,
                                      const std::string &deviceName = {})
    {
        (void)deviceId;
        (void)mode;
        (void)deviceName;
        return Error::Make(Err::Unsupported, "Video",
                           "video preview is not implemented on this platform");
    }
    // Stop capturing and release the tap. Idempotent; stopping an unknown
    // device is Ok (nothing to do).
    virtual Result<void> StopPreview(const std::string &deviceId)
    {
        (void)deviceId;
        return Error::Make(Err::Unsupported, "Video",
                           "video preview is not implemented on this platform");
    }
    virtual std::vector<uint8_t> PreviewFrame(const std::string &deviceId)
    {
        (void)deviceId;
        return {};
    }
    virtual std::string PreviewFailure(const std::string &deviceId) const
    {
        (void)deviceId;
        return {};
    }
    // Devices with a live preview tap (drives any future frame pump).
    virtual std::vector<std::string> ActivePreviews() const { return {}; }

    struct DecodedFrame {
        uint32_t width = 0;
        uint32_t height = 0;
        std::vector<uint8_t> rgba;   // tightly packed RGBA8, row 0 at the top
        uint64_t seq = 0;            // see PreviewFrameSeq
        bool empty() const { return width == 0 || height == 0 || rgba.empty(); }
    };
    virtual DecodedFrame PreviewFramePixels(const std::string &deviceId)
    {
        (void)deviceId;
        return {};
    }
    virtual uint64_t PreviewFrameSeq(const std::string &deviceId)
    {
        (void)deviceId;
        return 0;
    }
    // The newest frame WITHOUT a copy. A tap already holds its pixels as one immutable buffer, so a
    // consumer that only READS them (a GPU surface drawing the camera on an output) shares that buffer
    // instead of taking a private 8 MB copy per call — PreviewFramePixels copies, and three scenes plus
    // a feed asking per frame was the bulk of a camera's per-frame cost. The base implementation wraps
    // PreviewFramePixels (one copy, correct everywhere); a backend that keeps the buffer shared
    // overrides it.
    struct SharedFrame {
        uint32_t width = 0;
        uint32_t height = 0;
        uint64_t seq = 0;
        std::shared_ptr<const std::vector<uint8_t>> rgba;   // tightly packed RGBA8, row 0 at the top
        bool empty() const { return width == 0 || height == 0 || !rgba || rgba->empty(); }
    };
    virtual SharedFrame PreviewFrameShared(const std::string &deviceId)
    {
        DecodedFrame frame = PreviewFramePixels(deviceId);
        SharedFrame shared;
        shared.width = frame.width;
        shared.height = frame.height;
        shared.seq = frame.seq;
        if (!frame.empty())
            shared.rgba = std::make_shared<const std::vector<uint8_t>>(std::move(frame.rgba));
        return shared;
    }
    virtual void SetPreviewLive(const std::string &deviceId, bool live)
    {
        (void)deviceId;
        (void)live;
    }

    // ---- Screen-capture taps (monitor id = IMonitor's "\\\.\DISPLAY1") ----
    virtual Result<void> StartScreenPreview(const std::string &monitorId)
    {
        (void)monitorId;
        return Error::Make(Err::Unsupported, "Video",
                           "screen preview is not implemented on this platform");
    }
    virtual Result<void> StopScreenPreview(const std::string &monitorId)
    {
        (void)monitorId;
        return Error::Make(Err::Unsupported, "Video",
                           "screen preview is not implemented on this platform");
    }
};
} // namespace bps::platform