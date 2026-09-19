#include "platform/linux/LinuxPower.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>

namespace bps::platform {

namespace {
std::string ReadFile(std::string_view path) {
    std::ifstream in{std::string(path)};
    std::string out, line;
    while (std::getline(in, line)) out += line;
    return out;
}
} // namespace

PowerInfo LinuxPower::Current() const {
    PowerInfo info;

    std::error_code ec;
    std::filesystem::directory_iterator it("/sys/class/power_supply", ec);
    if (ec) return info;

    for (const auto& entry : it) {
        std::string base = entry.path().string();
        std::string type = ReadFile(base + "/type");
        if (type.find("Battery") == std::string::npos) continue;

        info.hasBattery = true;
        std::string capacity = ReadFile(base + "/capacity");
        if (!capacity.empty()) info.batteryPercent = std::atoi(capacity.c_str());

        std::string status = ReadFile(base + "/status");
        if (status.find("Discharging") != std::string::npos) {
            info.onBattery = true;
            info.charging = false;
        } else if (status.find("Charging") != std::string::npos) {
            info.onBattery = false;
            info.charging = true;
        } else if (status.find("Full") != std::string::npos) {
            info.onBattery = false;
            info.charging = false;
        }
        break; // first battery is enough for this pass
    }
    return info;
}

} // namespace bps::platform
