#include "modules/presentation/PresentationStateMachine.hpp"

namespace bps::presentation {

bool PresentationStateMachine::Can(PresentationState from, PresentationState to) {
    if (from == to) return true;
    // Closed is the terminal state — reachable from anywhere.
    if (to == PresentationState::Closed) return true;
    switch (from) {
        case PresentationState::Created:    return to == PresentationState::Loaded;
        case PresentationState::Loaded:     return to == PresentationState::Validated;
        case PresentationState::Validated:  return to == PresentationState::Compiled;
        case PresentationState::Compiled:   return to == PresentationState::Prepared;
        case PresentationState::Prepared:   return to == PresentationState::Ready;
        case PresentationState::Ready:
            return to == PresentationState::Live || to == PresentationState::Stopped;
        case PresentationState::Live:
            return to == PresentationState::Paused || to == PresentationState::Stopped ||
                   to == PresentationState::Finished;
        case PresentationState::Paused:
            return to == PresentationState::Live || to == PresentationState::Stopped;
        case PresentationState::Stopped:
            return to == PresentationState::Ready;
        case PresentationState::Finished:
            return to == PresentationState::Ready;
        case PresentationState::Recovering:
            return to == PresentationState::Loaded || to == PresentationState::Prepared ||
                   to == PresentationState::Ready;
        case PresentationState::Closed:
            return false;
    }
    return false;
}

Result<void> PresentationStateMachine::TransitionTo(PresentationState to) {
    auto from = state_.load();
    if (!Can(from, to))
        return Error::Make(Err::Presentation_InvalidTransition, "PresentationStateMachine",
                           "invalid transition " + std::string(ToString(from)) + " -> " +
                               std::string(ToString(to)));
    state_.store(to);
    return Ok();
}

} // namespace bps::presentation
