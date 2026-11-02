#pragma once

#include "../INotifications.hpp"

namespace bps::platform {
class WindowsNotifications final : public INotifications {
public:
    bool Supported() const override;
    Result<void> Show(std::string_view title, std::string_view body) override;
};
} // namespace bps::platform