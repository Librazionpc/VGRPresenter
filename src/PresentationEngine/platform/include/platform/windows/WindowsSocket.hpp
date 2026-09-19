#pragma once

// Winsock backend of the PAL ISocket interfaces (Windows). Lives in the PAL —
// the only place that includes <winsock2.h>. WSAStartup is performed once per
// process by the factory (ref-counted); errors carry nativeError = WSAGetLastError().

#include "../ISocket.hpp"

#include <winsock2.h>

#include <string>

namespace bps::platform {

class WindowsSocket final : public ISocket {
public:
    explicit WindowsSocket(SOCKET s) : sock_(s) {}
    ~WindowsSocket() override { Close(); }

    Result<size_t> Send(std::string_view data) override;
    Result<size_t> Receive(void* buf, size_t maxBytes) override;
    Result<size_t> ReceiveTimeout(void* buf, size_t maxBytes,
                                  int timeoutMs) override;
    void Close() override;
    bool IsOpen() const noexcept override { return sock_ != INVALID_SOCKET; }

private:
    SOCKET sock_ = INVALID_SOCKET;
};

class WindowsListener final : public ISocketListener {
public:
    explicit WindowsListener(SOCKET s) : sock_(s) {}
    ~WindowsListener() override { Close(); }

    Result<std::unique_ptr<ISocket>> Accept(int timeoutMs) override;
    void Close() override;
    bool IsOpen() const noexcept override { return sock_ != INVALID_SOCKET; }
    uint16_t Port() const noexcept override;

private:
    SOCKET sock_ = INVALID_SOCKET;
};

class WindowsSocketFactory final : public ISocketFactory {
public:
    WindowsSocketFactory() { EnsureWinsock(); }
    ~WindowsSocketFactory() override { ReleaseWinsock(); }

    Result<std::unique_ptr<ISocket>> Connect(std::string host,
                                             uint16_t port) override;
    Result<std::unique_ptr<ISocketListener>> Listen(uint16_t port) override;

private:
    // Winsock reference counting (WSAStartup/WSACleanup). Nested factories are
    // fine; the last release cleans up at process teardown.
    static void EnsureWinsock();
    static void ReleaseWinsock();
};

} // namespace bps::platform
