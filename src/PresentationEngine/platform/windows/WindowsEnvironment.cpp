#include "platform/windows/WindowsEnvironment.hpp"
#include "platform/windows/WinUtil.hpp"
#include "platform/OsTag.hpp"

#include <windows.h>

#include <cstdlib>
#include <thread>

namespace bps::platform {

namespace {
std::string RegistryString(HKEY root, const char* subKey, const char* value) {
    char buf[1024] = {0};
    DWORD size = sizeof(buf);
    DWORD type = 0;
    if (RegGetValueA(root, subKey, value, RRF_RT_REG_SZ, &type, buf, &size) != ERROR_SUCCESS)
        return {};
    return buf;
}

// Reports the running process architecture (canonical name) — the most useful
// answer on Windows, where a 32-bit build can run on 64-bit hardware (WOW64)
// and an ARM64 build can run under x64 emulation. Compile-time: deterministic
// and identical to BuildInfo (kCompileArch).
std::string ArchString() { return CompileArch(); }

std::string OsVersion() {
    // RtlGetVersion (Vista+) is not deprecated like GetVersionExA.
    using RtlGetVersionFn = LONG(WINAPI*)(PRTL_OSVERSIONINFOW);
    static auto fn = reinterpret_cast<RtlGetVersionFn>(
        GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion"));
    if (!fn) return {};
    OSVERSIONINFOW ovi{};
    ovi.dwOSVersionInfoSize = sizeof(ovi);
    if (fn(&ovi) != 0) return {};
    return std::to_string(ovi.dwMajorVersion) + "." + std::to_string(ovi.dwMinorVersion) + "." +
           std::to_string(ovi.dwBuildNumber);
}

std::string Timezone() {
    DYNAMIC_TIME_ZONE_INFORMATION tz{};
    if (GetDynamicTimeZoneInformation(&tz) == TIME_ZONE_ID_INVALID) return {};
    return win::Utf8(tz.TimeZoneKeyName);
}

std::string Locale() {
    wchar_t buf[LOCALE_NAME_MAX_LENGTH] = {0};
    if (GetUserDefaultLocaleName(buf, LOCALE_NAME_MAX_LENGTH) == 0) return {};
    return win::Utf8(buf);
}
} // namespace

EnvironmentInfo WindowsEnvironment::Current() const {
    EnvironmentInfo info;
    info.osName = "Windows";
    info.osVersion = OsVersion();
    info.arch = ArchString();
    info.buildNumber = info.osVersion;

    info.cpuModel = RegistryString(
        HKEY_LOCAL_MACHINE,
        "HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0", "ProcessorNameString");
    info.gpuName = RegistryString(
        HKEY_LOCAL_MACHINE,
        "SYSTEM\\CurrentControlSet\\Control\\Class\\{4d36e968-e325-11ce-bfc1-08002be10318}\\0000",
        "DriverDesc");

    MEMORYSTATUSEX ms{};
    ms.dwLength = sizeof(ms);
    if (GlobalMemoryStatusEx(&ms)) info.totalRamBytes = ms.ullTotalPhys;
    SYSTEM_INFO si{};
    GetNativeSystemInfo(&si);
    info.coreCount = si.dwNumberOfProcessors > 0 ? si.dwNumberOfProcessors : 1;

    char buf[256] = {0};
    DWORD len = sizeof(buf);
    if (GetComputerNameA(buf, &len) != 0) info.hostname = buf;
    len = sizeof(buf);
    if (GetUserNameA(buf, &len) != 0) info.username = buf;

    info.locale = Locale();
    info.timezone = Timezone();
    return info;
}

} // namespace bps::platform
