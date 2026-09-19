#pragma once

// Linux PAL backend for the input subsystem: parses /proc/bus/input/devices
// (evdev device list). Device discovery only; raw event capture is a future
// extension (PAL.md §11).

#include "../IInput.hpp"

namespace bps::platform {

class LinuxInput final : public IInput {
public:
    std::vector<InputDeviceInfo> Enumerate() const override;
    bool HasKeyboard() const override;
    bool HasPointer() const override;
};

} // namespace bps::platform
