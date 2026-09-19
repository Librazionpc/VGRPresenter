#include "platform/common/PosixSocket.hpp"
#include "platform/OsTag.hpp"

#include <cerrno>
#include <cstring>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

namespace bps::platform {

namespace {

Error SocketError(std::string_view module, std::string_view op, int e) {
    Error err = Error::Make(Err::IoError, module,
                            std::string(op) + " failed: " + std::strerror(e));
    err.nativeError = e;
    err.stack = CaptureStack();   // debug builds only
    return err;
}

#if defined(__APPLE__)
// macOS has no MSG_NOSIGNAL; use SO_NOSIGPIPE instead (set once on connect).
constexpr int kSendFlags = 0;
#else
constexpr int kSendFlags = MSG_NOSIGNAL;
#endif

} // namespace

Result<size_t> PosixSocket::Send(std::string_view data) {
    if (fd_ < 0)
        return Error::Make(Err::InvalidState, "PosixSocket", "socket closed");
    size_t sent = 0;
    while (sent < data.size()) {
        ssize_t n = ::send(fd_, data.data() + sent, data.size() - sent, kSendFlags);
        if (n < 0) {
            if (errno == EINTR) continue;
            int e = errno;
            return SocketError("PosixSocket", "send", e);
        }
        sent += static_cast<size_t>(n);
    }
    return sent;
}

Result<size_t> PosixSocket::Receive(void* buf, size_t maxBytes) {
    if (fd_ < 0)
        return Error::Make(Err::InvalidState, "PosixSocket", "socket closed");
    while (true) {
        ssize_t n = ::recv(fd_, buf, maxBytes, 0);
        if (n >= 0) return static_cast<size_t>(n);
        if (errno == EINTR) continue;
        int e = errno;
        return SocketError("PosixSocket", "recv", e);
    }
}

Result<size_t> PosixSocket::ReceiveTimeout(void* buf, size_t maxBytes, int timeoutMs) {
    if (fd_ < 0)
        return Error::Make(Err::InvalidState, "PosixSocket", "socket closed");
    struct pollfd pfd { fd_, POLLIN, 0 };
    while (true) {
        int pr = ::poll(&pfd, 1, timeoutMs);
        if (pr > 0 && (pfd.revents & POLLIN)) {
            return Receive(buf, maxBytes);
        }
        if (pr == 0) {
            return Error::Make(Err::Timeout, "PosixSocket", "receive timed out");
        }
        if (errno == EINTR) continue;
        int e = errno;
        return SocketError("PosixSocket", "poll", e);
    }
}

void PosixSocket::Close() {
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
}

Result<std::unique_ptr<ISocket>> PosixListener::Accept(int timeoutMs) {
    if (fd_ < 0)
        return Error::Make(Err::InvalidState, "PosixListener", "listener closed");
    struct pollfd pfd { fd_, POLLIN, 0 };
    while (true) {
        int pr = ::poll(&pfd, 1, timeoutMs);
        if (pr > 0 && (pfd.revents & POLLIN)) {
            int cfd = ::accept(fd_, nullptr, nullptr);
            if (cfd >= 0) {
#if defined(__APPLE__)
                int one = 1;
                (void)::setsockopt(cfd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
                return std::unique_ptr<ISocket>(new PosixSocket(cfd));
            }
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) continue;
            int e = errno;
            return SocketError("PosixListener", "accept", e);
        }
        if (pr == 0) {
            return Error::Make(Err::Timeout, "PosixListener", "accept timed out");
        }
        if (errno == EINTR) continue;
        int e = errno;
        return SocketError("PosixListener", "poll", e);
    }
}

void PosixListener::Close() {
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
}

uint16_t PosixListener::Port() const noexcept {
    if (fd_ < 0) return 0;
    sockaddr_in actual{};
    socklen_t len = sizeof actual;
    if (::getsockname(fd_, reinterpret_cast<sockaddr*>(&actual), &len) != 0) return 0;
    return ntohs(actual.sin_port);
}

Result<std::unique_ptr<ISocket>> PosixSocketFactory::Connect(std::string host, uint16_t port) {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        int e = errno;
        return SocketError("PosixSocketFactory", "socket", e);
    }
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (::inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1) {
        ::close(fd);
        return Error::Make(Err::InvalidArgument, "PosixSocketFactory",
                           "invalid host: " + host);
    }
    if (::connect(fd, reinterpret_cast<const sockaddr*>(&addr), sizeof addr) != 0) {
        int e = errno;
        ::close(fd);
        return SocketError("PosixSocketFactory", "connect", e);
    }
#if defined(__APPLE__)
    int one = 1;
    (void)::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
    return std::unique_ptr<ISocket>(new PosixSocket(fd));
}

Result<std::unique_ptr<ISocketListener>> PosixSocketFactory::Listen(uint16_t port) {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        int e = errno;
        return SocketError("PosixSocketFactory", "socket", e);
    }
    int one = 1;
    (void)::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(port);
    if (::bind(fd, reinterpret_cast<const sockaddr*>(&addr), sizeof addr) != 0) {
        int e = errno;
        ::close(fd);
        return SocketError("PosixSocketFactory", "bind", e);
    }
    if (::listen(fd, 16) != 0) {
        int e = errno;
        ::close(fd);
        return SocketError("PosixSocketFactory", "listen", e);
    }
    return std::unique_ptr<ISocketListener>(new PosixListener(fd));
}

} // namespace bps::platform
