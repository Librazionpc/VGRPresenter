#pragma once

// PAL audio device subsystem (Phase 2): enumerates audio *devices* and their
// capabilities. Not media playback and not audio mixing — feature modules
// (media module, streaming) consume device info through this interface.

#include "core/common/Common.hpp"

#include <string>
#include <vector>

namespace bps::platform {

struct AudioDeviceInfo {
    std::string id;            // stable id (e.g. ALSA "hw:0,0", "default")
    std::string name;          // friendly name
    bool isInput = false;      // false = output device
    bool isDefault = false;
    uint32_t sampleRateHz = 0; // 0 = unknown
    uint32_t channels = 0;     // 0 = unknown
};

class IAudio {
public:
    virtual ~IAudio() = default;

    // All devices visible to the OS (best effort; may be empty on systems
    // without a supported audio stack).
    virtual std::vector<AudioDeviceInfo> Enumerate() const = 0;

    virtual Result<AudioDeviceInfo> DefaultOutput() const = 0;
    virtual Result<AudioDeviceInfo> DefaultInput() const = 0;

    // Stable summary string of the current device set (ids + direction). The
    // Kernel's platform watcher compares successive values to emit
    // DeviceConnected / DeviceRemoved OS events (DoD §13 hot plug).
    virtual std::string Fingerprint() const = 0;
};

} // namespace bps::platform
