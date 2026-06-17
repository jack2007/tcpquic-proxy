#include "proxy_auth.h"

#include <cctype>
#include <set>

namespace {

bool TqBase64Value(int ch) {
    if (ch >= 'A' && ch <= 'Z') {
        return true;
    }
    if (ch >= 'a' && ch <= 'z') {
        return true;
    }
    if (ch >= '0' && ch <= '9') {
        return true;
    }
    return ch == '+' || ch == '/';
}

int TqBase64DecodeByte(char ch) {
    if (ch >= 'A' && ch <= 'Z') {
        return ch - 'A';
    }
    if (ch >= 'a' && ch <= 'z') {
        return ch - 'a' + 26;
    }
    if (ch >= '0' && ch <= '9') {
        return ch - '0' + 52;
    }
    if (ch == '+') {
        return 62;
    }
    if (ch == '/') {
        return 63;
    }
    return -1;
}

bool TqEqualsIgnoreCase(std::string_view left, std::string_view right) {
    if (left.size() != right.size()) {
        return false;
    }
    for (size_t i = 0; i < left.size(); ++i) {
        const unsigned char a = static_cast<unsigned char>(left[i]);
        const unsigned char b = static_cast<unsigned char>(right[i]);
        if (std::tolower(a) != std::tolower(b)) {
            return false;
        }
    }
    return true;
}

} // namespace

TqProxyAuthTable::TqProxyAuthTable(std::vector<TqProxyAuthUser> users) : Users_(std::move(users)) {}

bool TqConstantTimeEquals(std::string_view left, std::string_view right) {
    if (left.size() != right.size()) {
        return false;
    }
    unsigned char diff = 0;
    for (size_t i = 0; i < left.size(); ++i) {
        diff |= static_cast<unsigned char>(left[i] ^ right[i]);
    }
    return diff == 0;
}

bool TqBase64Decode(std::string_view input, std::string& out) {
    out.clear();
    if (input.empty()) {
        return false;
    }

    size_t padding = 0;
    if (input.size() >= 1 && input[input.size() - 1] == '=') {
        ++padding;
        if (input.size() >= 2 && input[input.size() - 2] == '=') {
            ++padding;
        }
    }
    if (padding > 2) {
        return false;
    }

    std::string cleaned;
    cleaned.reserve(input.size());
    for (char ch : input) {
        if (ch == '=') {
            cleaned.push_back(ch);
        } else if (TqBase64Value(static_cast<unsigned char>(ch))) {
            cleaned.push_back(ch);
        } else if (ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n') {
            continue;
        } else {
            return false;
        }
    }
    if (cleaned.empty() || (cleaned.size() % 4) != 0) {
        return false;
    }

    out.reserve((cleaned.size() / 4) * 3);
    for (size_t i = 0; i < cleaned.size(); i += 4) {
        const int b0 = TqBase64DecodeByte(cleaned[i]);
        const int b1 = TqBase64DecodeByte(cleaned[i + 1]);
        const int b2 = cleaned[i + 2] == '=' ? 0 : TqBase64DecodeByte(cleaned[i + 2]);
        const int b3 = cleaned[i + 3] == '=' ? 0 : TqBase64DecodeByte(cleaned[i + 3]);
        if (b0 < 0 || b1 < 0 || (cleaned[i + 2] != '=' && b2 < 0) || (cleaned[i + 3] != '=' && b3 < 0)) {
            return false;
        }

        const uint32_t triple =
            (static_cast<uint32_t>(b0) << 18) |
            (static_cast<uint32_t>(b1) << 12) |
            (static_cast<uint32_t>(b2) << 6) |
            static_cast<uint32_t>(b3);

        out.push_back(static_cast<char>((triple >> 16) & 0xFF));
        if (cleaned[i + 2] != '=') {
            out.push_back(static_cast<char>((triple >> 8) & 0xFF));
        }
        if (cleaned[i + 3] != '=') {
            out.push_back(static_cast<char>(triple & 0xFF));
        }
    }
    return !out.empty();
}

