#include "admin_auth.h"

#include <nlohmann/json.hpp>

#include <array>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <random>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <process.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace {

std::mutex& TqRuntimeBinaryNameMutex() {
    static std::mutex m;
    return m;
}

std::string& TqRuntimeBinaryNameStorage() {
    static std::string name;
    return name;
}

std::string TqBaseNameFromArgv0(const char* argv0) {
    if (argv0 == nullptr || argv0[0] == '\0') {
        return {};
    }
    std::string path(argv0);
    const size_t slash = path.find_last_of("/\\");
    std::string name = slash == std::string::npos ? path : path.substr(slash + 1);
    if (name.size() > 4) {
        const size_t ext = name.size() - 4;
        if (name[ext] == '.' &&
            (name[ext + 1] == 'e' || name[ext + 1] == 'E') &&
            (name[ext + 2] == 'x' || name[ext + 2] == 'X') &&
            (name[ext + 3] == 'e' || name[ext + 3] == 'E')) {
            name.resize(ext);
        }
    }
    return name.empty() ? std::string{} : name;
}

std::string TqRuntimeBinaryName() {
    std::lock_guard<std::mutex> lock(TqRuntimeBinaryNameMutex());
    const std::string& name = TqRuntimeBinaryNameStorage();
    return name.empty() ? "tcpquic-proxy" : name;
}

std::string TqHexEncode(const unsigned char* data, size_t len) {
    static constexpr char kHex[] = "0123456789abcdef";
    std::string out;
    out.resize(len * 2);
    for (size_t i = 0; i < len; ++i) {
        out[i * 2] = kHex[data[i] >> 4];
        out[i * 2 + 1] = kHex[data[i] & 0x0f];
    }
    return out;
}

bool TqFillRandom(unsigned char* data, size_t len) {
#if !defined(_WIN32)
    int fd = ::open("/dev/urandom", O_RDONLY);
    if (fd < 0) {
        return false;
    }
    size_t off = 0;
    while (off < len) {
        const ssize_t got = ::read(fd, data + off, len - off);
        if (got < 0) {
            if (errno == EINTR) {
                continue;
            }
            ::close(fd);
            return false;
        }
        if (got == 0) {
            ::close(fd);
            return false;
        }
        off += static_cast<size_t>(got);
    }
    ::close(fd);
    return true;
#else
    std::random_device rd;
    for (size_t i = 0; i < len; ++i) {
        data[i] = static_cast<unsigned char>(rd());
    }
    return true;
#endif
}

uint64_t TqCurrentPid() {
#if defined(_WIN32)
    return static_cast<uint64_t>(_getpid());
#else
    return static_cast<uint64_t>(::getpid());
#endif
}

std::string TqDefaultTokenFileName(const std::string& role) {
    const bool validRole = role == "client" || role == "server";
    return (validRole ? role + "-" : std::string{}) +
        "admin-" + std::to_string(TqCurrentPid()) + ".json";
}

uint64_t TqUnixNow() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count());
}

std::string TqTokenJson(const std::string& token, const std::string& listen) {
    return nlohmann::json{
        {"version", 1},
        {"token_type", "Bearer"},
        {"token", token},
        {"listen", listen},
        {"pid", TqCurrentPid()},
        {"created_at_unix", TqUnixNow()},
    }.dump(2) + "\n";
}

bool TqConstantTimeEquals(const std::string& a, const std::string& b) {
    const size_t maxLen = a.size() > b.size() ? a.size() : b.size();
    unsigned char diff = static_cast<unsigned char>(a.size() ^ b.size());
    for (size_t i = 0; i < maxLen; ++i) {
        const unsigned char ca = i < a.size() ? static_cast<unsigned char>(a[i]) : 0;
        const unsigned char cb = i < b.size() ? static_cast<unsigned char>(b[i]) : 0;
        diff |= static_cast<unsigned char>(ca ^ cb);
    }
    return diff == 0;
}

bool TqIsTokenHex(const std::string& token) {
    if (token.size() != 64) {
        return false;
    }
    for (char ch : token) {
        const bool digit = ch >= '0' && ch <= '9';
        const bool lower = ch >= 'a' && ch <= 'f';
        const bool upper = ch >= 'A' && ch <= 'F';
        if (!digit && !lower && !upper) {
            return false;
        }
    }
    return true;
}

