#include "listen_socket.h"

#include "scoped_socket.h"

#include <cstdint>
#include <limits>
#include <utility>

#if defined(_WIN32)
using TqSockLen = int;
#else
#include <cerrno>
#include <fcntl.h>
#include <netdb.h>
using TqSockLen = socklen_t;
#endif

namespace {

struct TqParsedHostPort {
    std::string Host;
    uint16_t Port{};
};

bool TqParsePortAllowZero(const std::string& text, uint16_t& port) {
    if (text.empty()) {
        return false;
    }

    unsigned long value = 0;
    for (char ch : text) {
        if (ch < '0' || ch > '9') {
            return false;
        }
        value = (value * 10) + static_cast<unsigned long>(ch - '0');
        if (value > std::numeric_limits<uint16_t>::max()) {
            return false;
        }
    }

    port = static_cast<uint16_t>(value);
    return true;
}

bool TqParseHostPortAllowZero(const std::string& target, TqParsedHostPort& out) {
    if (target.empty() || target.find("://") != std::string::npos) {
        return false;
    }

    std::string host;
    std::string portText;
    if (target.front() == '[') {
        const size_t close = target.find(']');
        if (close == std::string::npos || close + 2 > target.size() || target[close + 1] != ':') {
            return false;
        }
        host = target.substr(1, close - 1);
        portText = target.substr(close + 2);
    } else {
        const size_t colon = target.rfind(':');
        if (colon == std::string::npos || colon + 1 >= target.size()) {
            return false;
        }
        host = target.substr(0, colon);
        portText = target.substr(colon + 1);
        if (host.find(':') != std::string::npos) {
            return false;
        }
    }

    uint16_t port = 0;
    if (!TqParsePortAllowZero(portText, port)) {
        return false;
    }

    out.Host = std::move(host);
    out.Port = port;
    return true;
}

#if !defined(_WIN32)
bool TqSetCloseOnExec(TqSocketHandle socket) {
    const int fdFlags = ::fcntl(socket, F_GETFD, 0);
    return fdFlags >= 0 && ::fcntl(socket, F_SETFD, fdFlags | FD_CLOEXEC) == 0;
}
#endif

std::string TqBoundAddressString(TqSocketHandle socket, const std::string& requestedHost) {
    sockaddr_storage storage{};
    TqSockLen length = sizeof(storage);
    if (::getsockname(socket, reinterpret_cast<sockaddr*>(&storage), &length) != 0) {
        return {};
    }

    char host[INET6_ADDRSTRLEN]{};
    uint16_t port = 0;
    if (storage.ss_family == AF_INET) {
        const auto* addr = reinterpret_cast<const sockaddr_in*>(&storage);
        if (TqInetNtop(AF_INET, &addr->sin_addr, host, sizeof(host)) == nullptr) {
            return {};
        }
        port = ntohs(addr->sin_port);
        return std::string(host) + ":" + std::to_string(port);
    }

    if (storage.ss_family == AF_INET6) {
        const auto* addr = reinterpret_cast<const sockaddr_in6*>(&storage);
        if (TqInetNtop(AF_INET6, &addr->sin6_addr, host, sizeof(host)) == nullptr) {
            return {};
        }
        port = ntohs(addr->sin6_port);
        return "[" + std::string(host) + "]:" + std::to_string(port);
    }

    if (!requestedHost.empty()) {
        return requestedHost + ":0";
    }
    return {};
}

bool TqPrepareListenSocket(TqSocketHandle socket) {
    if (!TqSetNonBlocking(socket)) {
        return false;
    }
#if !defined(_WIN32)
    if (!TqSetCloseOnExec(socket)) {
        return false;
    }
#endif
    return true;
}

} // namespace

bool TqCreateNonBlockingListenSocket(const std::string& listen, TqListenSocket& out) {
    TqParsedHostPort hostPort{};
    if (!TqParseHostPortAllowZero(listen, hostPort)) {
        return false;
    }

    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;

    addrinfo* result = nullptr;
    const std::string port = std::to_string(hostPort.Port);
    const int status = ::getaddrinfo(
        hostPort.Host.empty() ? nullptr : hostPort.Host.c_str(),
        port.c_str(),
        &hints,
        &result);
    if (status != 0) {
        return false;
    }

    for (addrinfo* ai = result; ai != nullptr; ai = ai->ai_next) {
        TqScopedSocket fd(::socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol));
        if (!TqSocketValid(fd.Get()) || !TqPrepareListenSocket(fd.Get())) {
            continue;
        }

        (void)TqSetReuseAddr(fd.Get());
        if (::bind(fd.Get(), ai->ai_addr, static_cast<TqSockLen>(ai->ai_addrlen)) == 0 &&
            ::listen(fd.Get(), SOMAXCONN) == 0) {
            std::string address = TqBoundAddressString(fd.Get(), hostPort.Host);
            if (address.empty()) {
                continue;
            }
            out.Fd = fd.Release();
            out.Address = std::move(address);
            ::freeaddrinfo(result);
            return true;
        }
    }

    ::freeaddrinfo(result);
    return false;
}
