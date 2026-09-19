#pragma once

// PAL process subsystem (Phase 2): start/stop/inspect external processes and
// read environment variables. Future plugin hosts (OBS, PowerPoint, AI tools)
// and the plugin sandbox's out-of-process isolation will use this.

#include "core/common/Common.hpp"

#include <string>
#include <vector>

namespace bps::platform {

class IProcess {
public:
    virtual ~IProcess() = default;

    // Starts `command` with `args`; returns the child pid (or an Error).
    virtual Result<int> Start(std::string_view command, std::vector<std::string> args = {}) = 0;

    // Blocks until the child exits or `timeoutMs` elapses (-1 = wait forever).
    virtual Result<void> Wait(int pid, int timeoutMs = -1) = 0;
    // Exit code of a reaped child (-1 while still running).
    virtual Result<int> ExitCode(int pid) const = 0;
    virtual Result<void> Kill(int pid) = 0;        // SIGKILL
    virtual Result<void> Terminate(int pid) = 0;   // SIGTERM
    virtual Result<bool> IsRunning(int pid) const = 0;

    // This process's pid.
    virtual int CurrentProcessId() const = 0;

    // Terminate `pid`, wait for it to exit (up to 5s), then start `command`
    // afresh; returns the new child pid.
    virtual Result<int> Restart(int pid, std::string_view command,
                                std::vector<std::string> args = {}) = 0;

    // Environment access (process-wide).
    virtual Result<std::string> Environment(std::string_view variable) const = 0;
    virtual Result<void> SetEnvironment(std::string_view variable, std::string_view value) = 0;
};

} // namespace bps::platform
