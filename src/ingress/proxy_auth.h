#pragma once

#include "config.h"

#include <cstddef>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

class TqProxyAuthTable {
public:
    static constexpr size_t kMaxUsers = 64;
    static constexpr size_t kMaxFieldBytes = 255;

    TqProxyAuthTable() = default;
    explicit TqProxyAuthTable(std::vector<TqProxyAuthUser> users);

    bool Enabled() const { return !Users_.empty(); }
    bool Validate(std::string_view user, std::string_view pass) const;

private:
    std::vector<TqProxyAuthUser> Users_;
};

bool TqConstantTimeEquals(std::string_view left, std::string_view right);
bool TqBase64Decode(std::string_view input, std::string& out);
bool TqFindHttpHeaderValue(const std::string& request, std::string_view headerName, std::string_view& valueOut);
bool TqParseHttpProxyAuthorization(std::string_view headerValue, std::string& user, std::string& pass);

enum class TqHttpConnectAuthResult {
    Authorized,
    Disabled,
    MissingHeader,
    InvalidHeader,
    InvalidCredentials,
};

TqHttpConnectAuthResult TqHttpConnectRequestAuthResult(const std::string& request, const TqProxyAuthTable& auth);
bool TqHttpConnectRequestAuthorized(const std::string& request, const TqProxyAuthTable& auth);

bool TqValidateProxyAuthUsers(const std::vector<TqProxyAuthUser>& users, std::string& err);