bool TqFileContainsTokenOrPid(const std::string& path, const std::string& token) {
    std::ifstream in(path);
    if (!in) {
        return false;
    }
    std::string body((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    try {
        const auto json = nlohmann::json::parse(body);
        if (!token.empty() && json.value("token", std::string{}) == token) {
            return true;
        }
        return json.value("pid", uint64_t{0}) == TqCurrentPid();
    } catch (const nlohmann::json::exception&) {
        return false;
    }
}

class TqTokenJsonParser {
public:
    explicit TqTokenJsonParser(const std::string& text, std::string& err) : Text(text), Err(err) {}

    bool Parse(std::string& token) {
        token.clear();
        bool hasVersion = false;
        bool hasType = false;
        bool hasToken = false;
        uint64_t version = 0;
        std::string tokenType;

        SkipWs();
        if (!Consume('{')) {
            return Error("token file must be a JSON object");
        }
        SkipWs();
        if (Consume('}')) {
            return Error("token file is missing required fields");
        }
        for (;;) {
            std::string key;
            if (!ParseString(key)) {
                return false;
            }
            SkipWs();
            if (!Consume(':')) {
                return Error("expected ':' after token file key");
            }
            SkipWs();
            if (key == "version") {
                if (!ParseUint(version)) {
                    return false;
                }
                hasVersion = true;
            } else if (key == "token_type") {
                if (!ParseString(tokenType)) {
                    return false;
                }
                hasType = true;
            } else if (key == "token") {
                if (!ParseString(token)) {
                    return false;
                }
                hasToken = true;
            } else {
                if (!SkipValue()) {
                    return false;
                }
            }
            SkipWs();
            if (Consume('}')) {
                break;
            }
            if (!Consume(',')) {
                return Error("expected ',' or '}' in token file");
            }
            SkipWs();
        }
        SkipWs();
        if (Pos != Text.size()) {
            return Error("unexpected data after token file JSON");
        }
        if (!hasVersion || !hasType || !hasToken) {
            return Error("token file is missing required fields");
        }
        if (version != 1) {
            return Error("unsupported token file version");
        }
        if (tokenType != "Bearer") {
            return Error("unsupported token type");
        }
        if (!TqIsTokenHex(token)) {
            return Error("invalid token value");
        }
        return true;
    }

private:
    bool ParseString(std::string& value) {
        value.clear();
        if (!Consume('"')) {
            return Error("expected JSON string");
        }
        while (Pos < Text.size()) {
            const char ch = Text[Pos++];
            if (ch == '"') {
                return true;
            }
            if (static_cast<unsigned char>(ch) < 0x20) {
                return Error("invalid control character in JSON string");
            }
            if (ch != '\\') {
                value.push_back(ch);
                continue;
            }
            if (Pos >= Text.size()) {
                return Error("unterminated escape sequence");
            }
            const char esc = Text[Pos++];
            switch (esc) {
            case '"':
            case '\\':
            case '/':
                value.push_back(esc);
                break;
            case 'b':
                value.push_back('\b');
                break;
            case 'f':
                value.push_back('\f');
                break;
            case 'n':
                value.push_back('\n');
                break;
            case 'r':
                value.push_back('\r');
                break;
            case 't':
                value.push_back('\t');
                break;
            case 'u':
                if (!ParseUnicodeEscape(value)) {
                    return false;
                }
                break;
            default:
                return Error("invalid escape sequence");
            }
        }
        return Error("unterminated JSON string");
    }

    bool ParseUnicodeEscape(std::string& value) {
        if (Pos + 4 > Text.size()) {
            return Error("invalid unicode escape");
        }
        uint32_t code = 0;
        for (int i = 0; i < 4; ++i) {
            const int hex = HexValue(Text[Pos++]);
            if (hex < 0) {
                return Error("invalid unicode escape");
            }
            code = (code << 4) | static_cast<uint32_t>(hex);
        }
        if (code <= 0x7f) {
            value.push_back(static_cast<char>(code));
        } else {
            return Error("non-ascii unicode escape is not supported in token file");
        }
        return true;
    }

    bool ParseUint(uint64_t& value) {
        if (Pos >= Text.size() || Text[Pos] < '0' || Text[Pos] > '9') {
            return Error("expected unsigned integer");
        }
        uint64_t parsed = 0;
        while (Pos < Text.size() && Text[Pos] >= '0' && Text[Pos] <= '9') {
            const uint64_t digit = static_cast<uint64_t>(Text[Pos] - '0');
            if (parsed > (UINT64_MAX - digit) / 10) {
                return Error("integer value is too large");
            }
            parsed = (parsed * 10) + digit;
            ++Pos;
        }
        value = parsed;
        return true;
    }

    bool SkipValue() {
        if (Pos >= Text.size()) {
            return Error("expected JSON value");
        }
        if (Text[Pos] == '"') {
            std::string ignored;
            return ParseString(ignored);
        }
        uint64_t ignoredNumber = 0;
        if (Text[Pos] >= '0' && Text[Pos] <= '9') {
            return ParseUint(ignoredNumber);
        }
        if (Text.compare(Pos, 4, "true") == 0) {
            Pos += 4;
            return true;
        }
        if (Text.compare(Pos, 5, "false") == 0) {
            Pos += 5;
            return true;
        }
        if (Text.compare(Pos, 4, "null") == 0) {
            Pos += 4;
            return true;
        }
        return Error("unsupported JSON value in token file");
    }

    bool Consume(char expected) {
        if (Pos < Text.size() && Text[Pos] == expected) {
            ++Pos;
            return true;
        }
        return false;
    }

    void SkipWs() {
        while (Pos < Text.size()) {
            const char ch = Text[Pos];
            if (ch != ' ' && ch != '\t' && ch != '\r' && ch != '\n') {
                return;
            }
            ++Pos;
        }
    }

    int HexValue(char ch) const {
        if (ch >= '0' && ch <= '9') return ch - '0';
        if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
        if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
        return -1;
    }

    bool Error(const std::string& err) {
        Err = err;
        return false;
    }

    const std::string& Text;
    std::string& Err;
    size_t Pos{0};
};

bool TqEnsureSecureTokenParentDir(const std::filesystem::path& parent, bool parentExisted, std::string& err) {
    if (parent.empty()) {
        return true;
    }

#if !defined(_WIN32)
    struct stat st {};
    if (::stat(parent.c_str(), &st) != 0) {
        err = "failed to stat admin token directory: " + std::string(std::strerror(errno));
        return false;
    }
    if (!S_ISDIR(st.st_mode)) {
        err = "admin token parent is not a directory";
        return false;
    }
    if (st.st_uid != ::getuid()) {
        err = "admin token directory must be owned by current user";
        return false;
    }
    if (parentExisted && ::chmod(parent.c_str(), 0700) != 0) {
        err = "failed to set admin token directory permissions: " + std::string(std::strerror(errno));
        return false;
    }
    if (::stat(parent.c_str(), &st) != 0) {
        err = "failed to stat admin token directory: " + std::string(std::strerror(errno));
        return false;
    }
    if ((st.st_mode & 0777) != 0700) {
        err = "admin token directory permissions must be 0700";
        return false;
    }
#else
    (void)parentExisted;
    (void)err;
#endif
    return true;
}

bool TqWriteFileAtomic(const std::filesystem::path& path, const std::string& body, std::string& err) {
    const std::filesystem::path parent = path.parent_path();
    std::error_code ec;
    bool parentExisted = false;
    if (!parent.empty()) {
        parentExisted = std::filesystem::exists(parent, ec);
        ec.clear();
        std::filesystem::create_directories(parent, ec);
        if (ec) {
            err = "failed to create admin token directory: " + ec.message();
            return false;
        }
        if (!parentExisted) {
            std::filesystem::permissions(parent,
                std::filesystem::perms::owner_all,
                std::filesystem::perm_options::replace,
                ec);
            if (ec) {
                err = "failed to set admin token directory permissions: " + ec.message();
                return false;
            }
        }
        if (!TqEnsureSecureTokenParentDir(parent, parentExisted, err)) {
            return false;
        }
    }

    const std::filesystem::path tmp = path.string() + ".tmp." + std::to_string(TqCurrentPid());
#if !defined(_WIN32)
    int fd = ::open(tmp.c_str(), O_CREAT | O_TRUNC | O_WRONLY, 0600);
    if (fd < 0) {
        err = "failed to open admin token file: " + std::string(std::strerror(errno));
        return false;
    }
    size_t off = 0;
    while (off < body.size()) {
        const ssize_t written = ::write(fd, body.data() + off, body.size() - off);
        if (written < 0) {
            if (errno == EINTR) {
                continue;
            }
            err = "failed to write admin token file: " + std::string(std::strerror(errno));
            ::close(fd);
            std::filesystem::remove(tmp, ec);
            return false;
        }
        off += static_cast<size_t>(written);
    }
    if (::fsync(fd) != 0) {
        err = "failed to sync admin token file: " + std::string(std::strerror(errno));
        ::close(fd);
        std::filesystem::remove(tmp, ec);
        return false;
    }
    if (::fchmod(fd, 0600) != 0) {
        err = "failed to chmod admin token file: " + std::string(std::strerror(errno));
        ::close(fd);
        std::filesystem::remove(tmp, ec);
        return false;
    }
    if (::close(fd) != 0) {
        err = "failed to close admin token file: " + std::string(std::strerror(errno));
        std::filesystem::remove(tmp, ec);
        return false;
    }
#else
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) {
            err = "failed to open admin token file";
            return false;
        }
        out << body;
        out.close();
        if (!out) {
            err = "failed to write admin token file";
            std::filesystem::remove(tmp, ec);
            return false;
        }
    }
#endif
    std::filesystem::rename(tmp, path, ec);
    if (ec) {
        err = "failed to publish admin token file: " + ec.message();
        std::filesystem::remove(tmp, ec);
        return false;
    }
    return true;
}

} // namespace

