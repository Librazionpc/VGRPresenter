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

    // Begin previewing the named capture device. mode is the UI's capture
    // pick ("1920x1080p60" — width×height are honored; fps only breaks
    // ties between native types of the same size); an empty mode keeps the
    // device's default. Real MF Source Reader on Windows: native type at
    // the requested frame size, ~15 fps drain, frames converted to JPEG.
    // Idempotent: a second Start on a running tap is a no-op. NotFound
    // when the device doesn't exist, Unsupported on platforms without
    // capture support.
    virtual Result<void> StartPreview(const std::string &deviceId, const std::string &mode)
    {
        (void)deviceId;
        (void)mode;
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
    // The newest preview frame as JPEG bytes (already downscaled for a
    // dialog pane, ~480px wide). Empty vector = no tap or no frame yet (the
    // UI keeps showing the placeholder glyph).
    virtual std::vector<uint8_t> PreviewFrame(const std::string &deviceId)
    {
        (void)deviceId;
        return {};
    }
    // Devices with a live preview tap (drives any future frame pump).
    virtual std::vector<std::string> ActivePreviews() const { return {}; }

    // ---- Screen-capture taps (monitor id = IMonitor's "\\\.\DISPLAY1") ----
    // Same pane contract as the camera taps: Start → ~15 fps JPEG via
    // PreviewFrame, Stop, idempotent. The Windows backend grabs the monitor
    // named by id via GDI BitBlt (no MF involvement); Unsupported elsewhere.
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
};

} // namespace bps::platform
