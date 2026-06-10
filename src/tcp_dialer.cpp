#include "tcp_dialer.h"

#include "platform_socket.h"
#include "tuning.h"

#include <cerrno>
#include <cstring>

#if !defined(_WIN32)
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>
#endif

namespace {

socklen_t TqSockaddrLength(const sockaddr_storage& addr) {
    switch (addr.ss_family) {
    case AF_INET:
        return sizeof(sockaddr_in);
    case AF_INET6:
        return sizeof(sockaddr_in6);
    default:
        return 0;
    }
}

bool TqSetBlocking(TqSocketHandle socket) {
#if defined(_WIN32)
    u_long mode = 0;
    return ::ioctlsocket(socket, FIONBIO, &mode) == 0;
#else
    const int flags = ::fcntl(socket, F_GETFL, 0);
    return flags >= 0 && ::fcntl(socket, F_SETFL, flags & ~O_NONBLOCK) == 0;
#endif
}

bool TqFinishConnect(TqSocketHandle fd, int timeoutMs, int& connectErrno) {
#if defined(_WIN32)
    WSAPOLLFD pfd{};
    pfd.fd = fd;
    pfd.events = POLLOUT;
    const int pollResult = ::WSAPoll(&pfd, 1, timeoutMs);
    if (pollResult <= 0) {
        connectErrno = (pollResult == 0) ? WSAETIMEDOUT : TqLastSocketError();
        return false;
    }
    int error = 0;
    int errorLen = sizeof(error);
    if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&error), &errorLen) != 0) {
        connectErrno = TqLastSocketError();
        return false;
    }
#else
    pollfd pfd{};
    pfd.fd = fd;
    pfd.events = POLLOUT;
    int pollResult;
    do {
        pollResult = ::poll(&pfd, 1, timeoutMs);
    } while (pollResult < 0 && TqSocketInterrupted(TqLastSocketError()));
    if (pollResult <= 0) {
        connectErrno = (pollResult == 0) ? ETIMEDOUT : TqLastSocketError();
        return false;
    }
    int error = 0;
    socklen_t errorLen = sizeof(error);
    if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &errorLen) != 0) {
        connectErrno = TqLastSocketError();
        return false;
    }
#endif
    if (error != 0) {
        connectErrno = error;
        return false;
    }
    connectErrno = 0;
    return true;
}

void TqRecordDialFailure(int connectErrno, bool& refused, bool& timedOut) {
#if defined(_WIN32)
    if (connectErrno == WSAECONNREFUSED) {
        refused = true;
        return;
    }
    if (connectErrno == WSAETIMEDOUT || connectErrno == WSAEWOULDBLOCK) {
        timedOut = true;
    }
#else
    if (connectErrno == ECONNREFUSED) {
        refused = true;
        return;
    }
    if (connectErrno == ETIMEDOUT || connectErrno == EAGAIN) {
        timedOut = true;
    }
#endif
}

} // namespace

void TqTuneTcpForThroughput(TqSocketHandle fd, int bufferBytes) {
    if (!TqSocketValid(fd)) {
        return;
    }

    if (bufferBytes <= 0) {
        bufferBytes = TqGetActiveTcpSocketBuffer();
    }

    (void)TqSetNoDelay(fd);
    (void)TqSetSocketBuffer(fd, SO_RCVBUF, bufferBytes);
    (void)TqSetSocketBuffer(fd, SO_SNDBUF, bufferBytes);
}

TqDialResult TqDialTcp(const std::vector<sockaddr_storage>& addrs, int timeoutMs) {
    TqDialResult result{};
    if (addrs.empty() || timeoutMs < 0) {
        return result;
    }

    bool refused = false;
    bool timedOut = false;

    for (const auto& addr : addrs) {
        const socklen_t addrLen = TqSockaddrLength(addr);
        if (addrLen == 0) {
            continue;
        }

        TqSocketHandle fd = ::socket(addr.ss_family, SOCK_STREAM, IPPROTO_TCP);
        if (!TqSocketValid(fd)) {
            continue;
        }

        if (!TqSetNonBlocking(fd)) {
            TqCloseSocket(fd);
            continue;
        }

        int connectErrno = 0;
        bool connected = false;
        const int connectResult =
            ::connect(fd, reinterpret_cast<const sockaddr*>(&addr), addrLen);
        if (connectResult == 0) {
            connected = true;
        } else if (TqSocketInProgress(TqLastSocketError())) {
            connected = TqFinishConnect(fd, timeoutMs, connectErrno);
            if (!connected) {
                TqRecordDialFailure(connectErrno, refused, timedOut);
            }
        } else {
            connectErrno = TqLastSocketError();
            TqRecordDialFailure(connectErrno, refused, timedOut);
        }

        if (connected && TqSetBlocking(fd)) {
            TqTuneTcpForThroughput(fd);
            result.Fd = fd;
            return result;
        }

        TqCloseSocket(fd);
    }

    result.Refused = refused;
    result.TimedOut = timedOut || (!refused && !timedOut);
    return result;
}
