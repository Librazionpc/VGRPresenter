#include "platform/windows/WindowsInput.hpp"

#include "platform/windows/WinUtil.hpp"

#include <windows.h>

#include <algorithm>

namespace bps::platform {

namespace {

InputDeviceKind KindOf(const std::string& name, const RID_DEVICE_INFO& info) {
    std::string lower = name;
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return static_cast<char>(::tolower(c)); });
    if (info.dwType == RIM_TYPEKEYBOARD) return InputDeviceKind::Keyboard;
    if (info.dwType == RIM_TYPEMOUSE) return InputDeviceKind::Mouse;
    // HID devices: classify by name keywords; digitizer usage page is
    // available via GetRawInputDeviceInfoW(RIDI_PREPARSEDDATA) but names are
    // sufficient for discovery.
    if (lower.find("touch") != std::string::npos) return InputDeviceKind::Touch;
    if (lower.find("pen") != std::string::npos || lower.find("tablet") != std::string::npos)
        return InputDeviceKind::Pen;
    if (lower.find("gamepad") != std::string::npos ||
        lower.find("controller") != std::string::npos) return InputDeviceKind::Gamepad;
    if (lower.find("keyboard") != std::string::npos) return InputDeviceKind::Keyboard;
    if (lower.find("mouse") != std::string::npos) return InputDeviceKind::Mouse;
    return InputDeviceKind::Other;
}

} // namespace

std::vector<InputDeviceInfo> WindowsInput::Enumerate() const {
    std::vector<InputDeviceInfo> devices;
    UINT count = 0;
    if (::GetRawInputDeviceList(nullptr, &count, sizeof(RAWINPUTDEVICELIST)) != 0 || count == 0)
        return devices;
    std::vector<RAWINPUTDEVICELIST> list(count);
    UINT got = ::GetRawInputDeviceList(list.data(), &count, sizeof(RAWINPUTDEVICELIST));
    if (got == static_cast<UINT>(-1)) return devices;
    for (UINT i = 0; i < got; ++i) {
        // Device path (contains the VID/PID or HID path — a stable id).
        UINT len = 0;
        (void)::GetRawInputDeviceInfoW(list[i].hDevice, RIDI_DEVICENAME, nullptr, &len);
        std::wstring path;
        if (len > 0) {
            path.resize(len);
            (void)::GetRawInputDeviceInfoW(list[i].hDevice, RIDI_DEVICENAME, path.data(), &len);
            if (!path.empty() && path.back() == L'\0') path.pop_back();
        }
        // Device type.
        RID_DEVICE_INFO info{};
        info.cbSize = sizeof(info);
        UINT infoLen = sizeof(info);
        (void)::GetRawInputDeviceInfoW(list[i].hDevice, RIDI_DEVICEINFO, &info, &infoLen);

        InputDeviceInfo dev;
        dev.id = win::Utf8(path);
        if (dev.id.empty()) dev.id = "rawinput" + std::to_string(i);
        dev.name = dev.id;
        if (dev.id.rfind("\\\\?\\", 0) == 0) {
            // Strip the prefix and trailing interface GUID for readability.
            std::string n = dev.id.substr(4);
            auto pos = n.find('{');
            if (pos != std::string::npos) n.resize(pos);
            dev.name = n;
        }
        dev.kind = KindOf(dev.name, info);
        devices.push_back(std::move(dev));
    }
    return devices;
}

bool WindowsInput::HasKeyboard() const {
    for (const auto& d : Enumerate())
        if (d.kind == InputDeviceKind::Keyboard) return true;
    return false;
}

bool WindowsInput::HasPointer() const {
    for (const auto& d : Enumerate())
        if (d.kind == InputDeviceKind::Mouse || d.kind == InputDeviceKind::Touch ||
            d.kind == InputDeviceKind::Pen) return true;
    return false;
}

} // namespace bps::platform
