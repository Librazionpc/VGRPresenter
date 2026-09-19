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
    std::string appdata = Known(FOLDERID_RoamingAppData);
    if (appdata.empty()) return {};
    return appdata + "\\bps";
}

std::string WindowsPaths::CacheDir() const { return Known(FOLDERID_LocalAppData, "bps\\cache"); }
std::string WindowsPaths::PluginDir() const { return Known(FOLDERID_LocalAppData, "bps\\plugins"); }
std::string WindowsPaths::AssetDir() const { return Known(FOLDERID_LocalAppData, "bps\\assets"); }
std::string WindowsPaths::LogDir() const { return Known(FOLDERID_LocalAppData, "bps\\logs"); }

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
std::string WindowsPaths::UserDataDir() const { return Known(FOLDERID_LocalAppData, "bps"); }
std::string WindowsPaths::AppDataDir() const { return Known(FOLDERID_RoamingAppData, "bps"); }

} // namespace bps::platform
