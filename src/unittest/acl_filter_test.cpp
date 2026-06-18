#include "acl_filter.h"
#include "platform_socket.h"

#include <cstring>
#include <vector>

namespace {

bool MakeIpv4(const char* dotted, uint16_t port, sockaddr_storage& ss) {
    ss = sockaddr_storage{};
    auto* sin = reinterpret_cast<sockaddr_in*>(&ss);
    if (!TqInetPton(AF_INET, dotted, &sin->sin_addr)) {
        return false;
    }
    ss.ss_family = AF_INET;
    sin->sin_port = htons(port);
    return true;
}

bool SockaddrIpv4Equals(const sockaddr_storage& ss, const char* dotted) {
    if (ss.ss_family != AF_INET) {
        return false;
    }
    const auto* sin = reinterpret_cast<const sockaddr_in*>(&ss);
    in_addr expected{};
    if (!TqInetPton(AF_INET, dotted, &expected)) {
        return false;
    }
    return std::memcmp(&sin->sin_addr, &expected, sizeof(expected)) == 0;
}

uint16_t SockaddrPort(const sockaddr_storage& ss) {
    if (ss.ss_family == AF_INET) {
        return ntohs(reinterpret_cast<const sockaddr_in*>(&ss)->sin_port);
    }
    if (ss.ss_family == AF_INET6) {
        return ntohs(reinterpret_cast<const sockaddr_in6*>(&ss)->sin6_port);
    }
    return 0;
}

} // namespace

int main() {
    TqSocketStartup startup;
    if (!startup.Ok()) {
        return 1;
    }

    TqAcl acl;
    acl.AllowCidrs = {"10.0.0.0/8"};

    sockaddr_storage allowedCandidate{};
    sockaddr_storage staleOut{};
    if (!MakeIpv4("10.0.0.42", 1234, allowedCandidate) ||
        !MakeIpv4("192.0.2.1", 1, staleOut)) {
        return 2;
    }

    std::vector<sockaddr_storage> out;
    out.push_back(staleOut);
    if (!TqAclFilterResolvedAddresses(acl, {allowedCandidate}, 8080, out)) {
        return 3;
    }
    if (out.size() != 1) {
        return 4;
    }
    if (!SockaddrIpv4Equals(out[0], "10.0.0.42")) {
        return 5;
    }
    if (SockaddrPort(out[0]) != 8080) {
        return 6;
    }

    TqAcl overlap;
    overlap.AllowCidrs = {"10.0.0.0/8"};
    overlap.DenyCidrs = {"10.0.0.42/32"};
    if (!MakeIpv4("192.0.2.2", 2, staleOut)) {
        return 7;
    }
    out.push_back(staleOut);
    if (TqAclFilterResolvedAddresses(overlap, {allowedCandidate}, 443, out)) {
        return 8;
    }
    if (!out.empty()) {
        return 9;
    }

    TqAcl mixed;
    mixed.AllowCidrs = {"10.0.0.0/8"};
    mixed.DenyCidrs = {"10.0.0.13/32"};
    sockaddr_storage mixedAllowed{};
    sockaddr_storage mixedDenied{};
    if (!MakeIpv4("10.0.0.7", 0, mixedAllowed) ||
        !MakeIpv4("10.0.0.13", 0, mixedDenied) ||
        !MakeIpv4("192.0.2.4", 4, staleOut)) {
        return 10;
    }
    out.push_back(staleOut);
    if (TqAclFilterResolvedAddresses(mixed, {mixedAllowed, mixedDenied}, 9443, out)) {
        return 11;
    }
    if (!out.empty()) {
        return 12;
    }

    TqAcl emptyAllow;
    if (TqAclFilterResolvedAddresses(emptyAllow, {allowedCandidate}, 443, out)) {
        return 13;
    }
    if (!out.empty()) {
        return 14;
    }

    sockaddr_storage deniedCandidate{};
    if (!MakeIpv4("192.168.1.10", 0, deniedCandidate)) {
        return 15;
    }
    if (TqAclFilterResolvedAddresses(acl, {deniedCandidate}, 443, out)) {
        return 16;
    }
    if (!out.empty()) {
        return 17;
    }

    if (!MakeIpv4("192.0.2.3", 3, staleOut)) {
        return 18;
    }
    out.push_back(staleOut);
    if (TqAclFilterResolvedAddresses(acl, {}, 443, out)) {
        return 19;
    }
    if (!out.empty()) {
        return 20;
    }

    return 0;
}
