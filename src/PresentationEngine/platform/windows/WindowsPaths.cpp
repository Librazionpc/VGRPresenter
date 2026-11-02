#include "platform/windows/WindowsPaths.hpp"
#include "platform/windows/WinUtil.hpp"

#include <windows.h>
#include <shlobj.h>

namespace bps::platform {
namespace {
// Known folder -> wide path; empty on failure.
std::wstring KnownFolder(REFKNOWNFOLDERID id) {
    PWSTR path = nullptr;
    if (SHGetKnownFolderPath(id, 0, nullptr, &path) != S_OK || !path) return {};
    std::wstring out(path);
    CoTaskMemFree(path);
    return out;
}

std::string ProfileRoot() {
    wchar_t buf[MAX_PATH * 2] = {0};
    DWORD n = GetEnvironmentVariableW(L"VGR_PROFILE_DIR", buf, static_cast<DWORD>(sizeof(buf) / sizeof(buf[0])));
    if (n == 0 || n >= sizeof(buf) / sizeof(buf[0])) return {};
    std::string p = win::Utf8(std::wstring(buf, n));
    for (char& c : p) if (c == '/') c = '\\';
    while (!p.empty() && p.back() == '\\') p.pop_back();
    return p;
}

std::string Known(REFKNOWNFOLDERID id, const std::string& fallbackSuffix = "") {
    std::wstring w = KnownFolder(id);
    if (w.empty()) return {};
    std::string p = win::Utf8(w);
    if (!fallbackSuffix.empty() && p.back() != '\\') p += "\\";
    return p + fallbackSuffix;
}
} // namespace

std::string WindowsPaths::ExecutableDir() const {
    wchar_t buf[MAX_PATH] = {0};
    DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
    if (n == 0) return ".";
    std::wstring path(buf);
    size_t slash = path.find_last_of(L"\\/");
    if (slash != std::wstring::npos) path = path.substr(0, slash);
    return win::Utf8(path);
}

std::string WindowsPaths::CurrentWorkingDir() const {
    wchar_t buf[MAX_PATH] = {0};
    if (GetCurrentDirectoryW(MAX_PATH, buf) == 0) return ".";
    return win::Utf8(buf);
}

std::string WindowsPaths::ConfigDir() const {
    if (const std::string root = ProfileRoot(); !root.empty()) return root + "\\roaming";
    std::string appdata = Known(FOLDERID_RoamingAppData);
    if (appdata.empty()) return {};
    return appdata + "\\bps";
}

std::string WindowsPaths::CacheDir() const {
    if (const std::string root = ProfileRoot(); !root.empty()) return root + "\\local\\cache";
    return Known(FOLDERID_LocalAppData, "bps\\cache");
}
std::string WindowsPaths::PluginDir() const {
    if (const std::string root = ProfileRoot(); !root.empty()) return root + "\\local\\plugins";
    return Known(FOLDERID_LocalAppData, "bps\\plugins");
}
std::string WindowsPaths::AssetDir() const {
    if (const std::string root = ProfileRoot(); !root.empty()) return root + "\\local\\assets";
    return Known(FOLDERID_LocalAppData, "bps\\assets");
}
std::string WindowsPaths::LogDir() const {
    if (const std::string root = ProfileRoot(); !root.empty()) return root + "\\local\\logs";
    return Known(FOLDERID_LocalAppData, "bps\\logs");
}

std::string WindowsPaths::TempDir() const {
    wchar_t buf[MAX_PATH] = {0};
    if (GetTempPathW(MAX_PATH, buf) == 0) return ".";
    std::wstring p(buf);
    if (!p.empty() && p.back() == L'\\') p.pop_back();
    return win::Utf8(p);
}

std::string WindowsPaths::DownloadsDir() const { return Known(FOLDERID_Downloads); }
std::string WindowsPaths::DocumentsDir() const { return Known(FOLDERID_Documents); }
std::string WindowsPaths::DesktopDir() const { return Known(FOLDERID_Desktop); }
std::string WindowsPaths::UserDataDir() const {
    if (const std::string root = ProfileRoot(); !root.empty()) return root + "\\local";
    return Known(FOLDERID_LocalAppData, "bps");
}
std::string WindowsPaths::AppDataDir() const {
    if (const std::string root = ProfileRoot(); !root.empty()) return root + "\\roaming";
    return Known(FOLDERID_RoamingAppData, "bps");
}
} // namespace bps::platform