#pragma once

#include <cstdint>
#include <vector>

#include "acl.h"

bool TqAclFilterResolvedAddresses(
    const TqAcl& acl,
    const std::vector<sockaddr_storage>& candidates,
    uint16_t port,
    std::vector<sockaddr_storage>& outAddrs);
