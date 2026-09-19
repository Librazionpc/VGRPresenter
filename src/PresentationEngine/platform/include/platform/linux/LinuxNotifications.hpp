#pragma once

// Linux PAL backend for desktop notifications: bridges to notify-send
// (libnotify). Best-effort: headless hosts return Err::Unsupported.

#include "../INotifications.hpp"

namespace bps::platform {

class LinuxNotifications final : public INotifications {
public:
    bool Supported() const override;
    Result<void> Show(std::string_view title, std::string_view body) override;
};

} // namespace bps::platform
