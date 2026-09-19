#pragma once

#include "core/common/Common.hpp"
#include "core/config/Json.hpp"
#include "platform/ISocket.hpp"

#include <memory>
#include <string>

namespace bps {

// Synchronous TCP JSON client for IpcServer (core/ipc/). One Call() = one
// request/response round trip; the connection stays open between calls.
// Transport is the PAL ISocket interface (platform/ISocket.hpp) — no OS socket
// code lives in the core (PAL DoD §1).
class IpcClient {
public:
    ~IpcClient() { Close(); }

    Result<void> Connect(std::string host, uint16_t port);
    Result<json::Value> Call(std::string_view method, const json::Value& params);
    Result<std::string> ReadNextLine();   // raw next line (log streaming mode)
    void Close();
    bool Connected() const noexcept { return socket_ && socket_->IsOpen(); }

private:
    // Reads the next newline-terminated line. `pending_` keeps bytes that
    // arrived in the same segment as an earlier line, so multiple lines per
    // Receive() are preserved (required for log-streaming mode).
    std::string ReadLine();

    std::unique_ptr<platform::ISocket> socket_;
    std::string pending_;
};

} // namespace bps
