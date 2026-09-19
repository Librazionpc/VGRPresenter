#include "modules/notification/NotificationProviders.hpp"

#include "core/logging/Logger.hpp"
#include "platform/PlatformAccessor.hpp"

#include <algorithm>
#include <format>

namespace bps::notification {

// ---------------------------------------------------------------------------
// ConsoleNotificationProvider
// ---------------------------------------------------------------------------

Result<void> ConsoleNotificationProvider::Show(const Notification& n) {
    auto& logger = Logger::Instance();
    std::string line = std::string("[") + ToString(n.severity) + "][" + n.category + "] " +
                       n.title;
    if (!n.message.empty()) line += std::string(" - ") + n.message;
    switch (n.severity) {
        case Severity::Critical:
        case Severity::Error:   logger.Error(line, "Notify"); break;
        case Severity::Warning: logger.Warning(line, "Notify"); break;
        case Severity::Debug:   logger.Debug(line, "Notify"); break;
        default:                logger.Info(line, "Notify"); break;
    }
    return Ok();
}

Result<void> ConsoleNotificationProvider::Update(const Notification& n) {
    return Show(n);
}

// ---------------------------------------------------------------------------
// CenterNotificationProvider
// ---------------------------------------------------------------------------

Result<void> CenterNotificationProvider::Show(const Notification& n) {
    std::lock_guard<std::mutex> lock(mutex_);
    recent_.insert(recent_.begin(), n);
    if (recent_.size() > 200) recent_.resize(200);
    return Ok();
}

Result<void> CenterNotificationProvider::Update(const Notification& n) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& e : recent_) {
        if (e.id == n.id) {
            e = n;
            break;
        }
    }
    return Ok();
}

Result<void> CenterNotificationProvider::Dismiss(uint64_t id) {
    std::lock_guard<std::mutex> lock(mutex_);
    dismissed_.push_back(id);
    return Ok();
}

Notification CenterNotificationProvider::Latest() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return recent_.empty() ? Notification{} : recent_.front();
}

std::vector<Notification> CenterNotificationProvider::Recent(size_t max) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return std::vector<Notification>(recent_.begin(),
                                     recent_.begin() + std::min(max, recent_.size()));
}

// ---------------------------------------------------------------------------
// StatusBarNotificationProvider
// ---------------------------------------------------------------------------

Result<void> StatusBarNotificationProvider::Show(const Notification& n) {
    std::lock_guard<std::mutex> lock(mutex_);
    latest_ = n;
    hasLatest_ = true;
    return Ok();
}

Result<void> StatusBarNotificationProvider::Update(const Notification& n) {
    return Show(n);
}

Notification StatusBarNotificationProvider::Latest() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return latest_;
}

std::string StatusBarNotificationProvider::LatestText() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return hasLatest_ ? latest_.message : std::string{};
}

// ---------------------------------------------------------------------------
// WebhookNotificationProvider — HTTP POST over the PAL ISocket transport
// ---------------------------------------------------------------------------

namespace {

// Builds the JSON body for one notification (docs/specs/14 §Notification Object).
json::Value NotificationToJson(const Notification& n) {
    json::Value::Object o;
    o["id"] = json::Value::Number(static_cast<double>(n.id));
    if (!n.correlationId.empty()) o["correlationId"] = json::Value::String(n.correlationId);
    o["timestampMs"] = json::Value::Number(static_cast<double>(n.timestampMs));
    if (!n.sourceModule.empty()) o["sourceModule"] = json::Value::String(n.sourceModule);
    if (!n.sourceEvent.empty()) o["sourceEvent"] = json::Value::String(n.sourceEvent);
    o["severity"] = json::Value::String(ToString(n.severity));
    o["category"] = json::Value::String(n.category);
    o["priority"] = json::Value::String(ToString(n.priority));
    o["title"] = json::Value::String(n.title);
    o["message"] = json::Value::String(n.message);
    if (n.progress >= 0.0) o["progress"] = json::Value::Number(n.progress);
    if (!n.groupKey.empty()) o["groupKey"] = json::Value::String(n.groupKey);
    if (n.groupCount > 1) o["groupCount"] = json::Value::Number(static_cast<double>(n.groupCount));
    if (!n.payload.isNull()) o["payload"] = n.payload;
    if (!n.actions.empty()) {
        json::Value::Array acts;
        for (const auto& a : n.actions) {
            json::Value::Object ao;
            ao["id"] = json::Value::String(a.id);
            ao["label"] = json::Value::String(a.label);
            acts.emplace_back(std::move(ao));
        }
        o["actions"] = json::Value(std::move(acts));
    }
    return json::Value(std::move(o));
}

} // namespace

