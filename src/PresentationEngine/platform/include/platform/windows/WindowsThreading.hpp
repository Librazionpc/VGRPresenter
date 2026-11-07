#pragma once

#include "platform/IThreading.hpp"

namespace bps::platform {
class WindowsThreading final : public IThreading {
public:
    unsigned HardwareConcurrency() const override;
    Result<void> SetCurrentThreadName(std::string_view name) override;
    std::string CurrentThreadName() const override;
    uint64_t CurrentThreadId() const override;
    Result<void> SetThreadAffinity(std::thread& thread, unsigned cpu) override;
    Result<void> SetThreadAffinityById(uint64_t nativeThreadId, unsigned cpu) override;
    Result<void> SetCurrentThreadPriority(int priority) override;
};
} // namespace bps::platform