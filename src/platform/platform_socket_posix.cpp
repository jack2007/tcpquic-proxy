#include "platform_socket.h"

#include <arpa/inet.h>
#include <cerrno>
#include <fcntl.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

TqSocketStartup::TqSocketStartup() : Ok_(true) {}
TqSocketStartup::~TqSocketStartup() = default;

bool TqSocketValid(TqSocketHandle socket) {
    return socket >= 0;
}

void TqCloseSocket(TqSocketHandle socket) {
    if (TqSocketValid(socket)) {
        (void)::close(socket);
    }
}

void TqResetSocket(TqSocketHandle socket) {
    if (!TqSocketValid(socket)) {
        return;
    }
    linger resetLinger{};
    resetLinger.l_onoff = 1;
    resetLinger.l_linger = 0;
    (void)::setsockopt(socket, SOL_SOCKET, SO_LINGER, &resetLinger, sizeof(resetLinger));
    (void)::close(socket);
}

bool TqSetNonBlocking(TqSocketHandle socket) {
    const int flags = ::fcntl(socket, F_GETFL, 0);
    return flags >= 0 && ::fcntl(socket, F_SETFL, flags | O_NONBLOCK) == 0;
}

bool TqSetSocketBlocking(TqSocketHandle socket) {
    const int flags = ::fcntl(socket, F_GETFL, 0);
    return flags >= 0 && ::fcntl(socket, F_SETFL, flags & ~O_NONBLOCK) == 0;
}

int TqLastSocketError() {
    return errno;
}

bool TqSocketWouldBlock(int error) {
    return error == EAGAIN || error == EWOULDBLOCK;
}

bool TqSocketInProgress(int error) {
    return error == EINPROGRESS;
}

bool TqSocketInterrupted(int error) {
    return error == EINTR;
}

bool TqSocketConnectionRefused(int error) {
    return error == ECONNREFUSED;
}

bool TqSocketTimeoutLike(int error) {
    return error == ETIMEDOUT || error == EHOSTUNREACH || error == ENETUNREACH ||
        error == EAGAIN || error == EWOULDBLOCK;
}

int TqConnect(TqSocketHandle socket, const sockaddr* address, socklen_t addressLength) {
    return ::connect(socket, address, addressLength);
}

bool TqGetSocketError(TqSocketHandle socket, int& error) {
    socklen_t length = sizeof(error);
    return ::getsockopt(socket, SOL_SOCKET, SO_ERROR, &error, &length) == 0;
}

int TqSend(TqSocketHandle socket, const void* data, size_t length, TqSendFlags flags) {
    int nativeFlags = 0;
#ifdef MSG_NOSIGNAL
    if (flags == TqSendFlags::NoSignal) {
        nativeFlags |= MSG_NOSIGNAL;
    }
#elif defined(SO_NOSIGPIPE)
    if (flags == TqSendFlags::NoSignal) {
        int enabled = 1;
        (void)::setsockopt(socket, SOL_SOCKET, SO_NOSIGPIPE, &enabled, sizeof(enabled));
    }
#else
    (void)flags;
#endif
    return static_cast<int>(::send(socket, data, length, nativeFlags));
}

int TqRecv(TqSocketHandle socket, void* data, size_t length, TqRecvFlags flags) {
    int nativeFlags = 0;
#ifdef MSG_DONTWAIT
    if (flags == TqRecvFlags::DontWait) {
        nativeFlags |= MSG_DONTWAIT;
    }
#else
    (void)flags;
#endif
    return static_cast<int>(::recv(socket, data, length, nativeFlags));
}

bool TqShutdownSend(TqSocketHandle socket) {
    return ::shutdown(socket, SHUT_WR) == 0;
}

bool TqShutdownBoth(TqSocketHandle socket) {
    return ::shutdown(socket, SHUT_RDWR) == 0;
}

bool TqInetPton(int family, const char* text, void* out) {
    return ::inet_pton(family, text, out) == 1;
}

const char* TqInetNtop(int family, const void* addr, char* text, size_t textLength) {
    return ::inet_ntop(family, addr, text, static_cast<socklen_t>(textLength));
}

bool TqSetReuseAddr(TqSocketHandle socket) {
    int enabled = 1;
    return ::setsockopt(socket, SOL_SOCKET, SO_REUSEADDR, &enabled, sizeof(enabled)) == 0;
}

bool TqSetNoDelay(TqSocketHandle socket) {
    int enabled = 1;
    return ::setsockopt(socket, IPPROTO_TCP, TCP_NODELAY, &enabled, sizeof(enabled)) == 0;
}

bool TqSetSocketBuffer(TqSocketHandle socket, int option, int bytes) {
    return bytes <= 0 || ::setsockopt(socket, SOL_SOCKET, option, &bytes, sizeof(bytes)) == 0;
}

int TqGetSocketBuffer(TqSocketHandle socket, int option) {
    int bytes = 0;
    socklen_t length = sizeof(bytes);
    if (::getsockopt(socket, SOL_SOCKET, option, &bytes, &length) != 0) {
        return -1;
    }
    return bytes;
}

bool TqSocketPair(TqSocketHandle out[2]) {
    out[0] = TqInvalidSocket;
    out[1] = TqInvalidSocket;
    return ::socketpair(AF_UNIX, SOCK_STREAM, 0, out) == 0;
}
