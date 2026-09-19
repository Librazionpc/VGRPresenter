#include "platform/windows/WindowsSocket.hpp"

#include <ws2tcpip.h>

#include <algorithm>
#include <cstring>

namespace bps::platform {

namespace {

Error SocketError(std::string_view module, std::string_view op, int e) {
    return Error::Make(Err::IoError, module,
                       std::string(op) + " failed: " + std::to_string(e));
}

} // namespace

// ---------------------------------------------------------------------------
// Winsock bootstrap (process-wide, ref-counted)
// ---------------------------------------------------------------------------
namespace {
struct WinsockState {
    int refs = 0;
};
WinsockState& Wsa() {
    static WinsockState s;
    return s;
}
} // namespace

void WindowsSocketFactory::EnsureWinsock() {
    if (Wsa().refs++ == 0) {
        WSADATA wsa{};
        (void)::WSAStartup(MAKEWORD(2, 2), &wsa);
    }
}

void WindowsSocketFactory::ReleaseWinsock() {
    if (Wsa().refs > 0 && --Wsa().refs == 0) {
        (void)::WSACleanup();
    }
}

// ---------------------------------------------------------------------------
// WindowsSocket
// ---------------------------------------------------------------------------
Result<size_t> WindowsSocket::Send(std::string_view data) {
    if (sock_ == INVALID_SOCKET)
        return Error::Make(Err::InvalidState, "WindowsSocket", "socket closed");
    // Chunk the send: Win32 send() takes an int length, and on a 32-bit build
    // size_t can exceed INT_MAX for large buffers.
    const size_t kMaxChunk = size_t(1) << 20;
    size_t sent = 0;
    while (sent < data.size()) {
        size_t chunk = std::min(data.size() - sent, kMaxChunk);
        int n = ::send(sock_, data.data() + sent, static_cast<int>(chunk), 0);
        if (n == SOCKET_ERROR) {
            int e = ::WSAGetLastError();
            if (e == WSAEINTR) continue;
            return SocketError("WindowsSocket", "send", e);
        }
        sent += static_cast<size_t>(n);
    }
    return sent;
}

Result<size_t> WindowsSocket::Receive(void* buf, size_t maxBytes) {
    if (sock_ == INVALID_SOCKET)
        return Error::Make(Err::InvalidState, "WindowsSocket", "socket closed");
    const size_t kMaxChunk = size_t(1) << 20;
    size_t want = std::min(maxBytes, kMaxChunk);
    while (true) {
        int n = ::recv(sock_, static_cast<char*>(buf), static_cast<int>(want), 0);
        if (n >= 0) return static_cast<size_t>(n);
        int e = ::WSAGetLastError();
        if (e == WSAEINTR) continue;
        return SocketError("WindowsSocket", "recv", e);
    }
}

Result<size_t> WindowsSocket::ReceiveTimeout(void* buf, size_t maxBytes, int timeoutMs) {
    if (sock_ == INVALID_SOCKET)
        return Error::Make(Err::InvalidState, "WindowsSocket", "socket closed");
    WSAPOLLFD pfd{};
    pfd.fd = sock_;
    pfd.events = POLLRDNORM;
    while (true) {
        int pr = ::WSAPoll(&pfd, 1, timeoutMs);
        if (pr > 0 && (pfd.revents & POLLRDNORM)) {
            return Receive(buf, maxBytes);
        }
        if (pr == 0) {
            return Error::Make(Err::Timeout, "WindowsSocket", "receive timed out");
        }
        int e = ::WSAGetLastError();
        if (e == WSAEINTR) continue;
        return SocketError("WindowsSocket", "poll", e);
    }
}

void WindowsSocket::Close() {
    if (sock_ != INVALID_SOCKET) {
        (void)::closesocket(sock_);
        sock_ = INVALID_SOCKET;
    }
}

// ---------------------------------------------------------------------------
// WindowsListener
// ---------------------------------------------------------------------------
Result<std::unique_ptr<ISocket>> WindowsListener::Accept(int timeoutMs) {
    if (sock_ == INVALID_SOCKET)
        return Error::Make(Err::InvalidState, "WindowsListener", "listener closed");
    WSAPOLLFD pfd{};
    pfd.fd = sock_;
    pfd.events = POLLRDNORM;
    while (true) {
        int pr = ::WSAPoll(&pfd, 1, timeoutMs);
        if (pr > 0 && (pfd.revents & POLLRDNORM)) {
            SOCKET c = ::accept(sock_, nullptr, nullptr);
            if (c != INVALID_SOCKET) {
                return std::unique_ptr<ISocket>(new WindowsSocket(c));
            }
            int e = ::WSAGetLastError();
            if (e == WSAEINTR || e == WSAEWOULDBLOCK) continue;
            return SocketError("WindowsListener", "accept", e);
        }
        if (pr == 0) {
            return Error::Make(Err::Timeout, "WindowsListener", "accept timed out");
        }
        int e = ::WSAGetLastError();
        if (e == WSAEINTR) continue;
        return SocketError("WindowsListener", "poll", e);
    }
}

void WindowsListener::Close() {
    if (sock_ != INVALID_SOCKET) {
        (void)::closesocket(sock_);
        sock_ = INVALID_SOCKET;
    }
}

uint16_t WindowsListener::Port() const noexcept {
    if (sock_ == INVALID_SOCKET) return 0;
    sockaddr_in actual{};
    int len = sizeof actual;
    if (::getsockname(sock_, reinterpret_cast<sockaddr*>(&actual), &len) != 0) return 0;
    return ntohs(actual.sin_port);
}

// ---------------------------------------------------------------------------
// WindowsSocketFactory
// ---------------------------------------------------------------------------
Result<std::unique_ptr<ISocket>> WindowsSocketFactory::Connect(std::string host, uint16_t port) {
    SOCKET s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) {
        int e = ::WSAGetLastError();
        return SocketError("WindowsSocketFactory", "socket", e);
    }
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (::inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1) {
        ::closesocket(s);
        return Error::Make(Err::InvalidArgument, "WindowsSocketFactory",
                           "invalid host: " + host);
    }
    if (::connect(s, reinterpret_cast<const sockaddr*>(&addr), sizeof addr) == SOCKET_ERROR) {
        int e = ::WSAGetLastError();
        ::closesocket(s);
        return SocketError("WindowsSocketFactory", "connect", e);
    }
    return std::unique_ptr<ISocket>(new WindowsSocket(s));
}

Result<std::unique_ptr<ISocketListener>> WindowsSocketFactory::Listen(uint16_t port) {
    SOCKET s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) {
        int e = ::WSAGetLastError();
        return SocketError("WindowsSocketFactory", "socket", e);
    }
    BOOL one = TRUE;
    (void)::setsockopt(s, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&one), sizeof one);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(port);
    if (::bind(s, reinterpret_cast<const sockaddr*>(&addr), sizeof addr) == SOCKET_ERROR) {
        int e = ::WSAGetLastError();
        ::closesocket(s);
        return SocketError("WindowsSocketFactory", "bind", e);
    }
    if (::listen(s, 16) == SOCKET_ERROR) {
        int e = ::WSAGetLastError();
        ::closesocket(s);
        return SocketError("WindowsSocketFactory", "listen", e);
    }
    return std::unique_ptr<ISocketListener>(new WindowsListener(s));
}

} // namespace bps::platform
