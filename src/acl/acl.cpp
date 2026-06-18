#include "acl.h"

#include "acl_matcher.h"

#include <cstring>
#if !defined(_WIN32)
#include <netdb.h>
#endif
#include <string>
#include <vector>

bool TqValidateCidrList(const std::vector<std::string>& cidrs, std::string& err) {
    err.clear();
    for (const auto& cidr : cidrs) {
        if (cidr.empty()) {
            err = "empty CIDR entry";
            return false;
        }
        tq_acl_internal::ParsedCidr parsed{};
        if (!tq_acl_internal::ParseCidr(cidr, parsed)) {
            err = "invalid CIDR: " + cidr;
            return false;
        }
    }
    return true;
}

bool TqAcl::IsAllowed(const std::string& host, uint16_t /*port*/) const {
    sockaddr_storage ss{};
    if (!tq_acl_internal::ParseIpLiteral(host, ss)) {
        return false;
    }

    int family = AF_UNSPEC;
    uint8_t addr[16]{};
    if (!tq_acl_internal::ExtractIpBytes(ss, family, addr)) {
        return false;
    }

    const auto deny = tq_acl_internal::ParseCidrList(DenyCidrs);
    const auto allow = tq_acl_internal::ParseCidrList(AllowCidrs);
    return tq_acl_internal::IpBytesAllowed(family, addr, deny, allow);
}

bool TqAclResolveAndFilter(
    const TqAcl& acl,
    const std::string& host,
    uint16_t port,
    std::vector<sockaddr_storage>& out_addrs) {
    out_addrs.clear();

    const auto deny = tq_acl_internal::ParseCidrList(acl.DenyCidrs);
    const auto allow = tq_acl_internal::ParseCidrList(acl.AllowCidrs);

    sockaddr_storage literal{};
    if (tq_acl_internal::ParseIpLiteral(host, literal)) {
        tq_acl_internal::SetSockaddrPort(literal, port);
        std::vector<sockaddr_storage> candidates{literal};
        if (!tq_acl_internal::EvaluateCandidates(deny, allow, candidates, out_addrs)) {
            out_addrs.clear();
            return false;
        }
        for (auto& addr : out_addrs) {
            tq_acl_internal::SetSockaddrPort(addr, port);
        }
        return true;
    }

    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;

    const std::string port_str = std::to_string(port);
    addrinfo* result = nullptr;
    if (getaddrinfo(host.c_str(), port_str.c_str(), &hints, &result) != 0) {
        return false;
    }

    std::vector<sockaddr_storage> candidates;
    for (addrinfo* ai = result; ai != nullptr; ai = ai->ai_next) {
        if (ai->ai_addrlen > static_cast<int>(sizeof(sockaddr_storage))) {
            continue;
        }
        sockaddr_storage ss{};
        std::memcpy(&ss, ai->ai_addr, ai->ai_addrlen);
        candidates.push_back(ss);
    }
    freeaddrinfo(result);

    if (candidates.empty()) {
        return false;
    }

    if (!tq_acl_internal::EvaluateCandidates(deny, allow, candidates, out_addrs)) {
        out_addrs.clear();
        return false;
    }

    for (auto& addr : out_addrs) {
        tq_acl_internal::SetSockaddrPort(addr, port);
    }
    return true;
}
