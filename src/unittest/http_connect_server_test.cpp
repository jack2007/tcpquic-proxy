#include "../http_connect_server.h"

#include <cassert>
#include <cstring>

int main() {
    TunnelRequest req{};
    assert(TqParseHttpConnectRequest(
        "CONNECT example.test:443 HTTP/1.1\r\n"
        "Host: example.test:443\r\n"
        "\r\n",
        req));
    assert(req.AddrType == TQ_ADDR_DOMAIN);
    assert(std::strcmp(req.Host, "example.test") == 0);
    assert(req.Port == 443);

    TunnelRequest ipv4{};
    assert(TqParseHttpConnectRequest(
        "CONNECT 127.0.0.1:8443 HTTP/1.0\r\n"
        "\r\n",
        ipv4));
    assert(ipv4.AddrType == TQ_ADDR_IPV4);
    assert(std::strcmp(ipv4.Host, "127.0.0.1") == 0);
    assert(ipv4.Port == 8443);

    TunnelRequest ipv6{};
    assert(TqParseHttpConnectRequest(
        "CONNECT [2001:db8::1]:443 HTTP/1.1\r\n"
        "\r\n",
        ipv6));
    assert(ipv6.AddrType == TQ_ADDR_IPV6);
    assert(std::strcmp(ipv6.Host, "2001:db8::1") == 0);
    assert(ipv6.Port == 443);

    TunnelRequest rejected{};
    assert(!TqParseHttpConnectRequest(
        "CONNECT http://example.test:443 HTTP/1.1\r\n"
        "\r\n",
        rejected));
    assert(!TqParseHttpConnectRequest(
        "GET example.test:443 HTTP/1.1\r\n"
        "\r\n",
        rejected));
    assert(!TqParseHttpConnectRequest(
        "CONNECT example.test HTTP/1.1\r\n"
        "\r\n",
        rejected));
    assert(!TqParseHttpConnectRequest(
        "CONNECT example.test:0 HTTP/1.1\r\n"
        "\r\n",
        rejected));

    assert(TqHttpStatusForOpenError(TqOpenError::AclDenied) == 403);
    assert(TqHttpStatusForOpenError(TqOpenError::DnsFailed) == 502);
    assert(TqHttpStatusForOpenError(TqOpenError::TcpRefused) == 502);
    assert(TqHttpStatusForOpenError(TqOpenError::TcpTimeout) == 504);
    assert(TqHttpStatusForOpenError(TqOpenError::Internal) == 500);

    return 0;
}
