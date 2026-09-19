#pragma once

// Common (OS-agnostic) PAL backend for the timer subsystem — standard C++
// clocks per the PAL design rule.

#include "../ITimer.hpp"

namespace bps::platform {

class TimerImpl final : public ITimer {
public:
    int64_t NowNs() const override;
    int64_t WallClockMs() const override;
    void SleepMicros(int64_t micros) const override;
    ClockParts UtcNow() const override;
    ClockParts LocalNow() const override;
};

} // namespace bps::platform