bool TqAdminAuth::InitializeToken() {
    std::array<unsigned char, 32> bytes{};
    if (!TqFillRandom(bytes.data(), bytes.size())) {
        return false;
    }
    TokenValue = TqHexEncode(bytes.data(), bytes.size());
    return true;
}

bool TqAdminAuth::Authorize(const TqHttpRequest& req) const {
    if (TokenValue.empty()) {
        return false;
    }
    auto it = req.Headers.find("authorization");
    if (it == req.Headers.end()) {
        return false;
    }
    static constexpr const char* kPrefix = "Bearer ";
    const std::string& value = it->second;
    if (value.compare(0, std::strlen(kPrefix), kPrefix) != 0) {
        return false;
    }
    return TqConstantTimeEquals(value.substr(std::strlen(kPrefix)), TokenValue);
}

bool TqAdminAuth::LoadTokenFile(const std::string& path, std::string& err) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        err = "failed to open admin token file";
        return false;
    }
    std::string body((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (!in.good() && !in.eof()) {
        err = "failed to read admin token file";
        return false;
    }
    std::string token;
    TqTokenJsonParser parser(body, err);
    if (!parser.Parse(token)) {
        return false;
    }
    TokenValue = token;
    return true;
}

bool TqAdminAuth::WriteTokenFile(const std::string& path, const std::string& listen, std::string& err) {
    if (TokenValue.empty() && !InitializeToken()) {
        err = "failed to generate admin token";
        return false;
    }
    return TqWriteFileAtomic(std::filesystem::path(path), TqTokenJson(TokenValue, listen), err);
}

bool TqAdminAuth::CleanupTokenFile(const std::string& path) const {
    if (path.empty() || !std::filesystem::exists(path)) {
        return true;
    }
    if (!TqFileContainsTokenOrPid(path, TokenValue)) {
        return false;
    }
    std::error_code ec;
    std::filesystem::remove(path, ec);
    return !ec;
}

void TqAdminAuth::SetRuntimeBinaryName(const char* argv0) {
    const std::string name = TqBaseNameFromArgv0(argv0);
    if (name.empty()) {
        return;
    }

    std::lock_guard<std::mutex> lock(TqRuntimeBinaryNameMutex());
    TqRuntimeBinaryNameStorage() = name;
}

std::string TqAdminAuth::DefaultTokenFilePath() {
    return DefaultTokenFilePath(std::string{});
}

std::string TqAdminAuth::DefaultTokenFilePath(const std::string& role) {
    const std::string runtimeName = TqRuntimeBinaryName();
    std::filesystem::path base;
#if defined(_WIN32)
    char localAppData[MAX_PATH]{};
    const DWORD envLen = GetEnvironmentVariableA("LOCALAPPDATA", localAppData, MAX_PATH);
    base = envLen > 0 && envLen < MAX_PATH
        ? std::filesystem::path(localAppData) / runtimeName
        : std::filesystem::temp_directory_path() / runtimeName;
#else
    const char* runtimeDir = std::getenv("XDG_RUNTIME_DIR");
    if (runtimeDir != nullptr && runtimeDir[0] != '\0') {
        base = std::filesystem::path(runtimeDir) / runtimeName;
    } else {
        base = std::filesystem::temp_directory_path() /
            (runtimeName + "-" + std::to_string(static_cast<unsigned long>(::getuid())));
    }
#endif
    return (base / TqDefaultTokenFileName(role)).string();
}
