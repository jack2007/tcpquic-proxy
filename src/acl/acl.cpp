#include "acl.h"

#include "platform_socket.h"

#if !defined(_WIN32)
#include <arpa/inet.h>
#endif
#include <cctype>
#include <cstring>
#if !defined(_WIN32)
#include <netdb.h>
#endif
#include <string>
#include <vector>

namespace {

struct ParsedCidr {
    int Family{AF_UNSPEC}; // AF_INET or AF_INET6
    uint8_t Addr[16]{};
    uint8_t PrefixLen{};
};

bool ParsePrefix(const std::string& s, int& out) {
    if (s.empty()) {
        return false;
    }
    int value = 0;
    for (char c : s) {
        if (!std::isdigit(static_cast<unsigned char>(c))) {
            return false;
        }
        value = value * 10 + (c - '0');
        if (value > 128) {
            return false;
        }
    }
    out = value;
    return true;
}

bool ParseCidr(const std::string& cidr, ParsedCidr& out) {
    const auto slash = cidr.find('/');
    if (slash == std::string::npos) {
        return false;
    }

    const std::string addr_str = cidr.substr(0, slash);
    int prefix = 0;
    if (!ParsePrefix(cidr.substr(slash + 1), prefix)) {
        return false;
    }

    in_addr v4{};
    if (TqInetPton(AF_INET, addr_str.c_str(), &v4)) {
        if (prefix < 0 || prefix > 32) {
            return false;
        }
        out.Family = AF_INET;
        std::memset(out.Addr, 0, sizeof(out.Addr));
        std::memcpy(out.Addr, &v4, 4);
        out.PrefixLen = static_cast<uint8_t>(prefix);
        return true;
    }

    in6_addr v6{};
    if (TqInetPton(AF_INET6, addr_str.c_str(), &v6)) {
        if (prefix < 0 || prefix > 128) {
            return false;
        }
        out.Family = AF_INET6;
        std::memcpy(out.Addr, &v6, 16);
        out.PrefixLen = static_cast<uint8_t>(prefix);
        return true;
    }

    return false;
}

bool IpMatchesCidr(int family, const uint8_t* addr_bytes, const ParsedCidr& cidr) {
    if (family != cidr.Family) {
        return false;
    }

    const int byte_count = (family == AF_INET) ? 4 : 16;
    const uint8_t* network = cidr.Addr;
    const int prefix = cidr.PrefixLen;
    const int full_bytes = prefix / 8;
    const int rem_bits = prefix % 8;

    for (int i = 0; i < full_bytes; ++i) {
        if (i >= byte_count) {
            return false;
        }
        if (addr_bytes[i] != network[i]) {
            return false;
        }
    }

    if (rem_bits == 0) {
        return true;
    }

    if (full_bytes >= byte_count) {
        return false;
    }

    const uint8_t mask = static_cast<uint8_t>(0xFF << (8 - rem_bits));
    return (addr_bytes[full_bytes] & mask) == (network[full_bytes] & mask);
}

bool ExtractIpBytes(const sockaddr_storage& ss, int& family, uint8_t addr[16]) {
    std::memset(addr, 0, 16);
    if (ss.ss_family == AF_INET) {
        const auto* sin = reinterpret_cast<const sockaddr_in*>(&ss);
        family = AF_INET;
        std::memcpy(addr, &sin->sin_addr, 4);
        return true;
    }
    if (ss.ss_family == AF_INET6) {
        const auto* sin6 = reinterpret_cast<const sockaddr_in6*>(&ss);
        family = AF_INET6;
        std::memcpy(addr, &sin6->sin6_addr, 16);
        return true;
    }
    return false;
}

bool ParseIpLiteral(const std::string& host, sockaddr_storage& out) {
    std::memset(&out, 0, sizeof(out));

    in_addr v4{};
    if (TqInetPton(AF_INET, host.c_str(), &v4)) {
        auto* sin = reinterpret_cast<sockaddr_in*>(&out);
        out.ss_family = AF_INET;
        sin->sin_addr = v4;
        return true;
    }

    in6_addr v6{};
    if (TqInetPton(AF_INET6, host.c_str(), &v6)) {
        auto* sin6 = reinterpret_cast<sockaddr_in6*>(&out);
        out.ss_family = AF_INET6;
        sin6->sin6_addr = v6;
        return true;
    }

    return false;
}

void SetSockaddrPort(sockaddr_storage& ss, uint16_t port) {
    const uint16_t net_port = htons(port);
    if (ss.ss_family == AF_INET) {
        reinterpret_cast<sockaddr_in*>(&ss)->sin_port = net_port;
    } else if (ss.ss_family == AF_INET6) {
        reinterpret_cast<sockaddr_in6*>(&ss)->sin6_port = net_port;
    }
}

std::vector<ParsedCidr> ParseCidrList(const std::vector<std::string>& cidrs) {
    std::vector<ParsedCidr> parsed;
    parsed.reserve(cidrs.size());
    for (const auto& cidr : cidrs) {
        ParsedCidr p{};
        if (ParseCidr(cidr, p)) {
            parsed.push_back(p);
        }
    }
    return parsed;
}

bool IpBytesAllowed(
    int family,
    const uint8_t addr[16],
    const std::vector<ParsedCidr>& deny,
    const std::vector<ParsedCidr>& allow) {
    for (const auto& cidr : deny) {
        if (IpMatchesCidr(family, addr, cidr)) {
            return false;
        }
    }

    if (allow.empty()) {
        return false;
    }

    for (const auto& cidr : allow) {
        if (IpMatchesCidr(family, addr, cidr)) {
            return true;
        }
    }

    return false;
}

bool EvaluateCandidates(
    const std::vector<ParsedCidr>& deny,
    const std::vector<ParsedCidr>& allow,
    const std::vector<sockaddr_storage>& candidates,
    std::vector<sockaddr_storage>& out_allowed) {
    out_allowed.clear();

    if (allow.empty()) {
        return false;
    }

    // Any denied candidate rejects the entire target (spec §5.1).
    for (const auto& candidate : candidates) {
        int family = AF_UNSPEC;
        uint8_t addr[16]{};
        if (!ExtractIpBytes(candidate, family, addr)) {
            continue;
        }
        for (const auto& cidr : deny) {
            if (IpMatchesCidr(family, addr, cidr)) {
                return false;
            }
        }
    }

    for (const auto& candidate : candidates) {
        int family = AF_UNSPEC;
        uint8_t addr[16]{};
        if (!ExtractIpBytes(candidate, family, addr)) {
            continue;
        }
        if (IpBytesAllowed(family, addr, {}, allow)) {
            out_allowed.push_back(candidate);
        }
    }

    return !out_allowed.empty();
}

} // namespace

