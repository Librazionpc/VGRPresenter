#pragma once

// PAL timer subsystem (Phase 2): high-resolution time. Built on standard C++
// (std::chrono) per the PAL design rule — the interface exists so the rest of
// the engine never calls the OS clock directly.

#include "core/common/Common.hpp"

#include <chrono>
#include <cstdint>

namespace bps::platform {

class ITimer {
public:
    virtual ~ITimer() = default;

    // Monotonic high-resolution "now" in nanoseconds (steady clock).
    virtual int64_t NowNs() const = 0;
    // Wall-clock epoch milliseconds (for timestamps / logs).
    virtual int64_t WallClockMs() const = 0;
    // Sleep the calling thread for `micros` microseconds.
    virtual void SleepMicros(int64_t micros) const = 0;

    // Broken-down wall-clock time (UTC and local) for display / scheduling.
    struct ClockParts {
        int year = 0, month = 0, day = 0;       // month 1-12, day 1-31
        int hour = 0, minute = 0, second = 0;   // 24h
        int weekday = 0;                        // 0=Sunday .. 6=Saturday
    };
    virtual ClockParts UtcNow() const = 0;
    virtual ClockParts LocalNow() const = 0;

    // Engine time (shared steady clock the rest of the engine uses).
    EngineTime EngineNow() const { return bps::Now(); }

    struct Stopwatch {
        EngineTime start = EngineClock::now();
        double ElapsedSec() const {
            return std::chrono::duration<double>(EngineClock::now() - start).count();
        }
        int64_t ElapsedNs() const {
            return std::chrono::duration_cast<std::chrono::nanoseconds>(EngineClock::now() - start)
                .count();
        }
        void Reset() { start = EngineClock::now(); }
    };
    Stopwatch StartStopwatch() const { return Stopwatch{}; }
};

} // namespace bps::platform
