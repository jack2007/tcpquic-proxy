#include "admin_http.h"

#include <algorithm>
#include <arpa/inet.h>
#include <cerrno>
#include <cctype>
#include <cstring>
#include <limits>
#include <netinet/in.h>
#include <string>

#include <netdb.h>
#include <sys/socket.h>
#include <unistd.h>

namespace {

constexpr size_t TqMaxAdminHttpBytes = 64 * 1024;

enum class TqRequestReadState {
    Incomplete,
    Complete,
    Malformed
};

struct TqHostPort {
    std::string Host;
    uint16_t Port{};
};

std::string TqAsciiLower(std::string text) {
    for (char& ch : text) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    return text;
}

std::string TqTrim(std::string text) {
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t')) {
        text.pop_back();
    }
    size_t first = 0;
    while (first < text.size() && (text[first] == ' ' || text[first] == '\t')) {
        ++first;
    }
    return text.substr(first);
}

bool TqParseSize(const std::string& text, size_t& value) {
    if (text.empty()) {
        return false;
    }

    size_t parsed = 0;
    for (char ch : text) {
        if (ch < '0' || ch > '9') {
            return false;
        }
        const size_t digit = static_cast<size_t>(ch - '0');
        if (parsed > (std::numeric_limits<size_t>::max() - digit) / 10) {
            return false;
        }
        parsed = (parsed * 10) + digit;
    }

    value = parsed;
    return true;
}

bool TqParsePort(const std::string& text, uint16_t& port, bool allowZero = false) {
    size_t parsed = 0;
    if (!TqParseSize(text, parsed) || (!allowZero && parsed == 0) || parsed > std::numeric_limits<uint16_t>::max()) {
        return false;
    }
    port = static_cast<uint16_t>(parsed);
    return true;
}

bool TqParseHostPort(const std::string& listen, TqHostPort& out, bool allowZero = false) {
    const size_t pos = listen.rfind(':');
    if (pos == std::string::npos || pos == 0 || pos + 1 == listen.size()) {
        return false;
    }

    uint16_t port = 0;
    if (!TqParsePort(listen.substr(pos + 1), port, allowZero)) {
        return false;
    }

    out.Host = listen.substr(0, pos);
    out.Port = port;
    return true;
}

const char* TqReasonPhrase(int status) {
    switch (status) {
    case 200:
        return "OK";
    case 400:
        return "Bad Request";
    case 404:
        return "Not Found";
    case 500:
    default:
        return "Internal Server Error";
    }
}

bool TqSendAll(int fd, const std::string& data) {
    size_t sent = 0;
    while (sent < data.size()) {
        const ssize_t result = ::send(fd, data.data() + sent, data.size() - sent, MSG_NOSIGNAL);
        if (result < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        if (result == 0) {
            return false;
        }
        sent += static_cast<size_t>(result);
    }
    return true;
}

void TqCloseFd(int fd) {
    if (fd >= 0) {
        ::close(fd);
    }
}

bool TqCreateListenSocket(const std::string& listen, int& listenFd, std::string& err) {
    TqHostPort hostPort{};
    if (!TqParseHostPort(listen, hostPort, true)) {
        err = "admin listen must be host:port";
        return false;
    }

    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;

    addrinfo* result = nullptr;
    const std::string port = std::to_string(hostPort.Port);
    const int status = getaddrinfo(hostPort.Host.c_str(), port.c_str(), &hints, &result);
    if (status != 0) {
        err = gai_strerror(status);
        return false;
    }

    for (addrinfo* ai = result; ai != nullptr; ai = ai->ai_next) {
        int fd = ::socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0) {
            continue;
        }

        int enabled = 1;
        (void)::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &enabled, sizeof(enabled));

        if (::bind(fd, ai->ai_addr, ai->ai_addrlen) == 0 && ::listen(fd, SOMAXCONN) == 0) {
            listenFd = fd;
            freeaddrinfo(result);
            return true;
        }

        ::close(fd);
    }

    freeaddrinfo(result);
    err = std::strerror(errno);
    return false;
}

