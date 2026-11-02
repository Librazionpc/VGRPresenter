#pragma once

#include "core/common/Common.hpp"

#include <string>
#include <vector>

namespace bps::platform {
enum class InputDeviceKind : int { Keyboard, Mouse, Touch, Pen, Gamepad, Other };

inline const char* ToString(InputDeviceKind k) {
    switch (k) {
        case InputDeviceKind::Keyboard: return "Keyboard";
        case InputDeviceKind::Mouse:    return "Mouse";
        case InputDeviceKind::Touch:    return "Touch";
        case InputDeviceKind::Pen:      return "Pen";
        case InputDeviceKind::Gamepad:  return "Gamepad";
        case InputDeviceKind::Other:    return "Other";
    }
    return "Other";
}

struct InputDeviceInfo {
    std::string id;    // stable id (e.g. "input5", PnP id on Windows)
    std::string name;  // friendly name
    InputDeviceKind kind = InputDeviceKind::Other;
    std::string path;  // device node / interface where known (e.g. /dev/input/event3)
};

class IInput {
public:
    virtual ~IInput() = default;

    // All input devices reported by the OS (best effort; empty on systems
    // without a supported input stack).
    virtual std::vector<InputDeviceInfo> Enumerate() const = 0;

    virtual bool HasKeyboard() const = 0;
    virtual bool HasPointer() const = 0;   // mouse, touch or pen
};
} // namespace bps::platform