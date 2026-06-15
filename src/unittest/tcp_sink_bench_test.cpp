#include "tcp_sink_bench.h"

#include <cassert>
#include <string>

int main() {
    {
        TqTcpBenchEndpoint endpoint{};
        std::string err;
        assert(TqTcpBenchParseEndpoint("169.254.59.196:5201", endpoint, err));
        assert(endpoint.Host == "169.254.59.196");
        assert(endpoint.Port == 5201);
    }

    {
        TqTcpBenchEndpoint endpoint{};
        std::string err;
        assert(!TqTcpBenchParseEndpoint("169.254.59.196", endpoint, err));
        assert(!err.empty());
    }

    {
        TqTcpBenchEndpoint endpoint{"example.com", 443};
        const std::string request = TqTcpBenchBuildHttpConnectRequest(endpoint);
        assert(request.find("CONNECT example.com:443 HTTP/1.1\r\n") == 0);
        assert(request.find("Host: example.com:443\r\n") != std::string::npos);
        assert(request.rfind("\r\n\r\n") == request.size() - 4);
    }

    assert(TqTcpBenchHttpConnectSucceeded("HTTP/1.1 200 Connection Established\r\n\r\n"));
    assert(TqTcpBenchHttpConnectSucceeded("HTTP/1.0 200 OK\r\nHeader: value\r\n\r\n"));
    assert(!TqTcpBenchHttpConnectSucceeded("HTTP/1.1 407 Proxy Authentication Required\r\n\r\n"));
    assert(!TqTcpBenchHttpConnectSucceeded("not http\r\n\r\n"));

    return 0;
}
