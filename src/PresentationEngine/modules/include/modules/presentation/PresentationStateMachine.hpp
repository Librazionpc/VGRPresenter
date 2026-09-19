#pragma once

// PresentationStateMachine (docs/specs/19 §State machine). Every presentation
// moves through well-defined states; no invalid transitions are possible. The
// runtime validates each transition before applying it.

#include "core/common/Common.hpp"
#include "modules/presentation/PresentationTypes.hpp"

#include <atomic>

namespace bps::presentation {

class PresentationStateMachine {
public:
    // Returns true when `from -> to` is a legal transition.
    static bool Can(PresentationState from, PresentationState to);

    // Attempts a transition; fails with Presentation_InvalidTransition when
    // the move is not allowed (19 §State machine: no invalid transitions).
    Result<void> TransitionTo(PresentationState to);

    PresentationState State() const { return state_.load(); }
    void Reset(PresentationState to = PresentationState::Created) { state_.store(to); }

private:
    std::atomic<PresentationState> state_{PresentationState::Created};
};

} // namespace bps::presentation
