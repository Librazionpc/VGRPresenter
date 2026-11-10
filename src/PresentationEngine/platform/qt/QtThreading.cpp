#include "platform/qt/QtServices.hpp"

#include <QThread>

#include <cstdint>

namespace bps::platform {

// ---- Threading: QThread for the two questions Qt answers, the native adapter for the rest -------
//
// Qt has no cross-platform API for naming the CALLING thread, for pinning a thread to a CPU, or for
// its priority — those stay the native adapter's (the query below just forwards). What Qt does own
// is the machine's ideal worker count and the current thread's identity, which is what the engine
// schedules by; using it removes one OS-specific answer per platform from the scheduler.

unsigned QtThreading::HardwareConcurrency() const
{
    const int ideal = QThread::idealThreadCount();
    // A container or a restricted process can report 0/negative: fall back to the OS adapter rather
    // than telling the thread pool it has no cores.
    return ideal > 0 ? static_cast<unsigned>(ideal) : native_.HardwareConcurrency();
}

Result<void> QtThreading::SetCurrentThreadName(std::string_view name)
{
    return native_.SetCurrentThreadName(name);
}

std::string QtThreading::CurrentThreadName() const
{
    return native_.CurrentThreadName();
}

uint64_t QtThreading::CurrentThreadId() const
{
    return static_cast<uint64_t>(reinterpret_cast<uintptr_t>(QThread::currentThreadId()));
}

Result<void> QtThreading::SetThreadAffinity(std::thread &thread, unsigned cpu)
{
    return native_.SetThreadAffinity(thread, cpu);
}

Result<void> QtThreading::SetThreadAffinityById(uint64_t nativeThreadId, unsigned cpu)
{
    return native_.SetThreadAffinityById(nativeThreadId, cpu);
}

Result<void> QtThreading::SetCurrentThreadPriority(int priority)
{
    return native_.SetCurrentThreadPriority(priority);
}

} // namespace bps::platform
