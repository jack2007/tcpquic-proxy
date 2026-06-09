#include "http_connect_server.h"

#include "platform_socket.h"
#include "tcp_dialer.h"
#include "thread_pool.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string>
#include <utility>

#if !defined(_WIN32)
#include <netdb.h>
#endif

namespace {

constexpr size_t TqMaxHttpHeaderBytes = 16 * 1024;

struct TqHostPort {
    std::string Host;
    uint16_t Port{};
};

bool TqParsePort(const std::string& text, uint16_t& port) {
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

    if (value == 0) {
        return false;
    }

    port = static_cast<uint16_t>(value);
    return true;
}

bool TqParseHostPort(const std::string& target, TqHostPort& out) {
    if (target.empty() || target.find("://") != std::string::npos) {
        return false;
    }

    std::string host;
    std::string portText;
    if (target.front() == '[') {
        const size_t close = target.find(']');
        if (close == std::string::npos || close + 1 >= target.size() || target[close + 1] != ':') {
            return false;
        }
        host = target.substr(1, close - 1);
        portText = target.substr(close + 2);
    } else {
        const size_t colon = target.rfind(':');
        if (colon == std::string::npos || colon == 0 || colon + 1 >= target.size()) {
            return false;
        }
        host = target.substr(0, colon);
        portText = target.substr(colon + 1);
        if (host.find(':') != std::string::npos) {
            return false; // IPv6 literals must use [addr]:port form.
        }
    }

    uint16_t port = 0;
    if (host.empty() || !TqParsePort(portText, port)) {
        return false;
    }

    out.Host = host;
    out.Port = port;
    return true;
}

uint8_t TqAddrTypeForHost(const std::string& host) {
    in_addr ipv4{};
    if (TqInetPton(AF_INET, host.c_str(), &ipv4)) {
        return TQ_ADDR_IPV4;
    }

    in6_addr ipv6{};
    if (TqInetPton(AF_INET6, host.c_str(), &ipv6)) {
        return TQ_ADDR_IPV6;
    }

    return TQ_ADDR_DOMAIN;
}

bool TqSendAll(TqSocketHandle fd, const char* data, size_t length) {
    size_t sent = 0;
    while (sent < length) {
        const int result = TqSend(fd, data + sent, length - sent, TqSendFlags::None);
        if (result < 0) {
            if (TqSocketInterrupted(TqLastSocketError())) {
                continue;
            }
            return false;
        }
        if (result == 0) {
            return false;
        }
        sent += static_cast<size_t>(result);
    }
    return true;
}

bool TqSendHttpStatus(TqSocketHandle fd, int status) {
    const char* reason = "Internal Server Error";
    switch (status) {
    case 200:
        reason = "Connection Established";
        break;
    case 400:
        reason = "Bad Request";
        break;
    case 403:
        reason = "Forbidden";
        break;
    case 502:
        reason = "Bad Gateway";
        break;
    case 504:
        reason = "Gateway Timeout";
        break;
    default:
        status = 500;
        reason = "Internal Server Error";
        break;
    }

    char response[128];
    const int len = std::snprintf(
        response,
        sizeof(response),
        "HTTP/1.1 %d %s\r\n\r\n",
        status,
        reason);
    if (len <= 0 || static_cast<size_t>(len) >= sizeof(response)) {
        return false;
    }
    return TqSendAll(fd, response, static_cast<size_t>(len));
}

void TqHandleHttpConnectClient(TqSocketHandle clientFd, const TunnelStartFn& onTunnel) {
    std::string request;
    request.reserve(1024);

    char buffer[1024];
    while (request.find("\r\n\r\n") == std::string::npos) {
        if (request.size() >= TqMaxHttpHeaderBytes) {
            TqSendHttpStatus(clientFd, 400);
            TqCloseSocket(clientFd);
            return;
        }

        const int received = TqRecv(clientFd, buffer, sizeof(buffer), TqRecvFlags::None);
        if (received < 0) {
            if (TqSocketInterrupted(TqLastSocketError())) {
                continue;
            }
            TqCloseSocket(clientFd);
            return;
        }
        if (received == 0) {
            TqCloseSocket(clientFd);
            return;
        }

        request.append(buffer, static_cast<size_t>(received));
    }

    TunnelRequest tunnel{};
    if (!TqParseHttpConnectRequest(request, tunnel)) {
        TqSendHttpStatus(clientFd, 400);
        TqCloseSocket(clientFd);
        return;
    }

    if (!onTunnel) {
        TqSendHttpStatus(clientFd, 500);
        TqCloseSocket(clientFd);
        return;
    }

    TqTuneTcpForThroughput(clientFd);

    if (!TqSendHttpStatus(clientFd, 200)) {
        TqCloseSocket(clientFd);
        return;
    }

    const TqTunnelStartResult result = onTunnel(tunnel, clientFd);
    if (!result.Ok) {
        TqCloseSocket(clientFd);
        return;
    }
}

bool TqCreateListenSocket(const std::string& listenHostPort, TqSocketHandle& listenFd) {
    TqHostPort hostPort{};
    if (!TqParseHostPort(listenHostPort, hostPort)) {
        return false;
    }

    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;

    addrinfo* result = nullptr;
    const std::string port = std::to_string(hostPort.Port);
    const int status = getaddrinfo(
        hostPort.Host.empty() ? nullptr : hostPort.Host.c_str(),
        port.c_str(),
        &hints,
        &result);
    if (status != 0) {
        return false;
    }

    for (addrinfo* ai = result; ai != nullptr; ai = ai->ai_next) {
        TqSocketHandle fd = ::socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (!TqSocketValid(fd)) {
            continue;
        }

        (void)TqSetReuseAddr(fd);

        if (::bind(fd, ai->ai_addr, static_cast<int>(ai->ai_addrlen)) == 0 &&
            ::listen(fd, SOMAXCONN) == 0) {
            listenFd = fd;
            freeaddrinfo(result);
            return true;
        }

        TqCloseSocket(fd);
    }

    freeaddrinfo(result);
    return false;
}

} // namespace

