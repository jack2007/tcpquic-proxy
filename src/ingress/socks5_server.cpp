#include "socks5_server.h"

#include "platform_socket.h"

#include <cstring>
#include <string>

namespace {

constexpr uint8_t TqSocks5Version = 0x05;
constexpr uint8_t TqSocks5CmdConnect = 0x01;
constexpr uint8_t TqSocks5Rsv = 0x00;
constexpr uint8_t TqSocks5AtypIpv4 = 0x01;
constexpr uint8_t TqSocks5AtypDomain = 0x03;
constexpr uint8_t TqSocks5AtypIpv6 = 0x04;

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
