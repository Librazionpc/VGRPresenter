#pragma once

// PAL desktop notifications subsystem (Phase 2, DoD §17): fire-and-forget
// notifications (Linux: notify-send; Windows: PowerShell balloon tooltip).
// Notifications are best-effort — a missing provider yields
// Err::Unsupported (or Err::IoError when the provider tool failed), never a
// crash. Do not block on these in a hot path.

#include "core/common/Common.hpp"

#include <string>
#include <string_view>

namespace bps::platform {

class INotifications {
public:
    virtual ~INotifications() = default;

    // True when a desktop notification provider is available right now.
    virtual bool Supported() const = 0;

    // Show a transient notification. May return Unsupported when no provider
    // exists (headless server), IoError when the provider tool failed.
    virtual Result<void> Show(std::string_view title, std::string_view body) = 0;
};

} // namespace bps::platform
