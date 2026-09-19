#include "platform/windows/WindowsNotifications.hpp"

#include "platform/windows/WinUtil.hpp"

#include <windows.h>

#include <fstream>
#include <string>

namespace bps::platform {

namespace {

// Escapes a value for a PowerShell single-quoted string ('' = one quote).
std::wstring PsQuote(std::string_view s) {
    std::wstring out = L"'";
    for (char c : s) {
        if (c == '\'') out += L"''";
        else out += static_cast<wchar_t>(c);
    }
    out += L"'";
    return out;
}

} // namespace

bool WindowsNotifications::Supported() const {
    // PowerShell is present on every supported Windows SKU.
    return true;
}

Result<void> WindowsNotifications::Show(std::string_view title, std::string_view body) {
    std::wstring script =
        L"Add-Type -AssemblyName System.Windows.Forms\r\n"
        L"Add-Type -AssemblyName System.Drawing\r\n"
        L"$n = New-Object System.Windows.Forms.NotifyIcon\r\n"
        L"$n.Icon = [System.Drawing.SystemIcons]::Information\r\n"
        L"$n.Visible = $true\r\n"
        L"$n.ShowBalloonTip(4000, " + PsQuote(title) + L", " + PsQuote(body) +
        L", [System.Windows.Forms.ToolTipIcon]::Info)\r\n"
        L"Start-Sleep -Seconds 8\r\n";   // keep the tray icon alive while the balloon shows

    wchar_t tmp[MAX_PATH + 1] = {};
    if (::GetTempPathW(MAX_PATH, tmp) == 0)
        return Error::Make(Err::IoError, "Notifications", "GetTempPathW failed");
    std::wstring scriptPath = std::wstring(tmp) + L"bps_notify_" +
                              std::to_wstring(::GetCurrentProcessId()) + L".ps1";

    {
        // .c_str() (const wchar_t*), not the std::wstring itself — this
        // libstdc++'s wofstream constructor overload set resolves a
        // filesystem::path-like template parameter for a bare wstring and
        // fails deduction (wants .make_preferred(), a path-only method);
        // the plain wchar_t* pointer overload is unambiguous.
        std::wofstream out(scriptPath.c_str(), std::ios::trunc);
        if (!out) return Error::Make(Err::IoError, "Notifications", "cannot write script");
        out << script;
    }

    std::wstring cmdLine = L"powershell.exe -NoProfile -NonInteractive "
                           L"-ExecutionPolicy Bypass -File \"" + scriptPath + L"\"";
    STARTUPINFOW si{};
    si.cb = sizeof si;
    PROCESS_INFORMATION pi{};
    BOOL ok = ::CreateProcessW(nullptr, cmdLine.data(), nullptr, nullptr, FALSE,
                               CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    if (ok) {
        ::CloseHandle(pi.hThread);
        ::WaitForSingleObject(pi.hProcess, 20000);
        DWORD code = 0;
        (void)::GetExitCodeProcess(pi.hProcess, &code);
        ::CloseHandle(pi.hProcess);
        (void)::DeleteFileW(scriptPath.c_str());
        if (code == 0) return Ok();
        return Error::Make(Err::IoError, "Notifications",
                           "powershell exited with code " + std::to_string(code));
    }
    (void)::DeleteFileW(scriptPath.c_str());
    return Error::Make(Err::IoError, "Notifications", "CreateProcessW failed");
}

} // namespace bps::platform