int TqHttpStatusForOpenError(TqOpenError error) {
    switch (error) {
    case TqOpenError::Ok:
        return 200;
    case TqOpenError::AclDenied:
        return 403;
    case TqOpenError::DnsFailed:
    case TqOpenError::TcpRefused:
        return 502;
    case TqOpenError::TcpTimeout:
        return 504;
    case TqOpenError::Internal:
    default:
        return 500;
    }
}

bool TqParseHttpConnectRequest(const std::string& request, TunnelRequest& out) {
    const size_t headerEnd = request.find("\r\n\r\n");
    if (headerEnd == std::string::npos) {
        return false;
    }

    const size_t lineEnd = request.find("\r\n");
    if (lineEnd == std::string::npos || lineEnd == 0 || lineEnd > headerEnd) {
        return false;
    }

    const std::string line = request.substr(0, lineEnd);
    const size_t methodEnd = line.find(' ');
    if (methodEnd == std::string::npos) {
        return false;
    }
    const size_t targetEnd = line.find(' ', methodEnd + 1);
    if (targetEnd == std::string::npos || line.find(' ', targetEnd + 1) != std::string::npos) {
        return false;
    }

    const std::string method = line.substr(0, methodEnd);
    const std::string target = line.substr(methodEnd + 1, targetEnd - methodEnd - 1);
    const std::string version = line.substr(targetEnd + 1);
    if (method != "CONNECT" || version.rfind("HTTP/1.", 0) != 0 || version.size() != 8) {
        return false;
    }

    TqHostPort hostPort{};
    if (!TqParseHostPort(target, hostPort) || hostPort.Host.size() >= sizeof(out.Host)) {
        return false;
    }

    TunnelRequest parsed{};
    parsed.AddrType = TqAddrTypeForHost(hostPort.Host);
    std::memcpy(parsed.Host, hostPort.Host.c_str(), hostPort.Host.size() + 1);
    parsed.Port = hostPort.Port;
    parsed.CompressFlags = 0;

    out = parsed;
    return true;
}

TqHttpConnectServer::TqHttpConnectServer(std::string listenHostPort, TunnelStartFn onTunnel, TqThreadPool* pool) :
    ListenHostPort(std::move(listenHostPort)),
    OnTunnel(std::move(onTunnel)),
    Pool(pool) {}

TqHttpConnectServer::~TqHttpConnectServer() {
    Stop();
}

bool TqHttpConnectServer::Start(std::string& err) {
    if (TqSocketValid(ListenFd.load())) {
        return true;
    }
    TqSocketHandle listenFd = TqInvalidSocket;
    if (!TqCreateListenSocket(ListenHostPort, listenFd)) {
        err = "failed to listen on " + ListenHostPort;
        return false;
    }
    ListenFd.store(listenFd);
    Stopping.store(false);
    Worker = std::thread(&TqHttpConnectServer::Run, this);
    return true;
}

void TqHttpConnectServer::Stop() {
    Stopping.store(true);
    const TqSocketHandle fd = ListenFd.exchange(TqInvalidSocket);
    if (TqSocketValid(fd)) {
        (void)TqShutdownBoth(fd);
        TqCloseSocket(fd);
    }
    if (Worker.joinable()) {
        Worker.join();
    }
}

void TqHttpConnectServer::Run() {
    for (;;) {
        const TqSocketHandle listenFd = ListenFd.load();
        if (!TqSocketValid(listenFd)) {
            break;
        }
        TqSocketHandle clientFd = ::accept(listenFd, nullptr, nullptr);
        if (!TqSocketValid(clientFd)) {
            if (Stopping.load()) {
                break;
            }
            if (TqSocketInterrupted(TqLastSocketError())) {
                continue;
            }
            std::fprintf(stderr, "tcpquic-proxy: accept failed on %s (error=%d)\n",
                ListenHostPort.c_str(),
                TqLastSocketError());
            continue;
        }

        if (Pool == nullptr || !Pool->Submit([clientFd, onTunnel = OnTunnel]() { TqHandleHttpConnectClient(clientFd, onTunnel); })) {
            TqCloseSocket(clientFd);
        }
    }
}

void RunHttpConnectServer(const std::string& listenHostPort, TunnelStartFn onTunnel, TqThreadPool* pool) {
    std::string err;
    TqHttpConnectServer server(listenHostPort, std::move(onTunnel), pool);
    if (!server.Start(err)) {
        std::fprintf(stderr, "tcpquic-proxy: %s\n", err.c_str());
        return;
    }
    while (true) {
        std::this_thread::sleep_for(std::chrono::hours(24));
    }
}
