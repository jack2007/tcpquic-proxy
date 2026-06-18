#include "client_ingress_state.h"

#include "control_protocol.h"

#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace {

bool CheckDomainRequest(const TunnelRequest& req, const char* host, uint16_t port, uint8_t proto) {
    return req.AddrType == TQ_ADDR_DOMAIN &&
        std::strcmp(req.Host, host) == 0 &&
        req.Port == port &&
        req.IngressTraceProto == proto;
}

bool CheckRequest(const TunnelRequest& req, uint8_t addrType, const char* host, uint16_t port, uint8_t proto) {
    return req.AddrType == addrType &&
        std::strcmp(req.Host, host) == 0 &&
        req.Port == port &&
        req.IngressTraceProto == proto;
}

bool CheckBytes(const std::vector<uint8_t>& actual, const std::vector<uint8_t>& expected) {
    return actual == expected;
}

int TestSocksGreetingQueuesNoAuth() {
    TqClientIngressState state(TqClientIngressProto::Socks5);
    const uint8_t greeting[] = {0x05, 0x01, 0x00};

    if (state.Feed(greeting, sizeof(greeting)) != TqClientIngressResult::NeedWrite) {
        return 10;
    }
    const std::string& pending = state.PendingWrite();
    if (pending.size() != 2) {
        return 11;
    }
    if (static_cast<uint8_t>(pending[0]) != 0x05 || static_cast<uint8_t>(pending[1]) != 0x00) {
        return 12;
    }
    state.MarkWriteComplete(1);
    if (state.PendingWrite().size() != 1 || static_cast<uint8_t>(state.PendingWrite()[0]) != 0x00) {
        return 13;
    }
    state.MarkWriteComplete(100);
    if (!state.PendingWrite().empty()) {
        return 14;
    }
    return 0;
}

int TestSocksDomainRequestReady() {
    TqClientIngressState state(TqClientIngressProto::Socks5);
    const uint8_t greeting[] = {0x05, 0x01, 0x00};
    if (state.Feed(greeting, sizeof(greeting)) != TqClientIngressResult::NeedWrite) {
        return 20;
    }
    state.MarkWriteComplete(2);

    const std::vector<uint8_t> connect{
        0x05, 0x01, 0x00, 0x03, 0x0b,
        'e', 'x', 'a', 'm', 'p', 'l', 'e', '.', 'c', 'o', 'm',
        0x01, 0xbb};
    if (state.Feed(connect.data(), connect.size()) != TqClientIngressResult::ReadyToOpen) {
        return 21;
    }
    if (!CheckDomainRequest(state.Request(), "example.com", 443, 1)) {
        return 22;
    }
    return 0;
}

int TestSocksIpv4RequestReady() {
    TqClientIngressState state(TqClientIngressProto::Socks5);
    const uint8_t greeting[] = {0x05, 0x01, 0x00};
    if (state.Feed(greeting, sizeof(greeting)) != TqClientIngressResult::NeedWrite) {
        return 25;
    }
    state.MarkWriteComplete(2);

    const std::vector<uint8_t> connect{
        0x05, 0x01, 0x00, 0x01,
        127, 0, 0, 1,
        0x00, 0x50};
    if (state.Feed(connect.data(), connect.size()) != TqClientIngressResult::ReadyToOpen) {
        return 26;
    }
    if (!CheckRequest(state.Request(), TQ_ADDR_IPV4, "127.0.0.1", 80, 1)) {
        return 27;
    }
    return 0;
}

int TestSocksIpv6RequestReady() {
    TqClientIngressState state(TqClientIngressProto::Socks5);
    const uint8_t greeting[] = {0x05, 0x01, 0x00};
    if (state.Feed(greeting, sizeof(greeting)) != TqClientIngressResult::NeedWrite) {
        return 28;
    }
    state.MarkWriteComplete(2);

    const std::vector<uint8_t> connect{
        0x05, 0x01, 0x00, 0x04,
        0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1,
        0x01, 0xbb};
    if (state.Feed(connect.data(), connect.size()) != TqClientIngressResult::ReadyToOpen) {
        return 29;
    }
    if (!CheckRequest(state.Request(), TQ_ADDR_IPV6, "::1", 443, 1)) {
        return 35;
    }
    return 0;
}

