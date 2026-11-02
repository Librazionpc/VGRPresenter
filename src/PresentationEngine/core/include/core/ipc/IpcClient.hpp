#pragma once

#include "core/common/Common.hpp"
#include "core/config/Json.hpp"
#include "platform/ISocket.hpp"

#include <memory>
#include <string>

namespace bps {
class IpcClient {
public:
    ~IpcClient() { Close(); }

    Result<void> Connect(std::string host, uint16_t port);
    Result<json::Value> Call(std::string_view method, const json::Value& params);
    Result<std::string> ReadNextLine();   // raw next line (log streaming mode)
    void Close();
    bool Connected() const noexcept { return socket_ && socket_->IsOpen(); }

private:
    std::string ReadLine();

    std::unique_ptr<platform::ISocket> socket_;
    std::string pending_;
};
} // namespace bps