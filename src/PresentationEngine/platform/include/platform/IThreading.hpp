#pragma once

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

    // The same, for a thread known only by its OS thread id (a QtThread: its handle is not a
    // std::thread). Where the OS cannot do it the answer is Unsupported.
    virtual Result<void> SetThreadAffinityById(uint64_t nativeThreadId, unsigned cpu) {
        (void)nativeThreadId; (void)cpu;
        return Error::Make(Err::Unsupported, "Threading", "affinity by thread id is not available here");
    }

    virtual Result<void> SetCurrentThreadPriority(int priority) = 0;
};
} // namespace bps::platform