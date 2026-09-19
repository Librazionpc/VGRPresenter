#pragma once

// PresentationTimeline (docs/specs/19 §Timeline system). Sequential playback,
// parallel actions, delayed actions, timed events, loops. Deterministic: cues
// are sorted by their absolute time and fire in order as the clock advances.
// Adding cue types never changes this code.

#include "core/common/Common.hpp"
#include "modules/presentation/IPresentationCue.hpp"

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace bps::presentation {

class PresentationTimeline {
public:
    // Adds a cue; cues are kept sorted by AtSec().
    Result<void> AddCue(std::shared_ptr<IPresentationCue> cue);
    Result<void> RemoveCue(std::string_view id);

    // Resets the timeline (used when a presentation (re)starts).
    void Reset();
    void Clear();

    size_t Size() const;
    size_t Fired() const { return fired_.load(); }

    // Advances the clock to `timeSec`, firing every due cue in order.
    // Returns the number of cues fired in this call. A fired cue that returns
    // an error increments the engine error counter but never aborts playback.
    size_t Tick(double timeSec, CueContext& ctx);

private:
    mutable std::mutex mutex_;
    std::vector<std::shared_ptr<IPresentationCue>> cues_;
    size_t cursor_ = 0;
    std::atomic<size_t> fired_{0};
    std::atomic<uint64_t> errors_{0};
};

} // namespace bps::presentation
