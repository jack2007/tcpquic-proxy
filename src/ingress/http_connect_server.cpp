#include "http_connect_server.h"

#include "platform_socket.h"

#include <cstring>
#include <limits>
#include <string>

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
