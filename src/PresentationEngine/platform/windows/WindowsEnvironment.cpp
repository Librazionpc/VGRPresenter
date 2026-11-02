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

std::string ArchString() { return CompileArch(); }

struct RawOsVersion {
    DWORD major = 0;
    DWORD minor = 0;
    DWORD build = 0;
    bool ok = false;
};

RawOsVersion QueryOsVersion() {
    using RtlGetVersionFn = LONG(WINAPI*)(PRTL_OSVERSIONINFOW);
    static auto fn = win::ProcAddress<RtlGetVersionFn>(
        GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion");
    RawOsVersion out;
    if (!fn) return out;
    OSVERSIONINFOW ovi{};
    ovi.dwOSVersionInfoSize = sizeof(ovi);
    if (fn(&ovi) != 0) return out;
    out.major = ovi.dwMajorVersion;
    out.minor = ovi.dwMinorVersion;
    out.build = ovi.dwBuildNumber;
    out.ok = true;
    return out;
}

std::string OsVersionString(const RawOsVersion& v) {
    if (!v.ok) return {};
    return std::to_string(v.major) + "." + std::to_string(v.minor) + "." + std::to_string(v.build);
}

std::string FriendlyOsName(const RawOsVersion& v) {
    if (!v.ok) return "Windows";
    if (v.major == 10 && v.minor == 0)
        return v.build >= 22000 ? "Windows 11" : "Windows 10";
    return "Windows";   // pre-Win10 or a future major bump — don't fabricate a name
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
    const RawOsVersion raw = QueryOsVersion();
    info.osName = FriendlyOsName(raw);
    info.osVersion = OsVersionString(raw);
    info.arch = ArchString();
    info.buildNumber = info.osVersion;

    info.cpuModel = RegistryString(
        HKEY_LOCAL_MACHINE,
        "HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0", "ProcessorNameString");
    info.gpuName = RegistryString(
        HKEY_LOCAL_MACHINE,
        "SYSTEM\\CurrentControlSet\\Control\\Class\\{4d36e968-e325-11ce-bfc1-08002be10318}\\0000",
        "DriverDesc");
    // DXGI names the adapter the machine actually renders with (the registry's first entry can be an integrated one).
    if (const auto& gpu = win::PrimaryGpu(); !gpu.name.empty()) info.gpuName = gpu.name;

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