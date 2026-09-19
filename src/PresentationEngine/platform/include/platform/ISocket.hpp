#pragma once

// PAL socket subsystem (Phase 2): the only socket API the engine core touches.
// core/ipc (IpcServer/IpcClient) and future transports (WebSocket, gRPC) build
// on this interface; POSIX and Winsock implementations live under
// platform/common (PosixSocket) and platform/windows (WindowsSocket).
//
// Threading model: sockets are NOT thread-safe — each ISocket / ISocketListener
// is owned by a single thread (a connection thread on the server, the calling
// thread on the client). Receive()/Accept() block with a caller-supplied
// timeout so owning threads can poll and observe stop flags.

#include "core/common/Common.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

namespace bps::platform {

// A connected TCP stream socket (AF_INET).
class ISocket {
public:
    virtual ~ISocket() = default;

    // Sends ALL of `data` (loops on short writes / EINTR). Returns bytes sent
    // (= data.size()) or an Error (Err::IoError with nativeError set).
    virtual Result<size_t> Send(std::string_view data) = 0;

    // Blocks until data arrives or the peer closes. Returns 0 on orderly
    // close; a positive count of bytes written into buf otherwise.
    virtual Result<size_t> Receive(void* buf, size_t maxBytes) = 0;

    // Receive bounded by timeoutMs. Err::Timeout on expiry (no data); 0 on
    // orderly close.
    virtual Result<size_t> ReceiveTimeout(void* buf, size_t maxBytes,
                                          int timeoutMs) = 0;

    virtual void Close() = 0;
    virtual bool IsOpen() const noexcept = 0;
};

// A TCP listening socket (loopback). Accept(timeoutMs) is poll-based so a
// server accept-loop can wake every timeoutMs and check its stop flag.
class ISocketListener {
public:
    virtual ~ISocketListener() = default;

    // Returns a connected ISocket or Err::Timeout when nothing arrived within
    // timeoutMs. On a fatal error returns Err::IoError.
    virtual Result<std::unique_ptr<ISocket>> Accept(int timeoutMs) = 0;

    virtual void Close() = 0;
    virtual bool IsOpen() const noexcept = 0;

    // The actual bound port (useful when 0 = ephemeral was requested).
    virtual uint16_t Port() const noexcept = 0;
};

// Creates client connections and loopback listeners.
class ISocketFactory {
public:
    virtual ~ISocketFactory() = default;

    virtual Result<std::unique_ptr<ISocket>> Connect(std::string host,
                                                     uint16_t port) = 0;
    virtual Result<std::unique_ptr<ISocketListener>> Listen(uint16_t port) = 0;
};

} // namespace bps::platform
