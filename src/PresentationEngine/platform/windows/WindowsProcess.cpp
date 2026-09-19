#include "platform/windows/WindowsProcess.hpp"
#include "platform/windows/WinUtil.hpp"

#include <windows.h>

#include <sstream>

namespace bps::platform {

namespace {
Error ProcError(std::string_view op) {
    return Error::Make(Err::IoError, "Process",
                       std::string(op) + " failed: " + win::LastErrorString(GetLastError()));
}

std::string CommandLine(std::string_view command, const std::vector<std::string>& args) {
    std::string line(command);
    for (const auto& a : args) {
        line += ' ';
        // Quote arguments that contain spaces (simple quoting).
        if (a.find(' ') != std::string::npos || a.find('\t') != std::string::npos)
            line += std::string("\"") + a + "\"";
        else
            line += a;
    }
    return line;
}
} // namespace

Result<int> WindowsProcess::Start(std::string_view command, std::vector<std::string> args) {
    std::string cmdline = CommandLine(command, args);
    // CreateProcessA is fine for ASCII paths; a UTF-8 build would use the W API.
    STARTUPINFOA si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    std::string mutableCmd = cmdline;
    BOOL ok = CreateProcessA(nullptr, mutableCmd.data(), nullptr, nullptr, FALSE,
                             CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    if (!ok) return ProcError("CreateProcessA");
    CloseHandle(pi.hThread);
    std::lock_guard<std::mutex> lock(mutex_);
    handles_[static_cast<int>(pi.dwProcessId)] = pi.hProcess;
    return static_cast<int>(pi.dwProcessId);
}

Result<void> WindowsProcess::Wait(int pid, int timeoutMs) {
    HANDLE h = nullptr;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = handles_.find(pid);
        if (it == handles_.end())
            return Error::Make(Err::NotFound, "Process",
                               "pid " + std::to_string(pid) + " is not a child of this engine");
        h = it->second;
    }
    DWORD ms = timeoutMs < 0 ? INFINITE : static_cast<DWORD>(timeoutMs);
    if (WaitForSingleObject(h, ms) == WAIT_TIMEOUT)
        return Error::Make(Err::Timeout, "Process",
                           "wait for pid " + std::to_string(pid) + " timed out");
    return Ok();
}

Result<int> WindowsProcess::ExitCode(int pid) const {
    HANDLE h = nullptr;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = handles_.find(pid);
        if (it == handles_.end())
            return Error::Make(Err::NotFound, "Process", "unknown pid " + std::to_string(pid));
        h = it->second;
    }
    DWORD code = 0;
    if (!GetExitCodeProcess(h, &code)) return ProcError("GetExitCodeProcess");
    return static_cast<int>(code);
}

Result<void> WindowsProcess::Kill(int pid) { return Terminate(pid); }

Result<void> WindowsProcess::Terminate(int pid) {
    HANDLE h = nullptr;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = handles_.find(pid);
        if (it == handles_.end())
            return Error::Make(Err::NotFound, "Process", "unknown pid " + std::to_string(pid));
        h = it->second;
    }
    if (!TerminateProcess(h, 1)) return ProcError("TerminateProcess");
    return Ok();
}

Result<bool> WindowsProcess::IsRunning(int pid) const {
    auto code = ExitCode(pid);
    if (!code.ok()) return false;
    return code.value() == STILL_ACTIVE;
}

int WindowsProcess::CurrentProcessId() const { return static_cast<int>(GetCurrentProcessId()); }

Result<int> WindowsProcess::Restart(int pid, std::string_view command,
                                    std::vector<std::string> args) {
    auto alive = IsRunning(pid);
    if (!alive.ok() || alive.value()) {
        if (auto r = Terminate(pid); !r.ok()) {
            if (!alive.ok()) return r.error();   // only ESRCH-equivalent can pass
            return r.error();
        }
        (void)Wait(pid, 5000);
    }
    return Start(command, std::move(args));
}

Result<std::string> WindowsProcess::Environment(std::string_view variable) const {
    char buf[4096] = {0};
    DWORD n = GetEnvironmentVariableA(std::string(variable).c_str(), buf, sizeof(buf));
    if (n == 0)
        return Error::Make(Err::NotFound, "Process",
                           "environment variable not set: " + std::string(variable));
    return std::string(buf);
}

Result<void> WindowsProcess::SetEnvironment(std::string_view variable, std::string_view value) {
    // _putenv_s, not the raw Win32 SetEnvironmentVariableA: this MinGW
    // runtime's std::getenv() (used by e.g. WindowsNetwork::Proxy()) reads
    // the CRT's own environment copy, which SetEnvironmentVariableA doesn't
    // touch — a variable set that way is invisible to getenv() in the same
    // process even though it's genuinely set at the OS level. _putenv_s
    // updates both.
    if (_putenv_s(std::string(variable).c_str(), std::string(value).c_str()) != 0)
        return ProcError("_putenv_s");
    return Ok();
}

} // namespace bps::platform
