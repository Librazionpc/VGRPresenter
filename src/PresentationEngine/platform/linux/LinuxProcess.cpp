#include "platform/linux/LinuxProcess.hpp"

#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <vector>

namespace bps::platform {

namespace {
Error ProcError(std::string_view op, std::string_view what) {
    return Error::Make(Err::IoError, "Process", std::string(op) + " failed: " + std::string(what));
}
} // namespace

Result<int> LinuxProcess::Start(std::string_view command, std::vector<std::string> args) {
    std::vector<char*> argv;
    argv.reserve(args.size() + 2);
    argv.push_back(const_cast<char*>(std::string(command).c_str()));
    for (auto& a : args) argv.push_back(const_cast<char*>(a.c_str()));
    argv.push_back(nullptr);

    pid_t pid = fork();
    if (pid < 0) return ProcError("fork", std::strerror(errno));
    if (pid == 0) {
        // Child: exec the requested program. execvp searches PATH.
        execvp(argv[0], argv.data());
        _exit(127); // only reached if exec failed
    }
    return static_cast<int>(pid);
}

Result<void> LinuxProcess::Wait(int pid, int timeoutMs) {
    const auto deadline = std::chrono::steady_clock::now() +
                          (timeoutMs < 0 ? std::chrono::hours(24 * 365)
                                         : std::chrono::milliseconds(timeoutMs));
    while (true) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (statuses_.count(pid) > 0) return Ok();
        }
        int status = 0;
        pid_t rc = waitpid(pid, &status, WNOHANG);
        if (rc == pid) {
            std::lock_guard<std::mutex> lock(mutex_);
            statuses_[pid] = status;
            return Ok();
        }
        if (rc < 0 && errno != EINTR) {
            if (errno == ECHILD) {
                // Already reaped elsewhere; treat as done.
                std::lock_guard<std::mutex> lock(mutex_);
                if (statuses_.count(pid) > 0) return Ok();
            }
            return ProcError("waitpid", std::strerror(errno));
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            // Reap anyway so the child does not remain a zombie.
            while (waitpid(pid, &status, WNOHANG) == pid) {}
            return Error::Make(Err::Timeout, "Process",
                               "wait for pid " + std::to_string(pid) + " timed out");
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
}

Result<int> LinuxProcess::ExitCode(int pid) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = statuses_.find(pid);
    if (it == statuses_.end())
        return Error::Make(Err::NotFound, "Process",
                           "pid " + std::to_string(pid) + " not reaped (still running?)");
    int status = it->second;
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
    return -1;
}

Result<void> LinuxProcess::Kill(int pid) {
    if (kill(pid, SIGKILL) != 0) return ProcError("kill(SIGKILL)", std::strerror(errno));
    return Ok();
}

Result<void> LinuxProcess::Terminate(int pid) {
    if (kill(pid, SIGTERM) != 0) return ProcError("kill(SIGTERM)", std::strerror(errno));
    return Ok();
}

Result<bool> LinuxProcess::IsRunning(int pid) const {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (statuses_.count(pid) > 0) return false;
    }
    if (kill(pid, 0) != 0) return false;
    // Reap if the child exited between the status check and kill(0).
    int status = 0;
    if (waitpid(pid, &status, WNOHANG) == pid) {
        std::lock_guard<std::mutex> lock(mutex_);
        statuses_[pid] = status;
        return false;
    }
    return true;
}

int LinuxProcess::CurrentProcessId() const { return static_cast<int>(getpid()); }

Result<int> LinuxProcess::Restart(int pid, std::string_view command,
                                 std::vector<std::string> args) {
    if (auto r = Terminate(pid); !r.ok()) {
        // Already-dead children (ESRCH) should not block the restart — just
        // start the fresh instance.
        if (errno != ESRCH) return r.error();
    } else if (auto w = Wait(pid, 5000); !w.ok()) {
        // Do not fail the restart just because the child ignored SIGTERM;
        // escalate to SIGKILL so the new instance can start.
        if (auto k = Kill(pid); !k.ok()) return k.error();
        (void)Wait(pid, 5000);
    }
    return Start(command, std::move(args));
}

Result<std::string> LinuxProcess::Environment(std::string_view variable) const {
    const char* v = std::getenv(std::string(variable).c_str());
    if (!v) return Error::Make(Err::NotFound, "Process",
                               "environment variable not set: " + std::string(variable));
    return std::string(v);
}

Result<void> LinuxProcess::SetEnvironment(std::string_view variable, std::string_view value) {
    if (setenv(std::string(variable).c_str(), std::string(value).c_str(), 1) != 0)
        return ProcError("setenv", std::strerror(errno));
    return Ok();
}

} // namespace bps::platform
