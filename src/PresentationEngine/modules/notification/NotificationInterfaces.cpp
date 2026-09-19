#include "modules/notification/NotificationInterfaces.hpp"

#include "core/config/ConfigurationManager.hpp"

#include <algorithm>

namespace bps::notification {

// ---------------------------------------------------------------------------
// Default policy per severity (docs/specs/14 §Severity → channel mapping)
// ---------------------------------------------------------------------------

NotificationPolicy DefaultPolicyFor(Severity s) {
    NotificationPolicy p;
    switch (s) {
        case Severity::Critical:
            p.channels = {"center", "console"};
            p.priority = Priority::Critical;
            p.persistent = true;
            p.autoDismissMs = 0;
            break;
        case Severity::Error:
            p.channels = {"center", "console"};
            p.priority = Priority::High;
            p.persistent = true;
            p.autoDismissMs = 0;
            break;
        case Severity::Warning:
            p.channels = {"center", "statusbar", "console"};
            p.priority = Priority::Normal;
            p.autoDismissMs = 30000;
            break;
        case Severity::Success:
            p.channels = {"statusbar", "center", "console"};
            p.priority = Priority::Normal;
            p.autoDismissMs = 5000;
            break;
        case Severity::Information:
            p.channels = {"statusbar", "console"};
            p.priority = Priority::Normal;
            p.autoDismissMs = 8000;
            break;
        case Severity::Debug:
        default:
            p.channels = {"console"};
            p.priority = Priority::Low;
            p.autoDismissMs = 0;
            break;
    }
    return p;
}

// ---------------------------------------------------------------------------
// TemplateFormatter
// ---------------------------------------------------------------------------

Result<void> TemplateFormatter::RegisterTemplate(std::string_view topic,
                                                 std::string titleTemplate,
                                                 std::string messageTemplate) {
    if (topic.empty())
        return Error::Make(Err::InvalidArgument, "Notify", "template topic must not be empty");
    std::lock_guard<std::mutex> lock(mutex_);
    templates_[std::string(topic)] = Tmpl{std::move(titleTemplate), std::move(messageTemplate)};
    return Ok();
}

void TemplateFormatter::Clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    templates_.clear();
}

namespace {

std::string Render(std::string_view tpl, const NotificationSeed& seed) {
    std::string out;
    out.reserve(tpl.size() + 32);
    for (size_t i = 0; i < tpl.size();) {
        if (tpl[i] == '{') {
            size_t end = tpl.find('}', i);
            if (end == std::string_view::npos) {
                out += tpl.substr(i);
                break;
            }
            std::string_view key = tpl.substr(i + 1, end - i - 1);
            if (key == "title") out += seed.title;
            else if (key == "message") out += seed.message;
            else if (key == "module") out += seed.sourceModule;
            else if (key == "event") out += seed.sourceEvent;
            else if (key == "category") out += seed.category;
            else out += std::string(key);
            i = end + 1;
        } else {
            out += tpl[i++];
        }
    }
    return out;
}

} // namespace

void TemplateFormatter::Format(const NotificationSeed& seed, std::string& title,
                               std::string& message) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = templates_.find(seed.sourceEvent);
    if (it == templates_.end()) {
        title = seed.title;
        message = seed.message;
        return;
    }
    title = Render(it->second.title, seed);
    message = Render(it->second.message, seed);
}

// ---------------------------------------------------------------------------
// MemoryNotificationStorage (newest first, bounded)
// ---------------------------------------------------------------------------

Result<void> MemoryNotificationStorage::Append(const Notification& n) {
    std::lock_guard<std::mutex> lock(mutex_);
    byId_[n.id] = n;
    while (byId_.size() > maxEntries_) {
        auto it = std::prev(byId_.end());
        byId_.erase(it);
    }
    return Ok();
}

Result<void> MemoryNotificationStorage::Update(const Notification& n) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = byId_.find(n.id);
    if (it == byId_.end())
        return Error::Make(Err::Notification_HistoryFull, "Notify", "no such notification id");
    it->second = n;
    return Ok();
}

Result<void> MemoryNotificationStorage::Remove(uint64_t id) {
    std::lock_guard<std::mutex> lock(mutex_);
    byId_.erase(id);
    return Ok();
}

Result<std::vector<Notification>> MemoryNotificationStorage::All() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<Notification> out;
    out.reserve(byId_.size());
    for (const auto& [id, n] : byId_) {
        (void)id;
        out.push_back(n);
    }
    return Result<std::vector<Notification>>{std::move(out)};
}

Result<void> MemoryNotificationStorage::Clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    byId_.clear();
    return Ok();
}

size_t MemoryNotificationStorage::Count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return byId_.size();
}

// ---------------------------------------------------------------------------
// ConfigNotificationFilter — severity/category enablement from Configuration
// (docs/specs/14 §Notification Filters): notify.severity.*.enabled keys.
// ---------------------------------------------------------------------------

bool ConfigNotificationFilter::Allow(const Notification& n) const {
    auto& config = ConfigurationManager::Instance();
    if (!config.GetBool("notify.enabled", true)) return false;
    std::string key = std::string("notify.severity.") + ToString(n.severity) + ".enabled";
    if (!config.GetBool(key, true)) return false;
    if (n.severity == Severity::Debug && !config.GetBool("notify.showDebug", false)) return false;
    return true;
}

} // namespace bps::notification
