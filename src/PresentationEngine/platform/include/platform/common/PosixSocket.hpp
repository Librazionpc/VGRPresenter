#pragma once

// POSIX socket backend of the PAL ISocket interfaces (Linux; also usable on
// macOS via SO_NOSIGPIPE). Lives in the PAL — the only place that includes
// <sys/socket.h>. Errors carry nativeError = errno.

#include "../ISocket.hpp"

#include <string>

namespace bps::platform {

class PosixSocket final : public ISocket {
public:
    explicit PosixSocket(int fd) : fd_(fd) {}
    ~PosixSocket() override { Close(); }

    Result<size_t> Send(std::string_view data) override;
    Result<size_t> Receive(void* buf, size_t maxBytes) override;
    Result<size_t> ReceiveTimeout(void* buf, size_t maxBytes,
                                  int timeoutMs) override;
    void Close() override;
    bool IsOpen() const noexcept override { return fd_ >= 0; }

private:
    int fd_ = -1;
};

class PosixListener final : public ISocketListener {
public:
    explicit PosixListener(int fd) : fd_(fd) {}
    ~PosixListener() override { Close(); }

    Result<std::unique_ptr<ISocket>> Accept(int timeoutMs) override;
    void Close() override;
    bool IsOpen() const noexcept override { return fd_ >= 0; }
    uint16_t Port() const noexcept override;

private:
    int fd_ = -1;
};

class PosixSocketFactory final : public ISocketFactory {
public:
    Result<std::unique_ptr<ISocket>> Connect(std::string host,
                                             uint16_t port) override;
    Result<std::unique_ptr<ISocketListener>> Listen(uint16_t port) override;
};

} // namespace bps::platform
