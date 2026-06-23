#include "admin_auth.h"

#include <array>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <random>
#include <sstream>
#include <vector>

#if defined(_WIN32)
#include <process.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace {

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

uint64_t TqUnixNow() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count());
}

std::string TqJsonEscape(const std::string& value) {
    std::ostringstream out;
    for (unsigned char ch : value) {
        switch (ch) {
        case '\\':
            out << "\\\\";
            break;
        case '"':
            out << "\\\"";
            break;
        case '\n':
            out << "\\n";
            break;
        case '\r':
            out << "\\r";
            break;
        case '\t':
            out << "\\t";
            break;
        default:
            if (ch < 0x20) {
                out << "\\u";
                static constexpr char kHex[] = "0123456789abcdef";
                out << "00" << kHex[ch >> 4] << kHex[ch & 0x0f];
            } else {
                out << static_cast<char>(ch);
            }
            break;
        }
    }
    return out.str();
}

std::string TqTokenJson(const std::string& token, const std::string& listen) {
    std::ostringstream out;
    out << "{\n"
        << "  \"version\":1,\n"
        << "  \"token_type\":\"Bearer\",\n"
        << "  \"token\":\"" << TqJsonEscape(token) << "\",\n"
        << "  \"listen\":\"" << TqJsonEscape(listen) << "\",\n"
        << "  \"pid\":" << TqCurrentPid() << ",\n"
        << "  \"created_at_unix\":" << TqUnixNow() << "\n"
        << "}\n";
    return out.str();
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

bool TqFileContainsTokenOrPid(const std::string& path, const std::string& token) {
    std::ifstream in(path);
    if (!in) {
        return false;
    }
    std::string body((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (!token.empty() && body.find("\"token\":\"" + token + "\"") != std::string::npos) {
        return true;
    }
    return body.find("\"pid\":" + std::to_string(TqCurrentPid())) != std::string::npos;
}

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

std::string TqAdminAuth::DefaultTokenFilePath() {
    std::filesystem::path base;
#if defined(_WIN32)
    const char* localAppData = std::getenv("LOCALAPPDATA");
    base = localAppData != nullptr && localAppData[0] != '\0'
        ? std::filesystem::path(localAppData) / "tcpquic-proxy"
        : std::filesystem::temp_directory_path() / "tcpquic-proxy";
#else
    const char* runtimeDir = std::getenv("XDG_RUNTIME_DIR");
    if (runtimeDir != nullptr && runtimeDir[0] != '\0') {
        base = std::filesystem::path(runtimeDir) / "tcpquic-proxy";
    } else {
        base = std::filesystem::temp_directory_path() /
            ("tcpquic-proxy-" + std::to_string(static_cast<unsigned long>(::getuid())));
    }
#endif
    return (base / ("admin-" + std::to_string(TqCurrentPid()) + ".json")).string();
}
