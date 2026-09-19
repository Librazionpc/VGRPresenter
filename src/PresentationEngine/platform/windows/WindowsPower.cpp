#include "platform/windows/WindowsPower.hpp"

#include <windows.h>

namespace bps::platform {

PowerInfo WindowsPower::Current() const {
    PowerInfo info;
    SYSTEM_POWER_STATUS st{};
    if (!GetSystemPowerStatus(&st)) return info;
    // BatteryFlag: 128 = no system battery; 255 = unknown.
    if (st.BatteryFlag != 128 && st.BatteryFlag != 255) {
        info.hasBattery = true;
        info.batteryPercent = (st.BatteryLifePercent <= 100) ? st.BatteryLifePercent : -1;
        info.onBattery = (st.ACLineStatus == 0);          // 0 = offline
        info.charging = (st.BatteryFlag == 8);            // 8 = charging
    }
    return info;
}

} // namespace bps::platform
