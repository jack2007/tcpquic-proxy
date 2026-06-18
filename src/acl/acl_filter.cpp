#include "acl_filter.h"

#include "acl_matcher.h"

bool TqAclFilterResolvedAddresses(
    const TqAcl& acl,
    const std::vector<sockaddr_storage>& candidates,
    uint16_t port,
    std::vector<sockaddr_storage>& outAddrs) {
    outAddrs.clear();

    const auto deny = tq_acl_internal::ParseCidrList(acl.DenyCidrs);
    const auto allow = tq_acl_internal::ParseCidrList(acl.AllowCidrs);
    if (!tq_acl_internal::EvaluateCandidates(deny, allow, candidates, outAddrs)) {
        outAddrs.clear();
        return false;
    }

    for (auto& addr : outAddrs) {
        tq_acl_internal::SetSockaddrPort(addr, port);
    }
    return true;
}