int TestSocksPartialGreetingAndRequest() {
    TqClientIngressState state(TqClientIngressProto::Socks5);
    const uint8_t greetingPart1[] = {0x05, 0x01};
    const uint8_t greetingPart2[] = {0x00};
    if (state.Feed(greetingPart1, sizeof(greetingPart1)) != TqClientIngressResult::NeedRead) {
        return 30;
    }
    if (state.Feed(greetingPart2, sizeof(greetingPart2)) != TqClientIngressResult::NeedWrite) {
        return 31;
    }
    state.MarkWriteComplete(2);

    const uint8_t requestPart1[] = {0x05, 0x01, 0x00, 0x03, 0x0b, 'e', 'x'};
    const uint8_t requestPart2[] = {
        'a', 'm', 'p', 'l', 'e', '.', 'c', 'o', 'm', 0x01, 0xbb};
    if (state.Feed(requestPart1, sizeof(requestPart1)) != TqClientIngressResult::NeedRead) {
        return 32;
    }
    if (state.Feed(requestPart2, sizeof(requestPart2)) != TqClientIngressResult::ReadyToOpen) {
        return 33;
    }
    if (!CheckDomainRequest(state.Request(), "example.com", 443, 1)) {
        return 34;
    }
    return 0;
}

int TestSocksBufferedRequestAfterGreetingWrite() {
    TqClientIngressState state(TqClientIngressProto::Socks5);
    const std::vector<uint8_t> data{
        0x05, 0x01, 0x00,
        0x05, 0x01, 0x00, 0x03, 0x0b,
        'e', 'x', 'a', 'm', 'p', 'l', 'e', '.', 'c', 'o', 'm',
        0x01, 0xbb};
    if (state.Feed(data.data(), data.size()) != TqClientIngressResult::NeedWrite) {
        return 40;
    }
    state.MarkWriteComplete(2);
    if (state.Feed(nullptr, 0) != TqClientIngressResult::ReadyToOpen) {
        return 41;
    }
    if (!CheckDomainRequest(state.Request(), "example.com", 443, 1)) {
        return 42;
    }
    return 0;
}

int TestSocksEarlyDataAvailableAfterReady() {
    TqClientIngressState state(TqClientIngressProto::Socks5);
    const std::vector<uint8_t> early{'G', 'E', 'T', ' ', '/', '\r', '\n'};
    std::vector<uint8_t> data{
        0x05, 0x01, 0x00,
        0x05, 0x01, 0x00, 0x03, 0x0b,
        'e', 'x', 'a', 'm', 'p', 'l', 'e', '.', 'c', 'o', 'm',
        0x01, 0xbb};
    data.insert(data.end(), early.begin(), early.end());

    if (state.Feed(data.data(), data.size()) != TqClientIngressResult::NeedWrite) {
        return 46;
    }
    state.MarkWriteComplete(2);
    if (state.Feed(nullptr, 0) != TqClientIngressResult::ReadyToOpen) {
        return 47;
    }
    if (!CheckBytes(state.TakeBufferedData(), early)) {
        return 48;
    }
    if (!state.TakeBufferedData().empty()) {
        return 49;
    }
    return 0;
}

int TestSocksPostReadyFeedBuffersData() {
    TqClientIngressState state(TqClientIngressProto::Socks5);
    const uint8_t greeting[] = {0x05, 0x01, 0x00};
    if (state.Feed(greeting, sizeof(greeting)) != TqClientIngressResult::NeedWrite) {
        return 86;
    }
    state.MarkWriteComplete(2);

    const std::vector<uint8_t> connect{
        0x05, 0x01, 0x00, 0x03, 0x0b,
        'e', 'x', 'a', 'm', 'p', 'l', 'e', '.', 'c', 'o', 'm',
        0x01, 0xbb};
    if (state.Feed(connect.data(), connect.size()) != TqClientIngressResult::ReadyToOpen) {
        return 87;
    }

    const std::vector<uint8_t> body{'d', 'a', 't', 'a'};
    if (state.Feed(body.data(), body.size()) != TqClientIngressResult::ReadyToOpen) {
        return 88;
    }
    if (!CheckBytes(state.TakeBufferedData(), body)) {
        return 89;
    }
    return 0;
}

int TestSocksUnsupportedAuthCloses() {
    TqClientIngressState state(TqClientIngressProto::Socks5);
    const uint8_t greeting[] = {0x05, 0x02, 0x01, 0x02};
    if (state.Feed(greeting, sizeof(greeting)) != TqClientIngressResult::NeedWrite) {
        return 45;
    }
    const std::string& pending = state.PendingWrite();
    if (pending.size() != 2 || static_cast<uint8_t>(pending[1]) != 0xFF) {
        return 46;
    }
    state.MarkWriteComplete(2);
    if (state.Feed(nullptr, 0) != TqClientIngressResult::Close) {
        return 47;
    }
    return 0;
}

