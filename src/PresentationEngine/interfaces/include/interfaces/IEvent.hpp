#pragma once

#include "core/common/Common.hpp"

namespace bps {

// Base interface for every event travelling through the EventBus (docs/specs/05).
//
// Design note: this base is deliberately virtual-free so that event types remain
// aggregates (list-initializable). Every event declares a compile-time topic:
//
//     struct SlideChanged : IEvent {
//         static constexpr const char* kTopic = "presentation.slide_changed";
//         int index = 0;
//         int total = 0;
//         std::string title;
//     };
//
// EventBus::Subscribe<EventT>() and Publish<EventT>() use EventT::kTopic, so the
// topic is known at compile time for typed use.
class IEvent {
public:
    // Empty polymorphic-free base. shared_ptr<const IEvent> with a derived
    // make_shared retains the correct deleter, so no virtual destructor is needed.
    ~IEvent() = default;
};

} // namespace bps
