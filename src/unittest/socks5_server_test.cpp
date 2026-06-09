#include "../tcp_tunnel.h"
#include "../socks5_server.h"

#include <cassert>
#include <cstring>
#include <vector>

int main() {
    TunnelRequest domain{};
    const std::vector<uint8_t> domainConnect{
        0x05, 0x01, 0x00,
        0x05, 0x01, 0x00, 0x03, 0x0c,
        'e', 'x', 'a', 'm', 'p', 'l', 'e', '.', 't', 'e', 's', 't',
        0x01, 0xbb};
    assert(TqParseSocks5ConnectRequest(domainConnect, domain));
    assert(domain.AddrType == TQ_ADDR_DOMAIN);
    assert(std::strcmp(domain.Host, "example.test") == 0);
    assert(domain.Port == 443);

    TunnelRequest ipv4{};
    const std::vector<uint8_t> ipv4Connect{
        0x05, 0x01, 0x00,
        0x05, 0x01, 0x00, 0x01,
        127, 0, 0, 1,
        0x20, 0xfb};
    assert(TqParseSocks5ConnectRequest(ipv4Connect, ipv4));
    assert(ipv4.AddrType == TQ_ADDR_IPV4);
    assert(std::strcmp(ipv4.Host, "127.0.0.1") == 0);
    assert(ipv4.Port == 8443);

    TunnelRequest ipv6{};
    const std::vector<uint8_t> ipv6Connect{
        0x05, 0x01, 0x00,
        0x05, 0x01, 0x00, 0x04,
        0x20, 0x01, 0x0d, 0xb8, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1,
        0x01, 0xbb};
    assert(TqParseSocks5ConnectRequest(ipv6Connect, ipv6));
    assert(ipv6.AddrType == TQ_ADDR_IPV6);
    assert(std::strcmp(ipv6.Host, "2001:db8::1") == 0);
    assert(ipv6.Port == 443);

    TunnelRequest rejected{};
    assert(!TqParseSocks5ConnectRequest({0x04, 0x01, 0x00}, rejected));
    assert(!TqParseSocks5ConnectRequest({0x05, 0x01, 0x00, 0x05, 0x02, 0x00, 0x01, 127, 0, 0, 1, 0, 80}, rejected));
    assert(!TqParseSocks5ConnectRequest({0x05, 0x01, 0x00, 0x05, 0x01, 0x00, 0x03, 0x00, 0, 80}, rejected));
    assert(!TqParseSocks5ConnectRequest({0x05, 0x01, 0x00, 0x05, 0x01, 0x00, 0x09, 0, 80}, rejected));

    assert(TqSocks5RepForOpenError(TqOpenError::Ok) == TQ_SOCKS5_REP_SUCCEEDED);
    assert(TqSocks5RepForOpenError(TqOpenError::AclDenied) == TQ_SOCKS5_REP_CONNECTION_NOT_ALLOWED);
    assert(TqSocks5RepForOpenError(TqOpenError::DnsFailed) == TQ_SOCKS5_REP_HOST_UNREACHABLE);
    assert(TqSocks5RepForOpenError(TqOpenError::TcpRefused) == TQ_SOCKS5_REP_CONNECTION_REFUSED);
    assert(TqSocks5RepForOpenError(TqOpenError::TcpTimeout) == TQ_SOCKS5_REP_TTL_EXPIRED);
    assert(TqSocks5RepForOpenError(TqOpenError::Internal) == TQ_SOCKS5_REP_GENERAL_FAILURE);

    return 0;
}
