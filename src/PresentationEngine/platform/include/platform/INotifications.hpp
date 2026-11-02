#pragma once

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