std::string TqGetBoundListenAddress(int fd, const std::string& fallbackHost) {
    sockaddr_storage storage{};
    socklen_t storageLen = sizeof(storage);
    if (::getsockname(fd, reinterpret_cast<sockaddr*>(&storage), &storageLen) != 0) {
        return {};
    }

    if (storage.ss_family == AF_INET) {
        const auto* addr = reinterpret_cast<const sockaddr_in*>(&storage);
        char host[INET_ADDRSTRLEN]{};
        if (::inet_ntop(AF_INET, &addr->sin_addr, host, sizeof(host)) == nullptr) {
            return {};
        }
        return std::string(host) + ":" + std::to_string(ntohs(addr->sin_port));
    }

    if (storage.ss_family == AF_INET6) {
        const auto* addr = reinterpret_cast<const sockaddr_in6*>(&storage);
        char host[INET6_ADDRSTRLEN]{};
        if (::inet_ntop(AF_INET6, &addr->sin6_addr, host, sizeof(host)) == nullptr) {
            return {};
        }
        return std::string(host) + ":" + std::to_string(ntohs(addr->sin6_port));
    }

    return fallbackHost;
}

bool TqValidateAdminBindListen(const std::string& listen, std::string& err) {
    TqHostPort hostPort{};
    if (!TqParseHostPort(listen, hostPort, true)) {
        err = "admin listen must be host:port";
        return false;
    }
    if (hostPort.Host == "127.0.0.1" || hostPort.Host == "localhost" || hostPort.Host == "::1") {
        return true;
    }
    err = "admin listen must bind loopback in this stage";
    return false;
}

TqRequestReadState TqRequestReadStateFor(const std::string& raw, size_t& total) {
    const size_t headerEnd = raw.find("\r\n\r\n");
    if (headerEnd == std::string::npos) {
        return TqRequestReadState::Incomplete;
    }

    total = headerEnd + 4;
    bool hasContentLength = false;
    size_t contentLength = 0;
    size_t lineStart = raw.find("\r\n") + 2;
    while (lineStart < headerEnd) {
        const size_t lineEnd = raw.find("\r\n", lineStart);
        if (lineEnd == std::string::npos || lineEnd > headerEnd) {
            return TqRequestReadState::Malformed;
        }
        const std::string line = raw.substr(lineStart, lineEnd - lineStart);
        const size_t colon = line.find(':');
        if (colon == std::string::npos) {
            return TqRequestReadState::Malformed;
        }
        const std::string name = TqAsciiLower(line.substr(0, colon));
        const std::string value = TqTrim(line.substr(colon + 1));
        if (name == "content-length") {
            if (hasContentLength || !TqParseSize(value, contentLength)) {
                return TqRequestReadState::Malformed;
            }
            hasContentLength = true;
        } else if (name == "transfer-encoding") {
            return TqRequestReadState::Malformed;
        }
        lineStart = lineEnd + 2;
    }

    if (hasContentLength) {
        if (total > TqMaxAdminHttpBytes || contentLength > TqMaxAdminHttpBytes - total) {
            return TqRequestReadState::Malformed;
        }
        total += contentLength;
        return raw.size() >= total ? TqRequestReadState::Complete : TqRequestReadState::Incomplete;
    }

    return TqRequestReadState::Complete;
}