int TestHttpConnectReady() {
    TqClientIngressState state(TqClientIngressProto::HttpConnect);
    const std::string request =
        "CONNECT example.org:8443 HTTP/1.1\r\n"
        "Host: example.org:8443\r\n"
        "\r\n";
    if (state.Feed(reinterpret_cast<const uint8_t*>(request.data()), request.size()) !=
        TqClientIngressResult::ReadyToOpen) {
        return 50;
    }
    if (!CheckDomainRequest(state.Request(), "example.org", 8443, 2)) {
        return 51;
    }
    if (!state.PendingWrite().empty()) {
        return 52;
    }
    return 0;
}

int TestHttpEarlyDataAvailableAfterReady() {
    TqClientIngressState state(TqClientIngressProto::HttpConnect);
    const std::string header =
        "CONNECT example.org:8443 HTTP/1.1\r\n"
        "Host: example.org:8443\r\n"
        "\r\n";
    const std::vector<uint8_t> early{'P', 'A', 'Y', 'L', 'O', 'A', 'D'};
    std::vector<uint8_t> data(header.begin(), header.end());
    data.insert(data.end(), early.begin(), early.end());

    if (state.Feed(data.data(), data.size()) != TqClientIngressResult::ReadyToOpen) {
        return 53;
    }
    if (!CheckDomainRequest(state.Request(), "example.org", 8443, 2)) {
        return 54;
    }
    if (!CheckBytes(state.TakeBufferedData(), early)) {
        return 55;
    }
    if (!state.TakeBufferedData().empty()) {
        return 56;
    }
    return 0;
}

int TestHttpSmallHeaderWithLargeEarlyDataReady() {
    TqClientIngressState state(TqClientIngressProto::HttpConnect);
    const std::string header =
        "CONNECT example.org:8443 HTTP/1.1\r\n"
        "\r\n";
    std::vector<uint8_t> data(header.begin(), header.end());
    data.insert(data.end(), 16 * 1024 + 1, 'x');

    if (state.Feed(data.data(), data.size()) != TqClientIngressResult::ReadyToOpen) {
        return 57;
    }
    if (!CheckDomainRequest(state.Request(), "example.org", 8443, 2)) {
        return 58;
    }
    if (state.TakeBufferedData().size() != 16 * 1024 + 1) {
        return 59;
    }
    return 0;
}

int TestHttpPostReadyFeedBuffersData() {
    TqClientIngressState state(TqClientIngressProto::HttpConnect);
    const std::string request =
        "CONNECT example.org:8443 HTTP/1.1\r\n"
        "Host: example.org:8443\r\n"
        "\r\n";
    if (state.Feed(reinterpret_cast<const uint8_t*>(request.data()), request.size()) !=
        TqClientIngressResult::ReadyToOpen) {
        return 90;
    }

    const std::vector<uint8_t> body{'d', 'a', 't', 'a'};
    if (state.Feed(body.data(), body.size()) != TqClientIngressResult::ReadyToOpen) {
        return 91;
    }
    if (!CheckBytes(state.TakeBufferedData(), body)) {
        return 92;
    }
    return 0;
}

int TestHttpPartialHeader() {
    TqClientIngressState state(TqClientIngressProto::HttpConnect);
    const std::string part1 = "CONNECT example.org:8443 HTTP/1.1\r\nHost:";
    const std::string part2 = " example.org:8443\r\n\r\n";
    if (state.Feed(reinterpret_cast<const uint8_t*>(part1.data()), part1.size()) !=
        TqClientIngressResult::NeedRead) {
        return 60;
    }
    if (state.Feed(reinterpret_cast<const uint8_t*>(part2.data()), part2.size()) !=
        TqClientIngressResult::ReadyToOpen) {
        return 61;
    }
    if (!CheckDomainRequest(state.Request(), "example.org", 8443, 2)) {
        return 62;
    }
    return 0;
}

int TestHttpInvalidRequestCloses() {
    TqClientIngressState state(TqClientIngressProto::HttpConnect);
    const std::string request =
        "GET example.org:8443 HTTP/1.1\r\n"
        "\r\n";
    if (state.Feed(reinterpret_cast<const uint8_t*>(request.data()), request.size()) !=
        TqClientIngressResult::Close) {
        return 70;
    }
    return 0;
}

