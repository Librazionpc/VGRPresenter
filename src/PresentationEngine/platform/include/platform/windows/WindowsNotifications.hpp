#pragma once

// Windows PAL backend for desktop notifications: shows a tray-balloon via a
// short PowerShell script (System.Windows.Forms.NotifyIcon). Best-effort —
// returns Err::Unsupported when PowerShell is unavailable.

#include "../INotifications.hpp"

namespace bps::platform {

class WindowsNotifications final : public INotifications {
public:
    bool Supported() const override;
    Result<void> Show(std::string_view title, std::string_view body) override;
};

} // namespace bps::platform
