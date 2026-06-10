#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "platform_socket.h"

struct TqAcl {
    std::vector<std::string> AllowCidrs;
    std::vector<std::string> DenyCidrs;

    // Returns true when host is an IP literal that passes ACL (deny first, then allow).
    // Domains must use TqAclResolveAndFilter.
    bool IsAllowed(const std::string& host, uint16_t port) const;
};

// Resolves host (IP literal or domain via getaddrinfo), applies ACL to every A/AAAA
// candidate, and returns only addresses permitted for TCP dial.
// Returns false when: resolution fails, any candidate matches deny, allow list is
// empty, or no candidate matches allow.
bool TqAclResolveAndFilter(
    const TqAcl& acl,
    const std::string& host,
    uint16_t port,
    std::vector<sockaddr_storage>& out_addrs);

// Validates every CIDR string (e.g. "10.0.0.0/8"). Returns false and sets err
// when any entry is malformed.
bool TqValidateCidrList(const std::vector<std::string>& cidrs, std::string& err);