int TestHttpHeaderTooLargeCloses() {
    TqClientIngressState state(TqClientIngressProto::HttpConnect);
    std::string request = "CONNECT example.org:8443 HTTP/1.1\r\n";
    request.append(16 * 1024, 'x');
    if (state.Feed(reinterpret_cast<const uint8_t*>(request.data()), request.size()) !=
        TqClientIngressResult::Close) {
        return 80;
    }
    return 0;
}

int TestHttpExactlyCapIncompleteHeaderCloses() {
    TqClientIngressState state(TqClientIngressProto::HttpConnect);
    std::string request(16 * 1024, 'x');
    if (state.Feed(reinterpret_cast<const uint8_t*>(request.data()), request.size()) !=
        TqClientIngressResult::Close) {
        return 85;
    }
    return 0;
}

int TestSocksUserPassAuthRequiredRejectsNoAuth() {
    auto auth = std::make_shared<TqProxyAuthTable>(
        std::vector<TqProxyAuthUser>{{"alice", "secret-a"}});
    TqClientIngressState state(TqClientIngressProto::Socks5, auth);
    const uint8_t greeting[] = {0x05, 0x01, 0x00};

    if (state.Feed(greeting, sizeof(greeting)) != TqClientIngressResult::NeedWrite) {
        return 93;
    }
    const std::string& pending = state.PendingWrite();
    if (pending.size() != 2 ||
        static_cast<uint8_t>(pending[0]) != 0x05 ||
        static_cast<uint8_t>(pending[1]) != 0xFF) {
        return 94;
    }
    state.MarkWriteComplete(2);
    if (state.Feed(nullptr, 0) != TqClientIngressResult::Close) {
        return 95;
    }
    return 0;
}

int TestSocksUserPassAuthSuccess() {
    auto auth = std::make_shared<TqProxyAuthTable>(
        std::vector<TqProxyAuthUser>{{"alice", "secret-a"}});
    TqClientIngressState state(TqClientIngressProto::Socks5, auth);
    const std::vector<uint8_t> data{
        0x05, 0x01, 0x02,
        0x01, 0x05, 'a', 'l', 'i', 'c', 'e', 0x08,
        's', 'e', 'c', 'r', 'e', 't', '-', 'a',
        0x05, 0x01, 0x00, 0x03, 0x0b,
        'e', 'x', 'a', 'm', 'p', 'l', 'e', '.', 'c', 'o', 'm',
        0x01, 0xbb};

    if (state.Feed(data.data(), data.size()) != TqClientIngressResult::NeedWrite) {
        return 96;
    }
    const std::string& pending = state.PendingWrite();
    if (pending.size() != 4 ||
        static_cast<uint8_t>(pending[0]) != 0x05 ||
        static_cast<uint8_t>(pending[1]) != 0x02 ||
        static_cast<uint8_t>(pending[2]) != 0x01 ||
        static_cast<uint8_t>(pending[3]) != 0x00) {
        return 97;
    }
    state.MarkWriteComplete(4);
    if (state.Feed(nullptr, 0) != TqClientIngressResult::ReadyToOpen) {
        return 98;
    }
    if (!CheckDomainRequest(state.Request(), "example.com", 443, 1)) {
        return 99;
    }
    return 0;
}

int TestSocksUserPassAuthFailureClosesAfterStatus() {
    auto auth = std::make_shared<TqProxyAuthTable>(
        std::vector<TqProxyAuthUser>{{"alice", "secret-a"}});
    TqClientIngressState state(TqClientIngressProto::Socks5, auth);
    const std::vector<uint8_t> data{
        0x05, 0x01, 0x02,
        0x01, 0x05, 'a', 'l', 'i', 'c', 'e', 0x05,
        'w', 'r', 'o', 'n', 'g'};

    if (state.Feed(data.data(), data.size()) != TqClientIngressResult::NeedWrite) {
        return 100;
    }
    const std::string& pending = state.PendingWrite();
    if (pending.size() != 4 ||
        static_cast<uint8_t>(pending[1]) != 0x02 ||
        static_cast<uint8_t>(pending[3]) != 0x01) {
        return 101;
    }
    state.MarkWriteComplete(4);
    if (state.Feed(nullptr, 0) != TqClientIngressResult::Close) {
        return 102;
    }
    return 0;
}

