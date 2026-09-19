#pragma once

// NotificationHistory (docs/specs/14 §Notification History): searchable,
// filterable, exportable history of delivered notifications. Backed by an
// INotificationStorage (memory + optional persistence).

#include "modules/notification/Notification.hpp"
#include "modules/notification/NotificationInterfaces.hpp"

#include <memory>
#include <string>
#include <vector>

namespace bps::notification {

struct HistoryQuery {
    std::string text;                 // substring match on title/message/source
    std::string category;             // empty = any
    std::string module;               // empty = any
    Severity minSeverity = Severity::Debug;
    bool unreadOnly = false;
    bool includeDismissed = true;
    int maxResults = 200;
};

class NotificationHistory {
public:
    NotificationHistory() : storage_(std::make_shared<MemoryNotificationStorage>()) {}

    void SetStorage(std::shared_ptr<INotificationStorage> s) {
        storage_ = s ? std::move(s) : storage_;
    }
    std::shared_ptr<INotificationStorage> Storage() const { return storage_; }

    Result<void> Record(const Notification& n);
    Result<void> UpdateRecord(const Notification& n);
    Result<void> MarkRead(uint64_t id);
    Result<void> Dismiss(uint64_t id);
    Result<void> Remove(uint64_t id);
    std::vector<Notification> Query(const HistoryQuery& q) const;
    std::vector<Notification> All() const;
    Result<void> Clear();
    size_t Count() const;

    // Export as a JSON array to a host path (PAL filesystem).
    Result<void> Export(std::string_view hostPath) const;

private:
    std::shared_ptr<INotificationStorage> storage_;
};

} // namespace bps::notification
