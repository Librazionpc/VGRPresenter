#pragma once

#include "core/common/Common.hpp"

namespace bps {
class IEvent {
public:
    // Empty polymorphic-free base. shared_ptr<const IEvent> with a derived
    // make_shared retains the correct deleter, so no virtual destructor is needed.
    ~IEvent() = default;
};
} // namespace bps