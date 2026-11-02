#pragma once

#include "core/common/Common.hpp"

#include <chrono>
#include <optional>

namespace bps {
class PressureLatch {
public:
    struct Config {
        PressureLevel raiseAt = PressureLevel::High;       // this level or worse can raise
        PressureLevel clearBelow = PressureLevel::Medium;  // must drop below this to clear
        std::chrono::milliseconds sustain{0};              // continuous time at/above raiseAt
        std::chrono::milliseconds cooldown{0};             // min time between two raises
    };

    enum class Transition { None, Raised, Cleared };

    PressureLatch() = default;
    explicit PressureLatch(Config cfg) : cfg_(cfg) {}

    void Configure(Config cfg) { cfg_ = cfg; }
    bool Active() const noexcept { return active_; }

    // Feed one reading. Returns Raised/Cleared on the sample that changes state.
    Transition Update(PressureLevel level, EngineTime now) {
        if (level >= cfg_.raiseAt) {
            if (!aboveSince_) aboveSince_ = now;
            if (!active_ && now - *aboveSince_ >= cfg_.sustain &&
                (!lastRaised_ || now - *lastRaised_ >= cfg_.cooldown)) {
                active_ = true;
                lastRaised_ = now;
                return Transition::Raised;
            }
            return Transition::None;
        }
        // Below the raise level: the "continuous" run is broken.
        aboveSince_.reset();
        if (active_ && level < cfg_.clearBelow) {
            active_ = false;
            return Transition::Cleared;
        }
        return Transition::None;
    }

private:
    Config cfg_;
    bool active_ = false;
    std::optional<EngineTime> aboveSince_;   // start of the current unbroken run at/above raiseAt
    std::optional<EngineTime> lastRaised_;
};
} // namespace bps