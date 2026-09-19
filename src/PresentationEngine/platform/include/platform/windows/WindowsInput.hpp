#pragma once

// Windows PAL backend for the input subsystem: enumerates devices via the Raw
// Input API (GetRawInputDeviceList). Device discovery only; raw event capture
// is a future extension.

#include "../IInput.hpp"

namespace bps::platform {

class WindowsInput final : public IInput {
public:
    std::vector<InputDeviceInfo> Enumerate() const override;
    bool HasKeyboard() const override;
    bool HasPointer() const override;
};

} // namespace bps::platform
