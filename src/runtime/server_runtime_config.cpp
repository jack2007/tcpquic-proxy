#include "server_runtime_config.h"

#include <nlohmann/json.hpp>

#include <utility>

namespace {

void SplitCommaList(const std::string& value, std::vector<std::string>& out) {
    out.clear();
    size_t start = 0;
    while (start <= value.size()) {
        size_t comma = value.find(',', start);
        if (comma == std::string::npos) {
            comma = value.size();
        }
        const std::string item = value.substr(start, comma - start);
        const size_t begin = item.find_first_not_of(" \t");
        const size_t end = item.find_last_not_of(" \t");
        if (begin != std::string::npos) {
            out.push_back(item.substr(begin, end - begin + 1));
        }
        start = comma + 1;
    }
}

bool ReadStringList(const nlohmann::json& value, std::vector<std::string>& out) {
    if (value.is_string()) {
        SplitCommaList(value.get<std::string>(), out);
        return true;
    }
    if (!value.is_array()) {
        return false;
    }
    std::vector<std::string> next;
    next.reserve(value.size());
    for (const auto& item : value) {
        if (!item.is_string()) {
            return false;
        }
        next.push_back(item.get<std::string>());
    }
    out = std::move(next);
    return true;
}

bool IsStartupField(const std::string& key) {
    return key == "listen" ||
        key == "resolved_listens" ||
        key == "tls" ||
        key == "quic" ||
        key == "proto" ||
        key == "relay" ||
        key == "admin";
}

bool Error(std::string& err, bool& unsupported, const std::string& message) {
    err = message;
    unsupported = false;
    return false;
}

bool Unsupported(std::string& err, bool& unsupported, const std::string& field) {
    err = "server runtime field " + field + " requires process restart";
    unsupported = true;
    return false;
}

} // namespace

bool TqParseServerConfigPatch(
    const std::string& body,
    TqServerConfigPatch& patch,
    std::string& err,
    bool& unsupported) {
    patch = TqServerConfigPatch{};
    err.clear();
    unsupported = false;

    nlohmann::json root;
    try {
        root = nlohmann::json::parse(body);
    } catch (const nlohmann::json::exception&) {
        return Error(err, unsupported, "malformed server config patch object");
    }
    if (!root.is_object()) {
        return Error(err, unsupported, "server config patch must be an object");
    }

    for (const auto& item : root.items()) {
        const std::string& key = item.key();
        if (key == "allow_targets") {
            if (!ReadStringList(item.value(), patch.AllowTargets)) {
                return Error(err, unsupported, "invalid allow_targets");
            }
            patch.HasAllowTargets = true;
        } else if (key == "deny_targets") {
            if (!ReadStringList(item.value(), patch.DenyTargets)) {
                return Error(err, unsupported, "invalid deny_targets");
            }
            patch.HasDenyTargets = true;
        } else if (IsStartupField(key)) {
            return Unsupported(err, unsupported, key);
        } else {
            return Error(err, unsupported, "unknown server config field: " + key);
        }
    }

    if (!patch.HasAllowTargets && !patch.HasDenyTargets) {
        return Error(err, unsupported, "server config patch must include allow_targets or deny_targets");
    }
    return true;
}

TqServerRuntimeConfigState::TqServerRuntimeConfigState(const TqConfig& initial)
    : Config(initial) {
    Acl.AllowCidrs = initial.AllowTargets;
    Acl.DenyCidrs = initial.DenyTargets;
}

TqConfig TqServerRuntimeConfigState::SnapshotConfig() const {
    std::lock_guard<std::mutex> guard(Lock);
    return Config;
}

TqAcl TqServerRuntimeConfigState::SnapshotAcl() const {
    std::lock_guard<std::mutex> guard(Lock);
    return Acl;
}

bool TqServerRuntimeConfigState::BuildAclPatch(
    const TqServerConfigPatch& patch,
    TqConfig& nextConfig,
    TqAcl& nextAcl,
    std::string& err) const {
    {
        std::lock_guard<std::mutex> guard(Lock);
        nextConfig = Config;
    }

    if (patch.HasAllowTargets) {
        nextConfig.AllowTargets = patch.AllowTargets;
    }
    if (patch.HasDenyTargets) {
        nextConfig.DenyTargets = patch.DenyTargets;
    }

    if (!TqValidateCidrList(nextConfig.AllowTargets, err)) {
        return false;
    }
    if (!TqValidateCidrList(nextConfig.DenyTargets, err)) {
        return false;
    }

    nextAcl.AllowCidrs = nextConfig.AllowTargets;
    nextAcl.DenyCidrs = nextConfig.DenyTargets;
    err.clear();
    return true;
}

void TqServerRuntimeConfigState::Commit(const TqConfig& nextConfig, const TqAcl& nextAcl) {
    std::lock_guard<std::mutex> guard(Lock);
    Config = nextConfig;
    Acl = nextAcl;
}
