#include "modules/notification/NotificationRules.hpp"

#include <cstring>

namespace bps::notification {

std::optional<NotificationPolicy> DefaultNotificationRules::Evaluate(
    const NotificationSeed& seed) const {
    // PresentationSaved: toast for 5s, never interrupt a live presentation.
    if (seed.sourceEvent == "project.saved") {
        NotificationPolicy p;
        p.show = true;
        p.channels = {"statusbar", "center", "console"};
        p.priority = Priority::Normal;
        p.autoDismissMs = 5000;
        p.suppressInPresenting = true;   // deferred to the center while presenting
        return p;
    }
    // Import completed: informational, brief.
    if (seed.sourceEvent == "content.asset_imported") {
        NotificationPolicy p;
        p.show = true;
        p.channels = {"statusbar", "center", "console"};
        p.priority = Priority::Normal;
        p.autoDismissMs = 5000;
        p.suppressInPresenting = true;
        return p;
    }
    // Low memory: warning shown immediately, but still never during a
    // presentation unless the presenter must act — treat as warning.
    if (seed.sourceEvent == "resource.pressure_high") {
        NotificationPolicy p;
        p.show = true;
        p.channels = {"center", "console"};
        p.priority = Priority::High;
        p.autoDismissMs = 0;
        p.persistent = true;
        return p;
    }
    // Kernel panic / database corruption: critical, persistent, always shown.
    if (seed.sourceEvent == "kernel.panic") {
        NotificationPolicy p;
        p.show = true;
        p.channels = {"center", "console"};
        p.priority = Priority::Critical;
        p.persistent = true;
        p.suppressInPresenting = false;   // must be seen even while presenting
        return p;
    }
    // Display events: informational (do not spam during a presentation).
    if (seed.sourceEvent == "monitor.connected" ||
        seed.sourceEvent == "monitor.disconnected") {
        NotificationPolicy p;
        p.show = true;
        p.channels = {"statusbar", "console"};
        p.priority = Priority::Low;
        p.autoDismissMs = 5000;
        p.suppressInPresenting = true;
        return p;
    }
    // Debug records: log-only.
    if (seed.severity == Severity::Debug) {
        NotificationPolicy p;
        p.show = true;
        p.channels = {"console"};
        p.priority = Priority::Low;
        return p;
    }
    return std::nullopt;
}

std::shared_ptr<INotificationRule> MakeDefaultRules() {
    return std::make_shared<DefaultNotificationRules>();
}

} // namespace bps::notification
