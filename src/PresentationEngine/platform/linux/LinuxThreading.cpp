#include "platform/linux/LinuxThreading.hpp"

#include <pthread.h>
#include <sched.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <thread>

namespace bps::platform {

unsigned LinuxThreading::HardwareConcurrency() const {
    unsigned n = std::thread::hardware_concurrency();
    return n > 0 ? n : 1;
}

Result<void> LinuxThreading::SetCurrentThreadName(std::string_view name) {
    // pthread_setname_np caps at 15 chars including the terminator.
    char buf[16] = {0};
    std::strncpy(buf, std::string(name).c_str(), sizeof(buf) - 1);
    if (pthread_setname_np(pthread_self(), buf) != 0)
        return Error::Make(Err::IoError, "Threading", "pthread_setname_np failed");
    return Ok();
}

std::string LinuxThreading::CurrentThreadName() const {
    char buf[16] = {0};
    if (pthread_getname_np(pthread_self(), buf, sizeof(buf)) != 0) return {};
    return buf;
}

uint64_t LinuxThreading::CurrentThreadId() const {
    return static_cast<uint64_t>(syscall(SYS_gettid));
}

Result<void> LinuxThreading::SetCurrentThreadPriority(int priority) {
    if (priority == 0) return Ok();
    int clamped = std::max(1, std::min(5, priority));
    // 1 -> nice 6, 2 -> nice 2, 3 -> nice -2, 4 -> nice -6, 5 -> nice -10.
    int nice = 10 - clamped * 4;
    errno = 0;
    if (setpriority(PRIO_PROCESS, 0, nice) != 0 && errno != 0)
        return Error::Make(Err::IoError, "Threading",
                           "setpriority failed: " + std::string(std::strerror(errno)));
    return Ok();
}

Result<void> LinuxThreading::SetThreadAffinity(std::thread& thread, unsigned cpu) {
    cpu_set_t set;
    CPU_ZERO(&set);
    if (cpu >= static_cast<unsigned>(CPU_SETSIZE))
        return Error::Make(Err::InvalidArgument, "Threading",
                           "cpu " + std::to_string(cpu) + " out of range");
    CPU_SET(cpu, &set);
    int rc = pthread_setaffinity_np(thread.native_handle(), sizeof(set), &set);
    if (rc != 0)
        return Error::Make(Err::IoError, "Threading",
                           "pthread_setaffinity_np failed for cpu " + std::to_string(cpu) +
                               ": " + std::strerror(rc));
    return Ok();
}

} // namespace bps::platform
