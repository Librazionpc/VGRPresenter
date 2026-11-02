#pragma once

#include "../IInput.hpp"

namespace bps::platform {
class WindowsInput final : public IInput {
public:
    std::vector<InputDeviceInfo> Enumerate() const override;
    bool HasKeyboard() const override;
    bool HasPointer() const override;
};
} // namespace bps::platform