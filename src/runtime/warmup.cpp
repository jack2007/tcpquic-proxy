#include "warmup.h"

#include "platform_socket.h"
#include "tcp_tunnel.h"

#include <cstdio>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

namespace {

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

bool TqBuildTunnelRequest(const TqHostPort& hostPort, TunnelRequest& out) {
    if (hostPort.Host.size() >= sizeof(out.Host)) {
        return false;
    }

    out = TunnelRequest{};
    out.AddrType = TqAddrTypeForHost(hostPort.Host);
    std::memcpy(out.Host, hostPort.Host.c_str(), hostPort.Host.size() + 1);
    out.Port = hostPort.Port;
    out.CompressFlags = 0;
    return true;
}

bool TqSendAll(TqSocketHandle fd, const char* data, size_t length) {
    size_t sent = 0;
    while (sent < length) {
        const int n = TqSend(fd, data + sent, length - sent, TqSendFlags::None);
        if (n < 0) {
            if (TqSocketInterrupted(TqLastSocketError())) {
                continue;
            }
            return false;
        }
        if (n == 0) {
            return false;
        }
        sent += static_cast<size_t>(n);
    }
    return true;
}

bool TqDiscardHttpBody(TqSocketHandle fd, uint64_t targetBytes, uint64_t& downloaded) {
    constexpr size_t kChunkSize = 64 * 1024;
    constexpr size_t kMaxHeaderBytes = 64 * 1024;
    std::vector<char> chunk(kChunkSize);
    std::string headerAccum;
    bool headersDone = false;
    downloaded = 0;

    while (downloaded < targetBytes) {
        const int n = TqRecv(fd, chunk.data(), chunk.size(), TqRecvFlags::None);
        if (n < 0) {
            if (TqSocketInterrupted(TqLastSocketError())) {
                continue;
            }
            return false;
        }
        if (n == 0) {
            break;
        }

        if (!headersDone) {
            headerAccum.append(chunk.data(), static_cast<size_t>(n));
            const size_t headerEnd = headerAccum.find("\r\n\r\n");
            if (headerEnd != std::string::npos) {
                headersDone = true;
                const size_t bodyStart = headerEnd + 4;
                if (headerAccum.size() > bodyStart) {
                    downloaded += headerAccum.size() - bodyStart;
                }
                headerAccum.clear();
            } else if (headerAccum.size() > kMaxHeaderBytes) {
                return false;
            }
            continue;
        }

        downloaded += static_cast<uint64_t>(n);
    }

    return true;
}

} // namespace

bool TqRunClientWarmup(QuicClientSession& quic, const TqConfig& cfg) {
    if (cfg.WarmupMb == 0) {
        return true;
    }

    if (cfg.WarmupTarget.empty()) {
        std::fprintf(stderr, "tcpquic-proxy: warmup requires --warmup-target host:port\n");
        return false;
    }

    TqHostPort hostPort{};
    if (!TqParseHostPort(cfg.WarmupTarget, hostPort)) {
        std::fprintf(stderr, "tcpquic-proxy: invalid warmup target: %s\n", cfg.WarmupTarget.c_str());
        return false;
    }

    TunnelRequest req{};
    if (!TqBuildTunnelRequest(hostPort, req)) {
        std::fprintf(stderr, "tcpquic-proxy: failed to build warmup tunnel request\n");
        return false;
    }

    const std::string path = cfg.WarmupPath.empty() ? "/" : cfg.WarmupPath;
    const std::string httpRequest =
        "GET " + path + " HTTP/1.0\r\nHost: " + hostPort.Host + "\r\n\r\n";
    const uint64_t targetBytes = static_cast<uint64_t>(cfg.WarmupMb) * 1024 * 1024;

    for (uint32_t i = 0; i < cfg.QuicConnections; ++i) {
        if (!quic.EnsureConnected()) {
            return false;
        }
        MsQuicConnection* conn = quic.PickConnection();
        if (conn == nullptr) {
            std::fprintf(stderr, "tcpquic-proxy: warmup failed to pick QUIC connection %u/%u\n",
                i + 1, cfg.QuicConnections);
            return false;
        }

        TqSocketHandle pair[2]{TqInvalidSocket, TqInvalidSocket};
        if (!TqSocketPair(pair)) {
            std::fprintf(stderr, "tcpquic-proxy: warmup socketpair failed (error=%d)\n", TqLastSocketError());
            return false;
        }

        const TqSocketHandle tunnelFd = pair[0];
        const TqSocketHandle pumpFd = pair[1];
        const TqTunnelStartResult started = TqStartClientTunnel(conn, req, tunnelFd, cfg);
        if (!started.Ok) {
            TqCloseSocket(pumpFd);
            TqCloseSocket(tunnelFd);
            std::fprintf(stderr,
                "tcpquic-proxy: warmup tunnel %u/%u failed to open (error=%u)\n",
                i + 1,
                cfg.QuicConnections,
                static_cast<unsigned>(started.Error));
            return false;
        }

        if (!TqSendAll(pumpFd, httpRequest.data(), httpRequest.size())) {
            TqCloseSocket(pumpFd);
            std::fprintf(stderr, "tcpquic-proxy: warmup failed to send HTTP request on conn %u/%u\n",
                i + 1, cfg.QuicConnections);
            return false;
        }

        uint64_t downloaded = 0;
        if (!TqDiscardHttpBody(pumpFd, targetBytes, downloaded)) {
            TqCloseSocket(pumpFd);
            std::fprintf(stderr, "tcpquic-proxy: warmup failed reading response on conn %u/%u\n",
                i + 1, cfg.QuicConnections);
            return false;
        }

        TqCloseSocket(pumpFd);

        const double downloadedMb = static_cast<double>(downloaded) / (1024.0 * 1024.0);
        std::fprintf(stderr,
            "tcpquic-proxy: warmup conn %u/%u downloaded %.2f MB to %s\n",
            i + 1,
            cfg.QuicConnections,
            downloadedMb,
            cfg.WarmupTarget.c_str());
    }

    return true;
}
