#include "../acl.h"

#include <arpa/inet.h>
#include <cassert>
#include <cstring>
#include <string>
#include <vector>

namespace {

bool SockaddrIpv4Equals(const sockaddr_storage& ss, const char* dotted) {
    if (ss.ss_family != AF_INET) {
        return false;
    }
    const auto* sin = reinterpret_cast<const sockaddr_in*>(&ss);
    in_addr expected{};
    if (inet_pton(AF_INET, dotted, &expected) != 1) {
        return false;
    }
    return std::memcmp(&sin->sin_addr, &expected, sizeof(expected)) == 0;
}

} // namespace

int main() {
    TqAcl acl;
    acl.AllowCidrs = {"10.0.0.0/8", "192.168.1.0/24"};
    acl.DenyCidrs = {"10.0.0.1/32"};

    assert(acl.IsAllowed("10.0.0.50", 80));
    assert(!acl.IsAllowed("10.0.0.1", 80));
    assert(!acl.IsAllowed("8.8.8.8", 53));

    // Deny overrides allow for the same address.
    TqAcl overlap;
    overlap.AllowCidrs = {"10.0.0.0/8"};
    overlap.DenyCidrs = {"10.0.0.1/32"};
    assert(!overlap.IsAllowed("10.0.0.1", 443));

    // Empty allow list rejects everything (secure default).
    TqAcl empty_allow;
    empty_allow.DenyCidrs = {};
    assert(!empty_allow.IsAllowed("10.0.0.50", 80));

    // IP literal resolve+filter path.
    std::vector<sockaddr_storage> addrs;
    assert(TqAclResolveAndFilter(acl, "10.0.0.50", 8080, addrs));
    assert(addrs.size() == 1);
    assert(SockaddrIpv4Equals(addrs[0], "10.0.0.50"));
    assert(ntohs(reinterpret_cast<const sockaddr_in*>(&addrs[0])->sin_port) == 8080);

    addrs.clear();
    assert(!TqAclResolveAndFilter(acl, "10.0.0.1", 80, addrs));
    assert(addrs.empty());

    addrs.clear();
    assert(!TqAclResolveAndFilter(acl, "8.8.8.8", 53, addrs));
    assert(addrs.empty());

    // Domain resolution: localhost should map to loopback when allowed.
    TqAcl local_acl;
    local_acl.AllowCidrs = {"127.0.0.0/8"};
    addrs.clear();
    if (TqAclResolveAndFilter(local_acl, "localhost", 80, addrs)) {
        assert(!addrs.empty());
        bool has_loopback = false;
        for (const auto& addr : addrs) {
            if (addr.ss_family == AF_INET) {
                const auto* sin = reinterpret_cast<const sockaddr_in*>(&addr);
                const uint32_t ip = ntohl(sin->sin_addr.s_addr);
                if ((ip & 0xFF000000u) == 0x7F000000u) {
                    has_loopback = true;
                }
            }
        }
        assert(has_loopback);
    }

    // If a domain resolves to any denied address, reject entirely.
    TqAcl deny_loopback;
    deny_loopback.AllowCidrs = {"10.0.0.0/8", "127.0.0.0/8"};
    deny_loopback.DenyCidrs = {"127.0.0.1/32"};
    addrs.clear();
    assert(!TqAclResolveAndFilter(deny_loopback, "localhost", 80, addrs));

    std::string err;
    assert(TqValidateCidrList({"10.0.0.0/8", "192.168.1.0/24"}, err));
    assert(err.empty());
    assert(!TqValidateCidrList({"not-a-cidr"}, err));
    assert(!err.empty());
    assert(!TqValidateCidrList({"10.0.0.0/33"}, err));
    assert(!err.empty());

    return 0;
}
