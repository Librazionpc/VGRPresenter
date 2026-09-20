#pragma once

// PAL video-capture subsystem: enumerates video capture devices (webcams,
// capture cards) and their REAL capabilities — per-mode frame sizes and the
// frame rates each mode actually runs at, plus the device-wide max frame
// rate the UI gates its pickers against (anything faster is offered greyed
// out / rejected). Mode *selection* and streaming belong to the future
// capture feature module; this interface is discovery-only, same scope as
// IAudio (enumerate + defaults/fingerprint, no I/O).

#include "core/common/Common.hpp"

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

class IVideo {
public:
    virtual ~IVideo() = default;

    // All active video-capture devices visible to the OS (best effort; may
    // be empty on hosts without a capture stack).
    virtual std::vector<VideoDeviceInfo> Enumerate() const = 0;

    // Stable summary of the current device set (ids) — the Kernel's platform
    // watcher compares successive values for capture hot-plug events.
    virtual std::string Fingerprint() const = 0;
};

} // namespace bps::platform
