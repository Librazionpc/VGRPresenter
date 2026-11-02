#pragma once

#include "core/common/Common.hpp"
#include "core/config/Json.hpp"
#include "interfaces/IService.hpp"
#include "platform/ISocket.hpp"

#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

namespace bps {
class IpcServer final : public IService {
public:
    static IpcServer& Instance();
    ~IpcServer() override;   // stops a still-running server (no-op when not running)

    Result<void> Initialize() override { return Ok(); }
    using IService::Start;
    Result<void> Start(uint16_t port);          // 0 = ephemeral (OS-assigned)
    Result<void> Stop();
    Result<void> Shutdown() override { return Stop(); }

    uint16_t Port() const noexcept { return port_.load(); }
    bool Running() const noexcept { return running_.load(); }
    uint64_t RequestsServed() const noexcept { return requests_.load(); }
    size_t HandlerCount() const;

    using Handler = std::function<json::Value(const json::Value& params)>;
    Result<void> RegisterHandler(std::string method, Handler h);

    Result<void> Broadcast(std::string_view jsonLine);
    size_t SubscriberCount() const;

    const char* ServiceName() const noexcept override { return "IpcServer"; }
    HealthReport GetHealth() const override;

private:
    IpcServer() = default;

    void AcceptLoop();
    void HandleConnection(std::shared_ptr<platform::ISocket> conn);
    void ProcessLine(const std::shared_ptr<platform::ISocket>& conn,
                     const std::string& line);
    void Subscribe(const std::shared_ptr<platform::ISocket>& conn);
    void Unsubscribe(platform::ISocket* conn);

    std::atomic<bool> running_{false};
    std::atomic<uint16_t> port_{0};
    std::atomic<uint64_t> requests_{0};
    std::unique_ptr<platform::ISocketListener> listener_;
    std::thread acceptThread_;
    std::mutex threadsMutex_;
    std::vector<std::thread> connThreads_;                 // guarded by threadsMutex_
    std::map<std::string, Handler> handlers_;              // guarded by handlersMutex_
    mutable std::mutex handlersMutex_;
    struct SocketPtrHash {
        size_t operator()(const std::shared_ptr<platform::ISocket>& p) const {
            return std::hash<platform::ISocket*>{}(p.get());
        }
    };
    struct SocketPtrEq {
        bool operator()(const std::shared_ptr<platform::ISocket>& a,
                        const std::shared_ptr<platform::ISocket>& b) const {
            return a.get() == b.get();
        }
    };
    std::unordered_set<std::shared_ptr<platform::ISocket>, SocketPtrHash, SocketPtrEq>
        subscribers_;
    mutable std::mutex subscribersMutex_;
};
} // namespace bps