Result<void> WebhookNotificationProvider::Configure(const json::Value& cfg) {
    // Expected: { "url": "http://host:port/path" }
    if (cfg.type() != json::Value::Type::Object) return Ok();   // no config = disabled
    const auto* url = cfg.Find("url");
    if (!url || url->type() != json::Value::Type::String) return Ok();
    std::string u(url->asString());
    if (u.rfind("http://", 0) != 0)
        return Error::Make(Err::InvalidArgument, "Webhook",
                           "only http:// URLs are supported (https is a future backend)");
    std::string rest = u.substr(7);
    size_t slash = rest.find('/');
    std::string authority = slash == std::string::npos ? rest : rest.substr(0, slash);
    std::string path = slash == std::string::npos ? "/" : rest.substr(slash);
    if (path.empty()) path = "/";
    size_t colon = authority.rfind(':');
    std::string host = authority;
    uint16_t port = 80;
    if (colon != std::string::npos) {
        host = authority.substr(0, colon);
        try {
            port = static_cast<uint16_t>(std::stoi(authority.substr(colon + 1)));
        } catch (...) {
            return Error::Make(Err::InvalidArgument, "Webhook", "bad port in URL");
        }
    }
    if (host.empty() || port == 0)
        return Error::Make(Err::InvalidArgument, "Webhook", "bad URL: " + u);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        host_ = std::move(host);
        port_ = port;
        path_ = std::move(path);
    }
    return Ok();
}

void WebhookNotificationProvider::SetEndpoint(std::string host, uint16_t port,
                                              std::string path) {
    std::lock_guard<std::mutex> lock(mutex_);
    host_ = std::move(host);
    port_ = port;
    path_ = std::move(path);
}

Result<void> WebhookNotificationProvider::Show(const Notification& n) {
    // Synchronous HTTP POST (opt-in via config only). Delivery is fire-and-forget
    // from the pipeline's perspective — a slow/unreachable endpoint costs the
    // caller a bounded connect + 5 s receive timeout at most, never blocks the
    // notification queue, and is counted in `failed_` rather than retried here.
    // A fully async dispatch can ride the TaskScheduler in a later iteration.
    std::string host, path;
    uint16_t port = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        host = host_;
        port = port_;
        path = path_;
    }
    if (port == 0)
        return Error::Make(Err::Unsupported, "Webhook", "no endpoint configured");

    std::string body = NotificationToJson(n).ToString();
    std::string request = std::format("POST {} HTTP/1.1\r\n", path) +
                          std::format("Host: {}:{}\r\n", host, port) +
                          "Content-Type: application/json\r\n" +
                          std::format("Content-Length: {}\r\n", body.size()) +
                          "User-Agent: BelieversPresentationSoftware/1.0\r\n" +
                          "Connection: close\r\n\r\n" + body;

    auto& sockets = platform::PlatformAccessor::Get().Sockets();
    auto conn = sockets.Connect(host, port);
    if (!conn.ok()) {
        std::lock_guard<std::mutex> lock(mutex_);
        ++failed_;
        return Error::Make(Err::IoError, "Webhook",
                           "connect failed: " + conn.error().message);
    }
    auto sent = conn.value()->Send(request);
    // Capture the request regardless of send outcome (inspection hook).
    {
        std::lock_guard<std::mutex> lock(mutex_);
        lastRequest_ = request;
    }
    if (!sent.ok() || sent.value() != request.size()) {
        std::lock_guard<std::mutex> lock(mutex_);
        ++failed_;
        return Error::Make(Err::IoError, "Webhook", "send failed");
    }
    // Read the response line to confirm the server accepted it (best effort).
    char buf[256] = {};
    auto got = conn.value()->ReceiveTimeout(buf, sizeof buf - 1, 5000);
    conn.value()->Close();
    if (got.ok()) {
        std::string resp(buf, got.value());
        bool ok2xx = resp.starts_with("HTTP/1.1 2") || resp.starts_with("HTTP/1.0 2");
        if (!ok2xx) {
            std::lock_guard<std::mutex> lock(mutex_);
            ++failed_;
            return Error::Make(Err::IoError, "Webhook",
                               "server rejected: " + resp.substr(0, 40));
        }
    }
    std::lock_guard<std::mutex> lock(mutex_);
    ++delivered_;
    return Ok();
}

std::string WebhookNotificationProvider::LastRequest() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return lastRequest_;
}

size_t WebhookNotificationProvider::Delivered() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return delivered_;
}

size_t WebhookNotificationProvider::Failed() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return failed_;
}

// ---------------------------------------------------------------------------
// RecordingNotificationProvider (tests / automation)
// ---------------------------------------------------------------------------

Result<void> RecordingNotificationProvider::Show(const Notification& n) {
    std::lock_guard<std::mutex> lock(mutex_);
    seen_.push_back(n);
    return Ok();
}

Result<void> RecordingNotificationProvider::Update(const Notification& n) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& e : seen_) {
        if (e.id == n.id) {
            e = n;
            break;
        }
    }
    seen_.push_back(n);   // also record the update event
    return Ok();
}

Result<void> RecordingNotificationProvider::Dismiss(uint64_t id) {
    std::lock_guard<std::mutex> lock(mutex_);
    dismissed_.push_back(id);
    return Ok();
}

size_t RecordingNotificationProvider::Count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return seen_.size();
}

std::vector<Notification> RecordingNotificationProvider::All() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return seen_;
}

Notification RecordingNotificationProvider::Last() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return seen_.empty() ? Notification{} : seen_.back();
}

void RecordingNotificationProvider::Clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    seen_.clear();
    dismissed_.clear();
}

} // namespace bps::notification