int TestHttpConnectAuthRequiredRejectsMissingOrWrong() {
    auto auth = std::make_shared<TqProxyAuthTable>(
        std::vector<TqProxyAuthUser>{{"alice", "secret-a"}});
    {
        TqClientIngressState state(TqClientIngressProto::HttpConnect, auth);
        const std::string request =
            "CONNECT example.org:8443 HTTP/1.1\r\n"
            "Host: example.org:8443\r\n"
            "\r\n";
        if (state.Feed(reinterpret_cast<const uint8_t*>(request.data()), request.size()) !=
            TqClientIngressResult::NeedWrite) {
            return 103;
        }
        if (state.PendingWrite().find("407 Proxy Authentication Required") == std::string::npos) {
            return 104;
        }
        state.MarkWriteComplete(state.PendingWrite().size());
        if (state.Feed(nullptr, 0) != TqClientIngressResult::Close) {
            return 105;
        }
    }
    {
        TqClientIngressState state(TqClientIngressProto::HttpConnect, auth);
        const std::string request =
            "CONNECT example.org:8443 HTTP/1.1\r\n"
            "Host: example.org:8443\r\n"
            "Proxy-Authorization: Basic YWxpY2U6d3Jvbmc=\r\n"
            "\r\n";
        if (state.Feed(reinterpret_cast<const uint8_t*>(request.data()), request.size()) !=
            TqClientIngressResult::NeedWrite) {
            return 106;
        }
        if (state.PendingWrite().find("407 Proxy Authentication Required") == std::string::npos) {
            return 107;
        }
    }
    return 0;
}

int TestHttpConnectAuthSuccess() {
    auto auth = std::make_shared<TqProxyAuthTable>(
        std::vector<TqProxyAuthUser>{{"alice", "secret-a"}});
    TqClientIngressState state(TqClientIngressProto::HttpConnect, auth);
    const std::string request =
        "CONNECT example.org:8443 HTTP/1.1\r\n"
        "Host: example.org:8443\r\n"
        "Proxy-Authorization: Basic YWxpY2U6c2VjcmV0LWE=\r\n"
        "\r\n";
    if (state.Feed(reinterpret_cast<const uint8_t*>(request.data()), request.size()) !=
        TqClientIngressResult::ReadyToOpen) {
        return 108;
    }
    if (!CheckDomainRequest(state.Request(), "example.org", 8443, 2)) {
        return 109;
    }
    return 0;
}

} // namespace

int main() {
    int rc = TestSocksGreetingQueuesNoAuth();
    if (rc != 0) {
        return rc;
    }
    rc = TestSocksDomainRequestReady();
    if (rc != 0) {
        return rc;
    }
    rc = TestSocksIpv4RequestReady();
    if (rc != 0) {
        return rc;
    }
    rc = TestSocksIpv6RequestReady();
    if (rc != 0) {
        return rc;
    }
    rc = TestSocksPartialGreetingAndRequest();
    if (rc != 0) {
        return rc;
    }
    rc = TestSocksBufferedRequestAfterGreetingWrite();
    if (rc != 0) {
        return rc;
    }
    rc = TestSocksEarlyDataAvailableAfterReady();
    if (rc != 0) {
        return rc;
    }
    rc = TestSocksPostReadyFeedBuffersData();
    if (rc != 0) {
        return rc;
    }
    rc = TestSocksUnsupportedAuthCloses();
    if (rc != 0) {
        return rc;
    }
    rc = TestSocksUserPassAuthRequiredRejectsNoAuth();
    if (rc != 0) {
        return rc;
    }
    rc = TestSocksUserPassAuthSuccess();
    if (rc != 0) {
        return rc;
    }
    rc = TestSocksUserPassAuthFailureClosesAfterStatus();
    if (rc != 0) {
        return rc;
    }
    rc = TestHttpConnectReady();
    if (rc != 0) {
        return rc;
    }
    rc = TestHttpEarlyDataAvailableAfterReady();
    if (rc != 0) {
        return rc;
    }
    rc = TestHttpSmallHeaderWithLargeEarlyDataReady();
    if (rc != 0) {
        return rc;
    }
    rc = TestHttpPostReadyFeedBuffersData();
    if (rc != 0) {
        return rc;
    }
    rc = TestHttpPartialHeader();
    if (rc != 0) {
        return rc;
    }
    rc = TestHttpInvalidRequestCloses();
    if (rc != 0) {
        return rc;
    }
    rc = TestHttpHeaderTooLargeCloses();
    if (rc != 0) {
        return rc;
    }
    rc = TestHttpExactlyCapIncompleteHeaderCloses();
    if (rc != 0) {
        return rc;
    }
    rc = TestHttpConnectAuthRequiredRejectsMissingOrWrong();
    if (rc != 0) {
        return rc;
    }
    rc = TestHttpConnectAuthSuccess();
    if (rc != 0) {
        return rc;
    }
    return 0;
}
