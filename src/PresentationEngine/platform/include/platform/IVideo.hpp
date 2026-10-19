#pragma once

// PAL video-capture subsystem: enumerates video capture devices (webcams,
// capture cards) and their REAL capabilities — per-mode frame sizes and the
// frame rates each mode actually runs at, plus the device-wide max frame
// rate the UI gates its pickers against (anything faster is offered greyed
// out / rejected).
//
// Preview taps (added 2026-09-26): a per-device SOURCE READER tap delivers
// small JPEG frames at ~15 fps for the dialogs' preview panes — real video
// I/O, deliberately scoped: no mode negotiation beyond the reader's native
// type, no rendering pipeline, frames die at the JPEG. Backends without
// capture support keep the discovery surface and return Unsupported.

#include "core/common/Common.hpp"

#include <cstdint>
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

    // TARGETED device lookup — the capture equivalent of IAudio::EnumerateFor:
    // only the friendly names the caller needs (the app's saved video roster),
    // and no capability walk for the rest (activating a camera and walking its
    // media types is the expensive half of an enumeration). Empty `names` means
    // "none wanted". The default filters the bulk roster; backends that can walk
    // cheaply override it and never activate a device they weren't asked for.
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

    // Open, visible, capturable top-level windows (best effort; empty on
    // platforms without window capture). Ids stay valid while the window
    // lives — the UI re-enumerates on dialog open, so stale ids self-heal.
    virtual std::vector<WindowInfo> EnumerateWindows() const { return {}; }

    // Stable summary of the current device set (ids) — the Kernel's platform
    // watcher compares successive values for capture hot-plug events.
    virtual std::string Fingerprint() const = 0;

    // ---- Preview taps ---------------------------------------------------
    // deviceId is the device's own id (VideoDeviceInfo::id — the symbolic
    // link string on Windows, NOT a positional index: cameras hot-plug).

    // Begin previewing the named capture device. deviceName is a cached
    // friendly name used by platform workers for device recovery. mode is the UI's capture
    // pick ("1920x1080p60" — width×height are honored; fps only breaks
    // ties between native types of the same size); an empty mode keeps the
    // device's default. Real MF Source Reader on Windows: native type at
    // the requested frame size; a PANE tap drains at ~15 fps and publishes
    // JPEG, a LIVE tap (SetPreviewLive) publishes every sample as pixels.
    // Idempotent: a second Start on a running tap is a no-op. The result
    // reports whether the preview request was accepted; device open errors
    // are reported asynchronously by the platform preview path.
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
    // The newest preview frame as JPEG bytes. Empty vector = no tap or no frame
    // yet (the UI keeps showing the placeholder glyph). PANE taps (the default)
    // publish one encoded frame every ~66 ms: a thumbnail costs the CPU once and
    // the QML side decodes it. A LIVE tap publishes a fresh JPEG at only
    // kLiveJpegMs (5 Hz, for the small on-screen tiles) — its real output is
    // PreviewFramePixels, which is what a program feed must read.
    virtual std::vector<uint8_t> PreviewFrame(const std::string &deviceId)
    {
        (void)deviceId;
        return {};
    }
    // WHY a started preview will never produce a frame, if that is the case.
    //
    // StartPreview only ACCEPTS the request: the device is opened on the tap's
    // own worker (opening a camera can take seconds, so it must not run on the UI
    // thread), and the open can fail — unplugged, busy, driver error, a stream
    // with no decodable type. Until now such a tap kept `running` set and said
    // nothing: PreviewFrame stayed empty forever, the UI could not tell "failing"
    // from "warming up" and showed its connecting ring for as long as the card
    // was on screen, and a later StartPreview was answered "already running" so
    // the device was never retried. Empty = the tap is fine (or unknown); the
    // returned text is the platform's own reason, for the log and the card.
    virtual std::string PreviewFailure(const std::string &deviceId) const
    {
        (void)deviceId;
        return {};
    }
    // Devices with a live preview tap (drives any future frame pump).
    virtual std::vector<std::string> ActivePreviews() const { return {}; }

    // The SAME tap's newest frame, decoded — for an engine-side compositor
    // consumer (SceneBuilder painting a taken camera/screen input into a
    // real Frame) rather than a QML thumbnail pane, which wants the raw
    // PreviewFrame() JPEG bytes instead (its own Qt-side decode). Deliberately
    // a plain PAL-local struct, not modules/rendering's RgbaImage or
    // modules/media's Picture — platform/ stays a leaf layer with no
    // dependency upward into modules/. Empty = no tap or no frame yet.
    //
    // On a LIVE tap this is the tap's PRIMARY output: the capture thread keeps
    // the newest frame as unpacked pixels (no JPEG round trip, no 15 fps gate),
    // so a program feed reads the device's own rate at full resolution. `seq`
    // advances once per stored frame — compare it instead of copying a 1080p
    // picture to discover that nothing arrived.
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
    // How many frames this tap has stored (camera: every sample; screen: every
    // grab). 0 = no tap / no frame yet. A puller that only wants to copy when
    // something NEW arrived reads this first — the cheap question — and calls
    // PreviewFramePixels only when it moved.
    virtual uint64_t PreviewFrameSeq(const std::string &deviceId)
    {
        (void)deviceId;
        return 0;
    }
    // Make a RUNNING tap a program feed (live) or a thumbnail pane (not live).
    //
    // This is the difference between the two jobs a tap really has, and it is
    // set per tap rather than per API because a camera can be held by a board
    // row and a dialog at once:
    //   * pane  — one JPEG every ~66 ms (15 fps). Cheap, and enough for a
    //             thumbnail; this was the ONLY mode, which is why a live bus
    //             fed to the outputs moved at 15 fps on both HDMI and NDI.
    //   * live  — publish EVERY sample as unpacked pixels at the device's own
    //             rate and resolution (PreviewFramePixels), plus a 5 Hz JPEG so
    //             the small tiles that read PreviewFrame keep working.
    // Idempotent and cheap; a no-op for an unknown tap or a platform without
    // taps.
    virtual void SetPreviewLive(const std::string &deviceId, bool live)
    {
        (void)deviceId;
        (void)live;
    }

    // ---- Screen-capture taps (monitor id = IMonitor's "\\\.\DISPLAY1") ----
    // Same contract as the camera taps: Start → JPEG for a pane / unpacked
    // pixels for a live tap (see SetPreviewLive), Stop, idempotent. The Windows
    // backend grabs the monitor named by id via GDI BitBlt (no MF involvement);
    // Unsupported elsewhere.
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
    // Monitor ids flow through the same PreviewFrame as camera ids — the
    // "\\\.\DISPLAY..." prefix can never collide with a camera symlink.
    // The SAME taps also serve WINDOW capture: StartScreenPreview with a
    // "win:<hwnd>" id BitBlts that window instead of a monitor; the
    // per-grab IsWindow check ends the tap cleanly when the window closes.
    //
    // Every tap — camera, monitor, window — is CANCELLABLE, because Stop/pre-empt
    // must be able to shut a captured source down even while it misbehaves (a
    // camera that stops answering, a window whose UI thread is wedged). A stop
    // is therefore honored inside one worker iteration, and no capture worker
    // can be left parked in an unbounded driver call.
};

} // namespace bps::platform
