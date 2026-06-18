#pragma once

#include "platform_socket.h"

#include <cctype>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace tq_acl_internal {

struct ParsedCidr {
    int Family{AF_UNSPEC}; // AF_INET or AF_INET6
    uint8_t Addr[16]{};
    uint8_t PrefixLen{};
};

inline bool ParsePrefix(const std::string& s, int& out) {
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

inline bool ParseCidr(const std::string& cidr, ParsedCidr& out) {
    const auto slash = cidr.find('/');
    if (slash == std::string::npos) {
        return false;
    }

    const std::string addrStr = cidr.substr(0, slash);
    int prefix = 0;
    if (!ParsePrefix(cidr.substr(slash + 1), prefix)) {
        return false;
    }

    in_addr v4{};
    if (TqInetPton(AF_INET, addrStr.c_str(), &v4)) {
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
    if (TqInetPton(AF_INET6, addrStr.c_str(), &v6)) {
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

inline std::vector<ParsedCidr> ParseCidrList(const std::vector<std::string>& cidrs) {
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

inline bool IpMatchesCidr(int family, const uint8_t* addrBytes, const ParsedCidr& cidr) {
    if (family != cidr.Family) {
        return false;
    }

    const int byteCount = (family == AF_INET) ? 4 : 16;
    const uint8_t* network = cidr.Addr;
    const int prefix = cidr.PrefixLen;
    const int fullBytes = prefix / 8;
    const int remBits = prefix % 8;

    for (int i = 0; i < fullBytes; ++i) {
        if (i >= byteCount) {
            return false;
        }
        if (addrBytes[i] != network[i]) {
            return false;
        }
    }

    if (remBits == 0) {
        return true;
    }

    if (fullBytes >= byteCount) {
        return false;
    }

    const uint8_t mask = static_cast<uint8_t>(0xFF << (8 - remBits));
    return (addrBytes[fullBytes] & mask) == (network[fullBytes] & mask);
}

inline bool ExtractIpBytes(const sockaddr_storage& ss, int& family, uint8_t addr[16]) {
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

inline bool ParseIpLiteral(const std::string& host, sockaddr_storage& out) {
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

inline void SetSockaddrPort(sockaddr_storage& ss, uint16_t port) {
    const uint16_t netPort = htons(port);
    if (ss.ss_family == AF_INET) {
        reinterpret_cast<sockaddr_in*>(&ss)->sin_port = netPort;
    } else if (ss.ss_family == AF_INET6) {
        reinterpret_cast<sockaddr_in6*>(&ss)->sin6_port = netPort;
    }
}

inline bool IpBytesAllowed(
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

inline bool EvaluateCandidates(
    const std::vector<ParsedCidr>& deny,
    const std::vector<ParsedCidr>& allow,
    const std::vector<sockaddr_storage>& candidates,
    std::vector<sockaddr_storage>& outAllowed) {
    outAllowed.clear();

    if (allow.empty()) {
        return false;
    }

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
            outAllowed.push_back(candidate);
        }
    }

    return !outAllowed.empty();
}

} // namespace tq_acl_internal
