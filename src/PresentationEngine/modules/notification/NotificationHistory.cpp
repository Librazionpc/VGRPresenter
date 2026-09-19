#include "modules/notification/NotificationHistory.hpp"

#include "core/config/Json.hpp"
#include "platform/IPlatform.hpp"
#include "platform/PlatformAccessor.hpp"

#include <algorithm>
#include <sstream>

namespace bps::notification {

Result<void> NotificationHistory::Record(const Notification& n) {
    return storage_->Append(n);
}

Result<void> NotificationHistory::UpdateRecord(const Notification& n) {
    return storage_->Update(n);
}

Result<void> NotificationHistory::MarkRead(uint64_t id) {
    auto all = storage_->All();
    if (!all.ok()) return all.error();
    for (auto& n : all.value()) {
        if (n.id == id) {
            n.read = true;
            return storage_->Update(n);
        }
    }
    return Ok();   // idempotent
}

Result<void> NotificationHistory::Dismiss(uint64_t id) {
    auto all = storage_->All();
    if (!all.ok()) return all.error();
    for (auto& n : all.value()) {
        if (n.id == id) {
            n.dismissed = true;
            return storage_->Update(n);
        }
    }
    return Ok();
}

Result<void> NotificationHistory::Remove(uint64_t id) {
    return storage_->Remove(id);
}

std::vector<Notification> NotificationHistory::Query(const HistoryQuery& q) const {
    auto all = storage_->All();
    if (!all.ok()) return {};
    std::vector<Notification> out;
    for (const auto& n : all.value()) {
        if (q.minSeverity != Severity::Debug &&
            static_cast<int>(n.severity) < static_cast<int>(q.minSeverity))
            continue;
        if (!q.category.empty() && n.category != q.category) continue;
        if (!q.module.empty() && n.sourceModule != q.module) continue;
        if (q.unreadOnly && n.read) continue;
        if (!q.includeDismissed && n.dismissed) continue;
        if (!q.text.empty()) {
            bool hit = n.title.contains(q.text) || n.message.contains(q.text) ||
                       n.sourceModule.contains(q.text);
            if (!hit) continue;
        }
        out.push_back(n);
        if (static_cast<int>(out.size()) >= q.maxResults) break;
    }
    return out;
}

std::vector<Notification> NotificationHistory::All() const {
    auto all = storage_->All();
    return all.ok() ? all.value() : std::vector<Notification>{};
}

Result<void> NotificationHistory::Clear() { return storage_->Clear(); }

size_t NotificationHistory::Count() const { return storage_->Count(); }

Result<void> NotificationHistory::Export(std::string_view hostPath) const {
    json::Value::Array arr;
    for (const auto& n : All()) {
        json::Value::Object o;
        o["id"] = json::Value::Number(static_cast<double>(n.id));
        o["timestampMs"] = json::Value::Number(static_cast<double>(n.timestampMs));
        o["severity"] = json::Value::String(ToString(n.severity));
        o["category"] = json::Value::String(n.category);
        o["module"] = json::Value::String(n.sourceModule);
        o["event"] = json::Value::String(n.sourceEvent);
        o["title"] = json::Value::String(n.title);
        o["message"] = json::Value::String(n.message);
        o["read"] = json::Value::Bool(n.read);
        o["dismissed"] = json::Value::Bool(n.dismissed);
        arr.push_back(json::Value(std::move(o)));
    }
    auto& fs = platform::PlatformAccessor::Get().Filesystem();
    return fs.Write(hostPath, json::Value(std::move(arr)).ToString());
}

} // namespace bps::notification
