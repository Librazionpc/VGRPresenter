#pragma once

#include "core/common/Common.hpp"
#include "modules/rendering/RenderOutputs.hpp"

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace bps::platform { class ISocket; class ISocketListener; }

namespace bps::display {

// Local browser output. The page polls a PNG endpoint backed by the newest
// rendered frame, so a slow browser never blocks the render thread.
class HtmlOutputServer {
public:
    HtmlOutputServer() = default;
    ~HtmlOutputServer();
    HtmlOutputServer(const HtmlOutputServer&) = delete;
    HtmlOutputServer& operator=(const HtmlOutputServer&) = delete;

    Result<void> Start(uint16_t port = 0);
    void Stop();
    bool Running() const noexcept { return running_.load(std::memory_order_acquire); }
    std::string Url() const;
    void PublishFrame(const rendering::Frame& frame);

private:
    void Serve();
    void ServeClient(std::unique_ptr<platform::ISocket> socket);

    std::atomic<bool> running_{false};
    std::unique_ptr<platform::ISocketListener> listener_;
    std::thread worker_;
    std::atomic<uint16_t> port_{0};
    mutable std::mutex frameMutex_;
    std::shared_ptr<const rendering::Frame> latestFrame_;
};

} // namespace bps::display