void TqHandleAdminClient(int clientFd, const TqHttpHandler& handler) {
    std::string raw;
    raw.reserve(1024);

    char buffer[1024];
    for (;;) {
        size_t total = 0;
        const TqRequestReadState state = TqRequestReadStateFor(raw, total);
        if (state == TqRequestReadState::Complete) {
            raw.resize(total);
            break;
        }
        if (state == TqRequestReadState::Malformed) {
            (void)TqSendAll(clientFd, TqJsonResponse(400, "{\"error\":\"bad request\"}"));
            return;
        }
        if (raw.size() >= TqMaxAdminHttpBytes) {
            (void)TqSendAll(clientFd, TqJsonResponse(400, "{\"error\":\"bad request\"}"));
            return;
        }

        const ssize_t received = ::recv(clientFd, buffer, sizeof(buffer), 0);
        if (received < 0) {
            if (errno == EINTR) {
                continue;
            }
            return;
        }
        if (received == 0) {
            return;
        }
        raw.append(buffer, static_cast<size_t>(received));
    }

    TqHttpRequest req{};
    std::string err;
    if (!TqParseHttpRequest(raw, req, err)) {
        (void)TqSendAll(clientFd, TqJsonResponse(400, "{\"error\":\"bad request\"}"));
        return;
    }

    const std::string response = handler ? handler(req) : TqJsonResponse(500, "{\"error\":\"no handler\"}");
    (void)TqSendAll(clientFd, response);
}

} // namespace

bool TqParseHttpRequest(const std::string& raw, TqHttpRequest& req, std::string& err) {
    const size_t headerEnd = raw.find("\r\n\r\n");
    if (headerEnd == std::string::npos) {
        err = "missing header terminator";
        return false;
    }

    const size_t lineEnd = raw.find("\r\n");
    if (lineEnd == std::string::npos || lineEnd == 0 || lineEnd > headerEnd) {
        err = "missing request line";
        return false;
    }

    const std::string line = raw.substr(0, lineEnd);
    const size_t methodEnd = line.find(' ');
    const size_t pathEnd = methodEnd == std::string::npos ? std::string::npos : line.find(' ', methodEnd + 1);
    if (methodEnd == std::string::npos || pathEnd == std::string::npos || line.find(' ', pathEnd + 1) != std::string::npos) {
        err = "invalid request line";
        return false;
    }

    const std::string version = line.substr(pathEnd + 1);
    if (version != "HTTP/1.1") {
        err = "unsupported http version";
        return false;
    }

    size_t contentLength = 0;
    bool hasContentLength = false;
    size_t lineStart = lineEnd + 2;
    while (lineStart < headerEnd) {
        const size_t nextLine = raw.find("\r\n", lineStart);
        if (nextLine == std::string::npos || nextLine > headerEnd) {
            err = "invalid header";
            return false;
        }

        const std::string header = raw.substr(lineStart, nextLine - lineStart);
        const size_t colon = header.find(':');
        if (colon == std::string::npos) {
            err = "invalid header";
            return false;
        }

        const std::string name = TqAsciiLower(header.substr(0, colon));
        const std::string value = TqTrim(header.substr(colon + 1));
        if (name == "content-length") {
            if (hasContentLength || !TqParseSize(value, contentLength)) {
                err = "invalid content-length";
                return false;
            }
            if (contentLength > TqMaxAdminHttpBytes) {
                err = "content-length too large";
                return false;
            }
            hasContentLength = true;
        } else if (name == "transfer-encoding") {
            err = "unsupported http framing";
            return false;
        }

        lineStart = nextLine + 2;
    }

    const size_t bodyStart = headerEnd + 4;
    if (hasContentLength) {
        if (bodyStart > TqMaxAdminHttpBytes || contentLength > TqMaxAdminHttpBytes - bodyStart) {
            err = "content-length too large";
            return false;
        }
        if (raw.size() < bodyStart + contentLength) {
            err = "incomplete body";
            return false;
        }
        req.Body = raw.substr(bodyStart, contentLength);
    } else {
        req.Body.clear();
    }
    req.Method = line.substr(0, methodEnd);
    req.Path = line.substr(methodEnd + 1, pathEnd - methodEnd - 1);
    return true;
}

std::string TqJsonResponse(int status, const std::string& json) {
    if (status != 200 && status != 400 && status != 404 && status != 500) {
        status = 500;
    }

    return "HTTP/1.1 " + std::to_string(status) + " " + TqReasonPhrase(status) + "\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: " + std::to_string(json.size()) + "\r\n"
        "Connection: close\r\n"
        "\r\n" + json;
}

