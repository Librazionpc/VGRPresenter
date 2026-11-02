#pragma once

#include "modules/notification/NotificationInterfaces.hpp"

#include <memory>
#include <vector>

namespace bps::notification {
class DefaultNotificationRules final : public INotificationRule {
public:
    const char* Name() const noexcept override { return "default-rules"; }

    std::optional<NotificationPolicy> Evaluate(const NotificationSeed& seed) const override;
};

// Convenience: register the default rule set with a service.
std::shared_ptr<INotificationRule> MakeDefaultRules();
} // namespace bps::notification