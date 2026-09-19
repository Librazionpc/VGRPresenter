#pragma once

// Windows PAL backend for the process subsystem (CreateProcess + Win32
// process/thread APIs).

#include "platform/IProcess.hpp"

#ifdef _WIN32
#include <windows.h>
#endif

#include <mutex>
#include <unordered_map>

namespace bps::platform {

class WindowsProcess final : public IProcess {
public:
    Result<int> Start(std::string_view command, std::vector<std::string> args = {}) override;
    Result<void> Wait(int pid, int timeoutMs = -1) override;
    Result<int> ExitCode(int pid) const override;
    Result<void> Kill(int pid) override;
    Result<void> Terminate(int pid) override;
    Result<bool> IsRunning(int pid) const override;
    int CurrentProcessId() const override;
    Result<int> Restart(int pid, std::string_view command,
                        std::vector<std::string> args = {}) override;

    Result<std::string> Environment(std::string_view variable) const override;
    Result<void> SetEnvironment(std::string_view variable, std::string_view value) override;

private:
    // pid -> process handle (kept so Wait/ExitCode can query the live process).
    mutable std::mutex mutex_;
    std::unordered_map<int, HANDLE> handles_;   // NOLINT (Windows HANDLE = void*)
};

} // namespace bps::platform
