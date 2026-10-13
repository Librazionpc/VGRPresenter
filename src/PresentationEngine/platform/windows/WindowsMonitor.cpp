#include "platform/windows/WindowsMonitor.hpp"
#include "platform/windows/WinUtil.hpp"

#include <windows.h>

#include <vector>

namespace bps::platform {

namespace {

// Rotation -> degrees for DISPLAYCONFIG_TARGET_ROTATION.
int RotationDegrees(UINT32 rotation) {
    switch (rotation) {
        case DISPLAYCONFIG_ROTATION_ROTATE90:  return 90;
        case DISPLAYCONFIG_ROTATION_ROTATE180: return 180;
        case DISPLAYCONFIG_ROTATION_ROTATE270: return 270;
        default:                               return 0;
    }
}

int OrientationOf(const wchar_t* deviceName) {
    // QueryDisplayConfig is Vista+; best-effort — on failure report 0°.
    UINT32 pathCount = 0, modeCount = 0;
    if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &pathCount, &modeCount) != ERROR_SUCCESS)
        return 0;
    std::vector<DISPLAYCONFIG_PATH_INFO> paths(pathCount);
    std::vector<DISPLAYCONFIG_MODE_INFO> modes(modeCount);
    LONG rc = QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &pathCount, paths.data(), &modeCount,
                                 modes.data(), nullptr);
    if (rc != ERROR_SUCCESS) return 0;
    for (UINT32 i = 0; i < pathCount; ++i) {
        DISPLAYCONFIG_SOURCE_DEVICE_NAME src{};
        src.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
        src.header.size = sizeof(src);
        src.header.adapterId = paths[i].sourceInfo.adapterId;
        src.header.id = paths[i].sourceInfo.id;
        if (DisplayConfigGetDeviceInfo(&src.header) == ERROR_SUCCESS &&
            wcscmp(src.viewGdiDeviceName, deviceName) == 0)
            return RotationDegrees(paths[i].targetInfo.rotation);
    }
    return 0;
}

BOOL CALLBACK MonitorEnumProc(HMONITOR hMonitor, HDC, LPRECT, LPARAM lParam) {
    auto* out = reinterpret_cast<std::vector<MonitorInfo>*>(lParam);
    MONITORINFOEXW mi{};
    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoW(hMonitor, &mi)) return TRUE;
    MonitorInfo m;
    m.id = win::Utf8(mi.szDevice);                       // e.g. "\\\\.\\DISPLAY1"
    m.name = m.id;
    m.x = mi.rcMonitor.left;
    m.y = mi.rcMonitor.top;
    // rcMonitor is the monitor's rect in the session's VIRTUAL coordinates — the
    // ones DWM's display scaling produces — so on a 125%-scaled panel it reports
    // the LOGICAL size (a 1920x1200 laptop panel reads as 1536x960). Every
    // resolution this app shows for a display, and every size it derives from the
    // panel, must be the panel's REAL pixels, so the mode's dmPels* below wins.
    m.widthPx = mi.rcMonitor.right - mi.rcMonitor.left;
    m.heightPx = mi.rcMonitor.bottom - mi.rcMonitor.top;
    m.primary = (mi.dwFlags & MONITORINFOF_PRIMARY) != 0;
    m.connected = true;
    m.dpi = 96;
    m.orientation = OrientationOf(mi.szDevice);
    DEVMODEW dm{};
    dm.dmSize = sizeof(dm);
    if (EnumDisplaySettingsExW(mi.szDevice, ENUM_CURRENT_SETTINGS, &dm, 0)) {
        m.refreshRateHz = static_cast<int>(dm.dmDisplayFrequency);
        // The current mode's real pixel size (the panel's own resolution).
        if (dm.dmPelsWidth > 0 && dm.dmPelsHeight > 0) {
            // The scale factor is the ratio between the two coordinate spaces;
            // 96 dpi is the unscaled reference, so a panel running at 125% comes
            // back as 120 rather than a hard-coded 96 ("the display I have
            // connected is bigger than the laptop's and the list says otherwise").
            if (m.widthPx > 0 && static_cast<LONG>(dm.dmPelsWidth) != m.widthPx)
                m.dpi = static_cast<int>((96.0 * dm.dmPelsWidth) / m.widthPx + 0.5);
            m.widthPx = static_cast<int>(dm.dmPelsWidth);
            m.heightPx = static_cast<int>(dm.dmPelsHeight);
        }
    }
    out->push_back(std::move(m));
    return TRUE;
}
} // namespace

std::vector<MonitorInfo> WindowsMonitor::Enumerate() const {
    std::vector<MonitorInfo> out;
    EnumDisplayMonitors(nullptr, nullptr, MonitorEnumProc, reinterpret_cast<LPARAM>(&out));
    return out;
}

Result<MonitorInfo> WindowsMonitor::Primary() const {
    auto monitors = Enumerate();
    for (const auto& m : monitors)
        if (m.primary) return m;
    if (!monitors.empty()) return monitors.front();
    return Error::Make(Err::NotFound, "Monitor", "no monitors connected");
}

std::string WindowsMonitor::DefaultMonitorId() const {
    auto monitors = Enumerate();
    for (const auto& m : monitors)
        if (m.primary) return m.id;
    return monitors.empty() ? "" : monitors.front().id;
}

} // namespace bps::platform
