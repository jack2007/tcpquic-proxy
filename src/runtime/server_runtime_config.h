#pragma once

#include "acl.h"
#include "config.h"

#include <mutex>
#include <string>
#include <vector>

struct TqServerConfigPatch {
    bool HasAllowTargets{false};
    bool HasDenyTargets{false};
    std::vector<std::string> AllowTargets;
    std::vector<std::string> DenyTargets;
};

bool TqParseServerConfigPatch(
    const std::string& body,
    TqServerConfigPatch& patch,
    std::string& err,
    bool& unsupported);

class TqServerRuntimeConfigState {
public:
    explicit TqServerRuntimeConfigState(const TqConfig& initial);

    TqConfig SnapshotConfig() const;
    TqAcl SnapshotAcl() const;
    bool BuildAclPatch(
        const TqServerConfigPatch& patch,
        TqConfig& nextConfig,
        TqAcl& nextAcl,
        std::string& err) const;
    void Commit(const TqConfig& nextConfig, const TqAcl& nextAcl);

private:
    mutable std::mutex Lock;
    TqConfig Config;
    TqAcl Acl;
};
