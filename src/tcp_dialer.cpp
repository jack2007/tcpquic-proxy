#include "tcp_dialer.h"
#include "tuning.h"

#include <cerrno>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <unistd.h>

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

bool TqSetNonBlocking(int fd, bool enabled, int& originalFlags) {
    const int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) {
        return false;
    }
    originalFlags = flags;

    const int newFlags = enabled ? (flags | O_NONBLOCK) : flags;
    return fcntl(fd, F_SETFL, newFlags) == 0;
}

bool TqFinishConnect(int fd, int timeoutMs, int& connectErrno) {
    pollfd pfd{};
    pfd.fd = fd;
    pfd.events = POLLOUT;

    int pollResult;
    do {
        pollResult = poll(&pfd, 1, timeoutMs);
    } while (pollResult < 0 && errno == EINTR);

    if (pollResult == 0) {
        connectErrno = ETIMEDOUT;
        return false;
    }

    if (pollResult < 0) {
        connectErrno = errno;
        return false;
    }

    int error = 0;
    socklen_t errorLen = sizeof(error);
    if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &errorLen) != 0) {
        connectErrno = errno;
        return false;
    }

    if (error != 0) {
        connectErrno = error;
        return false;
    }

    connectErrno = 0;
    return true;
}

void TqRecordDialFailure(int connectErrno, bool& refused, bool& timedOut) {
    if (connectErrno == ECONNREFUSED) {
        refused = true;
        return;
    }
    if (connectErrno == ETIMEDOUT || connectErrno == EAGAIN) {
        timedOut = true;
    }
}

} // namespace

void TqTuneTcpForThroughput(int fd, int bufferBytes) {
    if (fd < 0) {
        return;
    }

    if (bufferBytes <= 0) {
        bufferBytes = TqGetActiveTcpSocketBuffer();
    }

    const int yes = 1;
    (void)setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &yes, sizeof(yes));

    (void)setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &bufferBytes, sizeof(bufferBytes));
    (void)setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &bufferBytes, sizeof(bufferBytes));
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

        const int fd = socket(addr.ss_family, SOCK_STREAM, IPPROTO_TCP);
        if (fd < 0) {
            continue;
        }

        int originalFlags = 0;
        if (!TqSetNonBlocking(fd, true, originalFlags)) {
            close(fd);
            continue;
        }

        int connectErrno = 0;
        bool connected = false;
        if (connect(fd, reinterpret_cast<const sockaddr*>(&addr), addrLen) == 0) {
            connected = true;
        } else if (errno == EINPROGRESS) {
            connected = TqFinishConnect(fd, timeoutMs, connectErrno);
            if (!connected) {
                TqRecordDialFailure(connectErrno, refused, timedOut);
            }
        } else {
            connectErrno = errno;
            TqRecordDialFailure(connectErrno, refused, timedOut);
        }

        if (connected && fcntl(fd, F_SETFL, originalFlags) == 0) {
            TqTuneTcpForThroughput(fd);
            result.Fd = fd;
            return result;
        }

        close(fd);
    }

    result.Refused = refused;
    result.TimedOut = timedOut || (!refused && !timedOut);
    return result;
}
