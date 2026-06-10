#include "socks5_server.h"

#include "platform_socket.h"
#include "tcp_dialer.h"
#include "tcp_tunnel.h"
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

constexpr uint8_t TqSocks5Version = 0x05;
constexpr uint8_t TqSocks5AuthNone = 0x00;
constexpr uint8_t TqSocks5AuthNoAcceptable = 0xFF;
constexpr uint8_t TqSocks5CmdConnect = 0x01;
constexpr uint8_t TqSocks5Rsv = 0x00;
constexpr uint8_t TqSocks5AtypIpv4 = 0x01;
constexpr uint8_t TqSocks5AtypDomain = 0x03;
constexpr uint8_t TqSocks5AtypIpv6 = 0x04;

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
            return false;
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

bool TqRecvAll(TqSocketHandle fd, uint8_t* data, size_t length) {
    size_t received = 0;
    while (received < length) {
        const int result = TqRecv(fd, data + received, length - received, TqRecvFlags::None);
        if (result < 0) {
            if (TqSocketInterrupted(TqLastSocketError())) {
                continue;
            }
            return false;
        }
        if (result == 0) {
            return false;
        }
        received += static_cast<size_t>(result);
    }
    return true;
}

bool TqSendAll(TqSocketHandle fd, const uint8_t* data, size_t length) {
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

bool TqSendSocks5Method(TqSocketHandle fd, uint8_t method) {
    const uint8_t response[] = {TqSocks5Version, method};
    return TqSendAll(fd, response, sizeof(response));
}

bool TqSendSocks5Reply(TqSocketHandle fd, uint8_t rep) {
    const uint8_t response[] = {
        TqSocks5Version,
        rep,
        TqSocks5Rsv,
        TqSocks5AtypIpv4,
        0x00, 0x00, 0x00, 0x00,
        0x00, 0x00};
    return TqSendAll(fd, response, sizeof(response));
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

bool TqReadSocks5Greeting(TqSocketHandle clientFd) {
    uint8_t header[2]{};
    if (!TqRecvAll(clientFd, header, sizeof(header)) || header[0] != TqSocks5Version || header[1] == 0) {
        return false;
    }

    uint8_t methods[255]{};
    if (!TqRecvAll(clientFd, methods, header[1])) {
        return false;
    }

    for (uint8_t i = 0; i < header[1]; ++i) {
        if (methods[i] == TqSocks5AuthNone) {
            return TqSendSocks5Method(clientFd, TqSocks5AuthNone);
        }
    }

    (void)TqSendSocks5Method(clientFd, TqSocks5AuthNoAcceptable);
    return false;
}

bool TqReadSocks5ConnectRequest(TqSocketHandle clientFd, TunnelRequest& out, uint8_t& rep) {
    uint8_t header[4]{};
    if (!TqRecvAll(clientFd, header, sizeof(header))) {
        return false;
    }
    if (header[0] != TqSocks5Version || header[2] != TqSocks5Rsv) {
        rep = TQ_SOCKS5_REP_GENERAL_FAILURE;
        return false;
    }
    if (header[1] != TqSocks5CmdConnect) {
        rep = TQ_SOCKS5_REP_COMMAND_NOT_SUPPORTED;
        return false;
    }

    std::vector<uint8_t> request(header, header + sizeof(header));
    switch (header[3]) {
    case TqSocks5AtypIpv4: {
        uint8_t tail[6]{};
        if (!TqRecvAll(clientFd, tail, sizeof(tail))) {
            return false;
        }
        request.insert(request.end(), tail, tail + sizeof(tail));
        break;
    }
    case TqSocks5AtypIpv6: {
        uint8_t tail[18]{};
        if (!TqRecvAll(clientFd, tail, sizeof(tail))) {
            return false;
        }
        request.insert(request.end(), tail, tail + sizeof(tail));
        break;
    }
    case TqSocks5AtypDomain: {
        uint8_t len = 0;
        if (!TqRecvAll(clientFd, &len, 1) || len == 0) {
            rep = TQ_SOCKS5_REP_ADDRESS_TYPE_NOT_SUPPORTED;
            return false;
        }
        request.push_back(len);
        std::vector<uint8_t> tail(static_cast<size_t>(len) + 2);
        if (!TqRecvAll(clientFd, tail.data(), tail.size())) {
            return false;
        }
        request.insert(request.end(), tail.begin(), tail.end());
        break;
    }
    default:
        rep = TQ_SOCKS5_REP_ADDRESS_TYPE_NOT_SUPPORTED;
        return false;
    }

    if (!TqParseSocks5ConnectRequest(request, out)) {
        rep = TQ_SOCKS5_REP_GENERAL_FAILURE;
        return false;
    }

    rep = TQ_SOCKS5_REP_SUCCEEDED;
    return true;
}

void TqHandleSocks5Client(TqSocketHandle clientFd, const TunnelStartFn& onTunnel) {
    if (!TqReadSocks5Greeting(clientFd)) {
        TqCloseSocket(clientFd);
        return;
    }

    TunnelRequest tunnel{};
    uint8_t rep = TQ_SOCKS5_REP_GENERAL_FAILURE;
    if (!TqReadSocks5ConnectRequest(clientFd, tunnel, rep)) {
        (void)TqSendSocks5Reply(clientFd, rep);
        TqCloseSocket(clientFd);
        return;
    }

    if (!onTunnel) {
        (void)TqSendSocks5Reply(clientFd, TQ_SOCKS5_REP_GENERAL_FAILURE);
        TqCloseSocket(clientFd);
        return;
    }

    TqTuneTcpForThroughput(clientFd);

    const TqTunnelStartResult result = onTunnel(tunnel, clientFd);
    if (!result.Ok) {
        (void)TqSendSocks5Reply(clientFd, TqSocks5RepForOpenError(result.Error));
        TqCloseSocket(clientFd);
        return;
    }

    if (!TqSendSocks5Reply(clientFd, TQ_SOCKS5_REP_SUCCEEDED)) {
        TqCloseSocket(clientFd);
        return;
    }
}

} // namespace

uint8_t TqSocks5RepForOpenError(TqOpenError error) {
    switch (error) {
    case TqOpenError::Ok:
        return TQ_SOCKS5_REP_SUCCEEDED;
    case TqOpenError::AclDenied:
        return TQ_SOCKS5_REP_CONNECTION_NOT_ALLOWED;
    case TqOpenError::DnsFailed:
        return TQ_SOCKS5_REP_HOST_UNREACHABLE;
    case TqOpenError::TcpRefused:
        return TQ_SOCKS5_REP_CONNECTION_REFUSED;
    case TqOpenError::TcpTimeout:
        return TQ_SOCKS5_REP_TTL_EXPIRED;
    case TqOpenError::Internal:
    default:
        return TQ_SOCKS5_REP_GENERAL_FAILURE;
    }
}

bool TqParseSocks5ConnectRequest(const std::vector<uint8_t>& request, TunnelRequest& out) {
    if (request.size() < 7 ||
        request[0] != TqSocks5Version ||
        request[1] != TqSocks5CmdConnect ||
        request[2] != TqSocks5Rsv) {
        return false;
    }

    size_t offset = 4;
    std::string host;
    uint8_t addrType = 0;
    switch (request[3]) {
    case TqSocks5AtypIpv4: {
        if (request.size() != 10) {
            return false;
        }
        char text[INET_ADDRSTRLEN]{};
        if (TqInetNtop(AF_INET, request.data() + offset, text, sizeof(text)) == nullptr) {
            return false;
        }
        host = text;
        addrType = TQ_ADDR_IPV4;
        offset += 4;
        break;
    }
    case TqSocks5AtypDomain: {
        const uint8_t len = request[offset++];
        if (len == 0 || request.size() != static_cast<size_t>(7 + len)) {
            return false;
        }
        host.assign(reinterpret_cast<const char*>(request.data() + offset), len);
        addrType = TQ_ADDR_DOMAIN;
        offset += len;
        break;
    }
    case TqSocks5AtypIpv6: {
        if (request.size() != 22) {
            return false;
        }
        char text[INET6_ADDRSTRLEN]{};
        if (TqInetNtop(AF_INET6, request.data() + offset, text, sizeof(text)) == nullptr) {
            return false;
        }
        host = text;
        addrType = TQ_ADDR_IPV6;
        offset += 16;
        break;
    }
    default:
        return false;
    }

    if (host.empty() || host.size() >= sizeof(out.Host) || offset + 2 != request.size()) {
        return false;
    }

    const uint16_t port = static_cast<uint16_t>(
        (static_cast<uint16_t>(request[offset]) << 8) |
        static_cast<uint16_t>(request[offset + 1]));
    if (port == 0) {
        return false;
    }

    TunnelRequest parsed{};
    parsed.AddrType = addrType;
    std::memcpy(parsed.Host, host.c_str(), host.size() + 1);
    parsed.Port = port;
    parsed.CompressFlags = 0;

    out = parsed;
    return true;
}

TqSocks5Server::TqSocks5Server(std::string listenHostPort, TunnelStartFn onTunnel, TqThreadPool* pool) :
    ListenHostPort(std::move(listenHostPort)),
    OnTunnel(std::move(onTunnel)),
    Pool(pool) {}

TqSocks5Server::~TqSocks5Server() {
    Stop();
}

bool TqSocks5Server::Start(std::string& err) {
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
    Worker = std::thread(&TqSocks5Server::Run, this);
    return true;
}

void TqSocks5Server::Stop() {
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

void TqSocks5Server::Run() {
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

        if (Pool == nullptr || !Pool->Submit([clientFd, onTunnel = OnTunnel]() { TqHandleSocks5Client(clientFd, onTunnel); })) {
            TqCloseSocket(clientFd);
        }
    }
}

void RunSocks5Server(const std::string& listenHostPort, TunnelStartFn onTunnel, TqThreadPool* pool) {
    std::string err;
    TqSocks5Server server(listenHostPort, std::move(onTunnel), pool);
    if (!server.Start(err)) {
        std::fprintf(stderr, "tcpquic-proxy: %s\n", err.c_str());
        return;
    }
    while (true) {
        std::this_thread::sleep_for(std::chrono::hours(24));
    }
}
