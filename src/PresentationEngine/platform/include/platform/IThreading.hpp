#pragma once

// PAL threading helpers (Phase 2). Standard C++ (std::thread/std::mutex) is the
// abstraction for creation and synchronization per the PAL design rule; this
// interface adds the OS-specific pieces std does not provide: thread naming,
// OS thread id, and CPU affinity. The engine never calls pthread/Windows APIs
// directly.

#include "core/common/Common.hpp"

#include <string>
#include <thread>

namespace bps::platform {

class IThreading {
public:
    virtual ~IThreading() = default;

    virtual unsigned HardwareConcurrency() const = 0;

    virtual Result<void> SetCurrentThreadName(std::string_view name) = 0;
    virtual std::string CurrentThreadName() const = 0;
    virtual uint64_t CurrentThreadId() const = 0;   // OS thread id (tid)

    // Pin `thread` to `cpu` (0-based logical CPU). Unsupported where the OS
    // does not allow it.
    virtual Result<void> SetThreadAffinity(std::thread& thread, unsigned cpu) = 0;

    // Best-effort priority hint for the CALLING thread (DoD §6): 0 = leave
    // unchanged, 1 = lowest ... 5 = highest. The OS mapping is approximate;
    // always returns Ok on platforms where the OS refuses to change priority.
    virtual Result<void> SetCurrentThreadPriority(int priority) = 0;
};

} // namespace bps::platform
