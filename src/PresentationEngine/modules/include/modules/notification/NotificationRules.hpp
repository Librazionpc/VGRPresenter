#pragma once

// NotificationRules (docs/specs/14 §Notification Rules + §Notification Policy
// Engine). Rules are evaluated in registration order; the first match wins.
// Rather than hardcoding behavior in the service, policies are data — a rule
// decides show?, channels, priority, auto-dismiss, persistence and whether it
// must be suppressed during a live presentation. Advanced users/administrators
// can register additional rules without touching engine code (Open/Closed).

#include "modules/notification/NotificationInterfaces.hpp"

#include <memory>
#include <vector>

namespace bps::notification {

// Default rule set implementing the documented examples (docs/specs/14 §Policy
// Engine): ProjectSaved → toast 5s unless presenting; DatabaseCorrupt /
// MemoryLow → critical persistent; ImportCompleted → informational.
class DefaultNotificationRules final : public INotificationRule {
public:
    const char* Name() const noexcept override { return "default-rules"; }

    std::optional<NotificationPolicy> Evaluate(const NotificationSeed& seed) const override;
};

// Convenience: register the default rule set with a service.
std::shared_ptr<INotificationRule> MakeDefaultRules();

} // namespace bps::notification
