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

bool TqSetNonBlocking(TqSocketHandle socket) {
    const int flags = ::fcntl(socket, F_GETFL, 0);
    return flags >= 0 && ::fcntl(socket, F_SETFL, flags | O_NONBLOCK) == 0;
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

int TqSend(TqSocketHandle socket, const void* data, size_t length, TqSendFlags flags) {
    int nativeFlags = 0;
#ifdef MSG_NOSIGNAL
    if (flags == TqSendFlags::NoSignal) {
        nativeFlags |= MSG_NOSIGNAL;
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

bool TqSocketPair(TqSocketHandle out[2]) {
    out[0] = TqInvalidSocket;
    out[1] = TqInvalidSocket;
    return ::socketpair(AF_UNIX, SOCK_STREAM, 0, out) == 0;
}
