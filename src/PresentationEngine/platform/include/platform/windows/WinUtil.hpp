#pragma once

// Shared helpers for the Windows PAL backends. Only ever included by
// platform/windows/ sources.

#ifdef _WIN32
// Without this, <windows.h> pulls in the legacy Winsock 1 header itself —
// which then blocks any LATER `#include <winsock2.h>` in the same
// translation unit (its own guard trips against what windows.h already
// dragged in), silently skipping everything gated behind it: NTDDI_VERSION-
// dependent chunks of <iphlpapi.h> including its own `#include <netioapi.h>`
// (MIB_IF_TABLE2/GetIfTable2/FreeMibTable — WindowsPlatform.cpp's network
// telemetry) and <iphlpapi.h>'s GetAdaptersAddresses/IP_ADAPTER_ADDRESSES
// (WindowsNetwork.cpp). WinUtil.hpp is the first header nearly every
// Windows PAL .cpp includes, so this is the one place that fixes it for
// all of them instead of each file having to get its own include order
// exactly right.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
// WIN32_LEAN_AND_MEAN alone only stops <windows.h> auto-pulling the legacy
// Winsock 1 header — it doesn't provide Winsock 2 itself. Callers that never
// explicitly include <winsock2.h> (e.g. WindowsPlatform.cpp, which only
// wants <iphlpapi.h>/<netioapi.h> for network telemetry) still need it
// loaded before <windows.h> for the same NTDDI_VERSION-gated chunks to
// unlock, so it's included here explicitly rather than left to each file.
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <string>
#include <string_view>

namespace bps::platform::win {

// GetProcAddress hands back a generic FARPROC; casting it straight to a typed
// function pointer trips -Wcast-function-type. Routing through void* is the
// standard, warning-free way to say "I know this export's real signature".
template <typename Fn>
inline Fn ProcAddress(HMODULE module, const char* name) {
    return reinterpret_cast<Fn>(reinterpret_cast<void*>(GetProcAddress(module, name)));
}

// UTF-16 -> UTF-8 (WideCharToMultiByte); empty on failure.
inline std::string Utf8(std::wstring_view w) {
    if (w.empty()) return {};
    int len = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0,
                                  nullptr, nullptr);
    if (len <= 0) return {};
    std::string out(static_cast<size_t>(len), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), out.data(), len,
                        nullptr, nullptr);
    return out;
}

// UTF-8 -> UTF-16 (MultiByteToWideChar); empty on failure.
inline std::wstring Wide(std::string_view s) {
    if (s.empty()) return {};
    int len = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    if (len <= 0) return {};
    std::wstring out(static_cast<size_t>(len), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), len);
    return out;
}

inline std::string LastErrorString(DWORD code) {
    char* buf = nullptr;
    DWORD n = FormatMessageA(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                                 FORMAT_MESSAGE_IGNORE_INSERTS,
                             nullptr, code, 0, reinterpret_cast<char*>(&buf), 0, nullptr);
    std::string out = (n > 0 && buf) ? std::string(buf, n) : "unknown error";
    if (buf) LocalFree(buf);
    while (!out.empty() && (out.back() == '\r' || out.back() == '\n')) out.pop_back();
    return out;
}

} // namespace bps::platform::win
#endif // _WIN32