bool TqValidateAdminListen(const std::string& listen, std::string& err) {
    auto pos = listen.rfind(':');
    if (pos == std::string::npos || pos == 0 || pos + 1 == listen.size()) {
        err = "admin listen must be host:port";
        return false;
    }
    uint16_t port = 0;
    if (!TqParsePort(listen.substr(pos + 1), port)) {
        err = "admin listen port must be in range 1..65535";
        return false;
    }
    std::string host = listen.substr(0, pos);
    if (host == "127.0.0.1" || host == "localhost" || host == "::1") {
        return true;
    }
    err = "admin listen must bind loopback in this stage";
    return false;
}

TqAdminHttpServer::TqAdminHttpServer(std::string listen, TqHttpHandler handler) :
    Listen(std::move(listen)),
    Handler(std::move(handler)) {
}

TqAdminHttpServer::~TqAdminHttpServer() {
    Stop();
}

bool TqAdminHttpServer::Start(std::string& err) {
    if (!TqValidateAdminBindListen(Listen, err)) {
        return false;
    }
    if (!TqCreateListenSocket(Listen, ListenFd, err)) {
        return false;
    }

    BoundListen = TqGetBoundListenAddress(ListenFd, Listen);

    Stopping = false;
    Thread = std::thread(&TqAdminHttpServer::Run, this, ListenFd);
    return true;
}

void TqAdminHttpServer::Stop() {
    Stopping = true;
    if (ListenFd >= 0) {
        ::shutdown(ListenFd, SHUT_RDWR);
        ::close(ListenFd);
        ListenFd = -1;
    }
    {
        std::lock_guard<std::mutex> guard(Lock);
        for (const ActiveClient& client : ActiveClients) {
            ::shutdown(client.Fd, SHUT_RDWR);
        }
    }
    if (Thread.joinable()) {
        Thread.join();
    }

    {
        std::unique_lock<std::mutex> guard(Lock);
        for (const ActiveClient& client : ActiveClients) {
            ::shutdown(client.Fd, SHUT_RDWR);
        }
        ActiveClientsDrained.wait(guard, [this]() { return ActiveClients.empty(); });
        ClientThreadsDrained.wait(guard, [this]() { return ActiveClientThreads == 0; });
    }
}

std::string TqAdminHttpServer::ListenAddress() const {
    std::lock_guard<std::mutex> guard(Lock);
    return BoundListen;
}

void TqAdminHttpServer::Run(int listenFd) {
    while (!Stopping) {
        int clientFd = ::accept(listenFd, nullptr, nullptr);
        if (clientFd < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }
        if (Stopping) {
            TqCloseFd(clientFd);
            break;
        }
        const uint64_t clientId = NextClientId.fetch_add(1);
        TqHttpHandler handler;
        {
            std::lock_guard<std::mutex> guard(Lock);
            ActiveClients.push_back({clientId, clientFd});
            ++ActiveClientThreads;
            handler = Handler;
        }
        std::thread([this, clientId, clientFd, handler]() { HandleClient(clientId, clientFd, handler); }).detach();
    }
}

void TqAdminHttpServer::HandleClient(uint64_t clientId, int clientFd, TqHttpHandler handler) {
    TqHandleAdminClient(clientFd, handler);
    {
        std::lock_guard<std::mutex> guard(Lock);
        RemoveActiveClientLocked(clientId);
    }
    TqCloseFd(clientFd);

    std::lock_guard<std::mutex> guard(Lock);
    --ActiveClientThreads;
    if (ActiveClientThreads == 0) {
        ClientThreadsDrained.notify_all();
    }
}

void TqAdminHttpServer::RemoveActiveClientLocked(uint64_t clientId) {
    ActiveClients.erase(std::remove_if(ActiveClients.begin(), ActiveClients.end(), [clientId](const ActiveClient& client) {
        return client.Id == clientId;
    }), ActiveClients.end());
    if (ActiveClients.empty()) {
        ActiveClientsDrained.notify_all();
    }
}