bool TqFindHttpHeaderValue(const std::string& request, std::string_view headerName, std::string_view& valueOut) {
    valueOut = {};
    const size_t headerEnd = request.find("\r\n\r\n");
    if (headerEnd == std::string::npos) {
        return false;
    }

    size_t pos = request.find("\r\n");
    if (pos == std::string::npos) {
        return false;
    }
    pos += 2;

    while (pos < headerEnd) {
        const size_t lineEnd = request.find("\r\n", pos);
        if (lineEnd == std::string::npos || lineEnd > headerEnd) {
            break;
        }
        const std::string_view line(request.data() + pos, lineEnd - pos);
        const size_t colon = line.find(':');
        if (colon != std::string::npos) {
            const std::string_view name = line.substr(0, colon);
            if (TqEqualsIgnoreCase(name, headerName)) {
                size_t valueStart = colon + 1;
                while (valueStart < line.size() &&
                       (line[valueStart] == ' ' || line[valueStart] == '\t')) {
                    ++valueStart;
                }
                valueOut = line.substr(valueStart);
                return true;
            }
        }
        pos = lineEnd + 2;
    }
    return false;
}

bool TqParseHttpProxyAuthorization(std::string_view headerValue, std::string& user, std::string& pass) {
    user.clear();
    pass.clear();

    constexpr std::string_view kBasic = "Basic ";
    if (headerValue.size() < kBasic.size() ||
        !TqEqualsIgnoreCase(headerValue.substr(0, kBasic.size()), kBasic)) {
        return false;
    }

    std::string decoded;
    if (!TqBase64Decode(headerValue.substr(kBasic.size()), decoded)) {
        return false;
    }

    const size_t colon = decoded.find(':');
    if (colon == std::string::npos || colon == 0) {
        return false;
    }

    user.assign(decoded, 0, colon);
    pass.assign(decoded, colon + 1, std::string::npos);
    if (user.empty() || user.size() > TqProxyAuthTable::kMaxFieldBytes ||
        pass.size() > TqProxyAuthTable::kMaxFieldBytes) {
        user.clear();
        pass.clear();
        return false;
    }
    return true;
}

bool TqProxyAuthTable::Validate(std::string_view user, std::string_view pass) const {
    if (!Enabled()) {
        return true;
    }
    if (user.empty() || pass.empty()) {
        return false;
    }

    bool matched = false;
    for (const TqProxyAuthUser& entry : Users_) {
        const bool userOk = TqConstantTimeEquals(entry.Username, user);
        const bool passOk = TqConstantTimeEquals(entry.Password, pass);
        matched = matched || (userOk && passOk);
    }
    return matched;
}

TqHttpConnectAuthResult TqHttpConnectRequestAuthResult(const std::string& request, const TqProxyAuthTable& auth) {
    if (!auth.Enabled()) {
        return TqHttpConnectAuthResult::Disabled;
    }

    std::string_view authHeader;
    if (!TqFindHttpHeaderValue(request, "Proxy-Authorization", authHeader)) {
        return TqHttpConnectAuthResult::MissingHeader;
    }

    std::string user;
    std::string pass;
    if (!TqParseHttpProxyAuthorization(authHeader, user, pass)) {
        return TqHttpConnectAuthResult::InvalidHeader;
    }
    if (!auth.Validate(user, pass)) {
        return TqHttpConnectAuthResult::InvalidCredentials;
    }
    return TqHttpConnectAuthResult::Authorized;
}

bool TqHttpConnectRequestAuthorized(const std::string& request, const TqProxyAuthTable& auth) {
    const TqHttpConnectAuthResult result = TqHttpConnectRequestAuthResult(request, auth);
    return result == TqHttpConnectAuthResult::Authorized ||
           result == TqHttpConnectAuthResult::Disabled;
}

bool TqValidateProxyAuthUsers(const std::vector<TqProxyAuthUser>& users, std::string& err) {
    if (users.size() > TqProxyAuthTable::kMaxUsers) {
        err = "proxy_auth exceeds maximum user count";
        return false;
    }

    std::set<std::string> seen;
    for (const TqProxyAuthUser& user : users) {
        if (user.Username.empty() || user.Password.empty()) {
            err = "proxy_auth username and password must be non-empty";
            return false;
        }
        if (user.Username.size() > TqProxyAuthTable::kMaxFieldBytes ||
            user.Password.size() > TqProxyAuthTable::kMaxFieldBytes) {
            err = "proxy_auth username or password too long";
            return false;
        }
        if (!seen.insert(user.Username).second) {
            err = "duplicate proxy_auth username: " + user.Username;
            return false;
        }
    }
    return true;
}