bool TqValidateCidrList(const std::vector<std::string>& cidrs, std::string& err) {
    err.clear();
    for (const auto& cidr : cidrs) {
        if (cidr.empty()) {
            err = "empty CIDR entry";
            return false;
        }
        ParsedCidr parsed{};
        if (!ParseCidr(cidr, parsed)) {
            err = "invalid CIDR: " + cidr;
            return false;
        }
    }
    return true;
}

bool TqAcl::IsAllowed(const std::string& host, uint16_t /*port*/) const {
    sockaddr_storage ss{};
    if (!ParseIpLiteral(host, ss)) {
        return false;
    }

    int family = AF_UNSPEC;
    uint8_t addr[16]{};
    if (!ExtractIpBytes(ss, family, addr)) {
        return false;
    }

    const auto deny = ParseCidrList(DenyCidrs);
    const auto allow = ParseCidrList(AllowCidrs);
    return IpBytesAllowed(family, addr, deny, allow);
}

bool TqAclResolveAndFilter(
    const TqAcl& acl,
    const std::string& host,
    uint16_t port,
    std::vector<sockaddr_storage>& out_addrs) {
    out_addrs.clear();

    const auto deny = ParseCidrList(acl.DenyCidrs);
    const auto allow = ParseCidrList(acl.AllowCidrs);

    sockaddr_storage literal{};
    if (ParseIpLiteral(host, literal)) {
        SetSockaddrPort(literal, port);
        std::vector<sockaddr_storage> candidates{literal};
        if (!EvaluateCandidates(deny, allow, candidates, out_addrs)) {
            out_addrs.clear();
            return false;
        }
        for (auto& addr : out_addrs) {
            SetSockaddrPort(addr, port);
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

    if (!EvaluateCandidates(deny, allow, candidates, out_addrs)) {
        out_addrs.clear();
        return false;
    }

    for (auto& addr : out_addrs) {
        SetSockaddrPort(addr, port);
    }
    return true;
}
