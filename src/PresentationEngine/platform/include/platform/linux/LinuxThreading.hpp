#pragma once

// Linux PAL backend for the threading subsystem (pthread naming/affinity,
// gettid). Creation and synchronization stay on std::thread/std::mutex.

#include "../IThreading.hpp"

namespace bps::platform {

class LinuxThreading final : public IThreading {
public:
    unsigned HardwareConcurrency() const override;
    Result<void> SetCurrentThreadName(std::string_view name) override;
    std::string CurrentThreadName() const override;
    uint64_t CurrentThreadId() const override;
    Result<void> SetThreadAffinity(std::thread& thread, unsigned cpu) override;
    Result<void> SetCurrentThreadPriority(int priority) override;
};

} // namespace bps::platform
