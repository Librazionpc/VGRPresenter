#include "platform/linux/LinuxInput.hpp"

#include <cstdio>
#include <sstream>

namespace bps::platform {

namespace {

// Parses one record of /proc/bus/input/devices:
//   I: Bus=...  N: Name="..."  S: Sysfs=.../input/input5  H: Handlers=... kbd event3 ...
InputDeviceKind KindFromHandlers(const std::string& handlers) {
    if (handlers.find("kbd") != std::string::npos) return InputDeviceKind::Keyboard;
    if (handlers.find("touchscreen") != std::string::npos ||
        handlers.find("touchpad") != std::string::npos) return InputDeviceKind::Touch;
    if (handlers.find("tablet") != std::string::npos ||
        handlers.find("pen") != std::string::npos) return InputDeviceKind::Pen;
    if (handlers.find("joystick") != std::string::npos ||
        handlers.find("gamepad") != std::string::npos) return InputDeviceKind::Gamepad;
    if (handlers.find("mouse") != std::string::npos) return InputDeviceKind::Mouse;
    return InputDeviceKind::Other;
}

std::string ReadProc(const char* path) {
    FILE* f = ::fopen(path, "r");
    if (!f) return {};
    std::string out;
    char buf[4096];
    size_t n = 0;
    while ((n = ::fread(buf, 1, sizeof buf, f)) > 0) out.append(buf, n);
    ::fclose(f);
    return out;
}

std::string BetweenQuotes(const std::string& line) {
    size_t a = line.find('"');
    if (a == std::string::npos) return {};
    size_t b = line.find('"', a + 1);
    if (b == std::string::npos) return {};
    return line.substr(a + 1, b - a - 1);
}

} // namespace

std::vector<InputDeviceInfo> LinuxInput::Enumerate() const {
    std::vector<InputDeviceInfo> devices;
    std::string data = ReadProc("/proc/bus/input/devices");
    std::istringstream ss(data);
    std::string line;
    std::string name, handlers, sysfs;
    auto flush = [&]() {
        if (name.empty() && handlers.empty()) return;
        InputDeviceInfo dev;
        dev.name = name;
        dev.kind = KindFromHandlers(handlers);
        // Stable id from Sysfs (.../input/input5).
        size_t pos = sysfs.find("/input/input");
        if (pos != std::string::npos) dev.id = "input" + sysfs.substr(pos + 12);
        if (dev.id.empty()) dev.id = dev.name;
        // Device node from the "eventN" handler.
        std::istringstream hs(handlers);
        std::string tok;
        while (hs >> tok)
            if (tok.rfind("event", 0) == 0) dev.path = "/dev/input/" + tok;
        devices.push_back(std::move(dev));
        name.clear(); handlers.clear(); sysfs.clear();
    };
    while (std::getline(ss, line)) {
        if (line.empty()) { flush(); continue; }
        if (line.rfind("N: Name=", 0) == 0) name = BetweenQuotes(line);
        else if (line.rfind("S: Sysfs=", 0) == 0) sysfs = line.substr(9);
        else if (line.rfind("H: Handlers=", 0) == 0) handlers = line.substr(12);
    }
    flush();
    return devices;
}

bool LinuxInput::HasKeyboard() const {
    for (const auto& d : Enumerate())
        if (d.kind == InputDeviceKind::Keyboard) return true;
    return false;
}

bool LinuxInput::HasPointer() const {
    for (const auto& d : Enumerate())
        if (d.kind == InputDeviceKind::Mouse || d.kind == InputDeviceKind::Touch ||
            d.kind == InputDeviceKind::Pen) return true;
    return false;
}

} // namespace bps::platform
