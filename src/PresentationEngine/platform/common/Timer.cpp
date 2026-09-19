#include "platform/common/Timer.hpp"

#include <ctime>
#include <thread>

namespace bps::platform {

int64_t TimerImpl::NowNs() const {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

int64_t TimerImpl::WallClockMs() const {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

void TimerImpl::SleepMicros(int64_t micros) const {
    if (micros <= 0) return;
    std::this_thread::sleep_for(std::chrono::microseconds(micros));
}

namespace {
ITimer::ClockParts FromTm(const std::tm& t) {
    ITimer::ClockParts p;
    p.year = t.tm_year + 1900;
    p.month = t.tm_mon + 1;
    p.day = t.tm_mday;
    p.hour = t.tm_hour;
    p.minute = t.tm_min;
    p.second = t.tm_sec;
    p.weekday = t.tm_wday;   // 0=Sunday (matches struct tm)
    return p;
}
} // namespace

ITimer::ClockParts TimerImpl::UtcNow() const {
    // std::gmtime/std::localtime are standard C++ (<ctime>) and build on every
    // OS — unlike the POSIX-only gmtime_r/localtime_r. They return a pointer to
    // a shared static buffer, which is fine here: these calls are infrequent
    // (logs/timestamps) and the result is copied out immediately.
    std::time_t t = std::time(nullptr);
    return FromTm(*std::gmtime(&t));
}

ITimer::ClockParts TimerImpl::LocalNow() const {
    std::time_t t = std::time(nullptr);
    return FromTm(*std::localtime(&t));
}

} // namespace bps::platform
