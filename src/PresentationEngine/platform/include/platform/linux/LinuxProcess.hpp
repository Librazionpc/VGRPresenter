#pragma once

// Linux PAL backend for the process subsystem (fork/exec, waitpid, signals).

#include "../IProcess.hpp"

#include <mutex>
#include <unordered_map>

namespace bps::platform {

class LinuxProcess final : public IProcess {
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
    // Reaped exit statuses; a running (unreaped) child is simply absent.
    // Both members are mutable: the const read APIs (ExitCode/IsRunning) still
    // record reaping when a child exits between polls.
    mutable std::mutex mutex_;
    mutable std::unordered_map<int, int> statuses_;
};

} // namespace bps::platform
