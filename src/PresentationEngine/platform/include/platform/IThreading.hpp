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

    virtual Result<void> SetCurrentThreadPriority(int priority) = 0;
};
} // namespace bps::platform