#include "core/ipc/IpcServer.hpp"

#include "core/logging/Logger.hpp"
#include "platform/PlatformAccessor.hpp"

#include <cstring>
#include <string>
#include <format>

namespace bps {

namespace {

constexpr int kMaxLine = 64 * 1024;

long long RequestId(const json::Value& req) {
    const json::Value* id = req.Find("id");
    return id ? id->asInt(0) : 0;
}

json::Value Response(long long id, const json::Value& result, bool ok,
                     const std::string& errMsg = {}, long long errCode = 0) {
    json::Value::Object o;
    o["id"] = json::Value::Number(static_cast<double>(id));
    if (ok) {
        o["result"] = result;
    } else {
        json::Value::Object e;
        e["code"] = json::Value::Number(static_cast<double>(errCode));
        e["message"] = json::Value::String(errMsg);
        o["error"] = json::Value(std::move(e));
    }
    return json::Value(std::move(o));
}

bool SendAll(platform::ISocket& conn, const std::string& data) {
    auto r = conn.Send(data);
    return r.ok() && r.value() == data.size();
}

} // namespace

IpcServer& IpcServer::Instance() {
    static IpcServer instance;
    return instance;
}

IpcServer::~IpcServer() {
    (void)Stop();   // joins the accept thread; safe no-op when never started
}

Result<void> IpcServer::Start(uint16_t port) {
    if (running_.load()) return Ok();
    auto listen = platform::PlatformAccessor::Get().Sockets().Listen(port);
    if (!listen.ok()) return listen.error();
    listener_ = std::move(listen.value());
    port_.store(listener_->Port());
    running_.store(true);
    acceptThread_ = std::thread([this] { AcceptLoop(); });
    Logger::Instance().Info(std::format("IPC server listening on 127.0.0.1:{}", port_.load()),
                            "Ipc");
    return Ok();
}

void IpcServer::AcceptLoop() {
    while (running_.load()) {
        auto r = listener_->Accept(100);   // 100 ms poll -> Stop() can interrupt
        if (!r.ok()) {
            if (r.error().code == Err::Timeout) continue;
            if (r.error().code == Err::InvalidState) break;   // listener closed
            continue;
        }
        if (!running_.load()) break;
        std::lock_guard<std::mutex> lock(threadsMutex_);
        // Shared ownership: the connection thread, and any Broadcast in
        // flight, keep the socket alive until the thread exits.
        connThreads_.emplace_back(
            [this, conn = std::shared_ptr<platform::ISocket>(std::move(r.value()))]() mutable {
                HandleConnection(std::move(conn));
            });
    }
}

void IpcServer::HandleConnection(std::shared_ptr<platform::ISocket> conn) {
    platform::ISocket* sock = conn.get();
    std::string buf;
    char tmp[4096];
    while (running_.load()) {
        auto n = sock->ReceiveTimeout(tmp, sizeof tmp, 100);
        if (!n.ok()) {
            if (n.error().code == Err::Timeout) {
                if (!running_.load()) break;
                continue;
            }
            break;
        }
        if (n.value() == 0) break;   // client closed
        buf.append(tmp, static_cast<size_t>(n.value()));
        size_t pos = 0;
        while ((pos = buf.find('\n')) != std::string::npos) {
            std::string line = buf.substr(0, pos);
            buf.erase(0, pos + 1);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            requests_.fetch_add(1);
            ProcessLine(conn, line);
        }
        if (buf.size() > kMaxLine) break;
    }
    Unsubscribe(sock);   // a disconnected client stops receiving the log stream
    conn->Close();
}

void IpcServer::ProcessLine(const std::shared_ptr<platform::ISocket>& conn,
                            const std::string& line) {
    platform::ISocket& sock = *conn;
    auto req = json::Parse(line);
    if (!req.ok()) {
        (void)SendAll(sock, Response(0, json::Value::Null(), false, req.error().message,
                                     static_cast<long long>(req.error().code))
                                .ToString() + "\n");
        return;
    }
    const json::Value& r = req.value();
    long long id = RequestId(r);
    std::string method(r.Find("method") ? std::string(r.Find("method")->asString())
                                        : std::string());
    json::Value params = r.Find("params") ? *r.Find("params") : json::Value::Object{};

    // Remote log streaming (02 §3): subscribe/unsubscribe are server-internal
    // (they need the connection socket, which a Handler cannot see).
    if (method == "log.subscribe") {
        Subscribe(conn);
        (void)SendAll(sock, Response(id, json::Value::Object{{{"subscribed",
                                                               json::Value::Number(1)}}},
                                     true)
                                .ToString() + "\n");
        return;
    }
    if (method == "log.unsubscribe") {
        Unsubscribe(conn.get());
        (void)SendAll(sock, Response(id, json::Value::Object{{{"unsubscribed",
                                                               json::Value::Number(1)}}},
                                     true)
                                .ToString() + "\n");
        return;
    }

    Handler h;
    {
        std::lock_guard<std::mutex> lock(handlersMutex_);
        auto it = handlers_.find(method);
        if (it == handlers_.end()) {
            (void)SendAll(sock, Response(id, json::Value::Null(), false,
                                         "unknown method: " + method,
                                         static_cast<long long>(Err::NotFound))
                                    .ToString() + "\n");
            return;
        }
        h = it->second;
    }
    json::Value result;
    try {
        result = h(params);
    } catch (const std::exception& ex) {
        (void)SendAll(sock, Response(id, json::Value::Null(), false, ex.what(),
                                     static_cast<long long>(Err::InvalidState))
                                .ToString() + "\n");
        return;
    }
    (void)SendAll(sock, Response(id, result, true).ToString() + "\n");
}

Result<void> IpcServer::Stop() {
    if (!running_.exchange(false)) return Ok();
    if (listener_) {
        listener_->Close();
        listener_.reset();
    }
    if (acceptThread_.joinable()) acceptThread_.join();
    {
        std::lock_guard<std::mutex> lock(threadsMutex_);
        for (auto& t : connThreads_)
            if (t.joinable()) t.join();
        connThreads_.clear();
    }
    Logger::Instance().Info("IPC server stopped", "Ipc");
    return Ok();
}

void IpcServer::Subscribe(const std::shared_ptr<platform::ISocket>& conn) {
    std::lock_guard<std::mutex> lock(subscribersMutex_);
    subscribers_.insert(conn);
}

void IpcServer::Unsubscribe(platform::ISocket* conn) {
    std::lock_guard<std::mutex> lock(subscribersMutex_);
    for (auto it = subscribers_.begin(); it != subscribers_.end(); ++it) {
        if (it->get() == conn) {
            subscribers_.erase(it);
            return;
        }
    }
}

Result<void> IpcServer::Broadcast(std::string_view jsonLine) {
    if (!running_.load()) return Ok();
    std::string line = std::string(jsonLine) + "\n";
    std::vector<std::shared_ptr<platform::ISocket>> targets;
    {
        std::lock_guard<std::mutex> lock(subscribersMutex_);
        targets.assign(subscribers_.begin(), subscribers_.end());
    }
    // Send OUTSIDE the lock with shared_ptrs held: SendAll blocks on a
    // slow/stalled subscriber, and Broadcast runs on the Logger pump thread — a
    // stuck client must not stall logging nor block
    // Subscribe/Unsubscribe/SubscriberCount. Holding the shared_ptr keeps the
    // socket alive even if its connection thread is tearing down concurrently.
    std::vector<platform::ISocket*> dead;
    for (const auto& s : targets)
        if (!SendAll(*s, line)) dead.push_back(s.get());
    for (platform::ISocket* s : dead) Unsubscribe(s);   // drop broken subscribers
    return Ok();
}

size_t IpcServer::SubscriberCount() const {
    std::lock_guard<std::mutex> lock(subscribersMutex_);
    return subscribers_.size();
}

Result<void> IpcServer::RegisterHandler(std::string method, Handler h) {
    if (method.empty() || !h)
        return Error::Make(Err::InvalidArgument, "IpcServer",
                           "method and handler required");
    std::lock_guard<std::mutex> lock(handlersMutex_);
    if (handlers_.count(method))
        return Error::Make(Err::AlreadyExists, "IpcServer",
                           "handler already registered: " + method);
    handlers_[std::move(method)] = std::move(h);
    return Ok();
}

size_t IpcServer::HandlerCount() const {
    std::lock_guard<std::mutex> lock(handlersMutex_);
    return handlers_.size();
}

HealthReport IpcServer::GetHealth() const {
    HealthReport r;
    r.state = running_.load() ? HealthState::Healthy : HealthState::Degraded;
    r.detail = std::format("port={} handlers={} served={}", port_.load(), HandlerCount(),
                           requests_.load());
    return r;
}

} // namespace bps
