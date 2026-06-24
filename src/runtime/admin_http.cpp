#include "admin_http.h"

#include "admin_auth.h"
#include "platform_socket.h"

#include "httplib.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <limits>
#include <sstream>
#include <string>

namespace {

constexpr size_t TqMaxAdminHttpBytes = 64 * 1024;

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
    case 201:
        return "Created";
    case 202:
        return "Accepted";
    case 204:
        return "No Content";
    case 400:
        return "Bad Request";
    case 401:
        return "Unauthorized";
    case 404:
        return "Not Found";
    case 409:
        return "Conflict";
    case 413:
        return "Payload Too Large";
    case 503:
        return "Service Unavailable";
    case 500:
    default:
        return "Internal Server Error";
    }
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

std::string TqLowerHeaderName(std::string text) {
    return TqAsciiLower(std::move(text));
}

bool TqIsLegacyAdminPath(const std::string& path) {
    auto isLegacyPeerAction = [&path](const char* suffix) {
        const size_t suffixLen = std::strlen(suffix);
        return path.compare(0, 7, "/peers/") == 0 &&
            path.size() > 7 + suffixLen &&
            path.compare(path.size() - suffixLen, suffixLen, suffix) == 0;
    };
    return path == "/health" ||
        path == "/metrics" ||
        path == "/config" ||
        isLegacyPeerAction("/enable") ||
        isLegacyPeerAction("/disable");
}

bool TqIsV1PeerPath(const std::string& path) {
    constexpr const char* kPeers = "/api/v1/peers";
    constexpr size_t kPeersLen = std::char_traits<char>::length(kPeers);
    if (path == kPeers) {
        return true;
    }
    if (path.compare(0, kPeersLen + 1, "/api/v1/peers/") != 0) {
        return false;
    }
    const std::string tail = path.substr(kPeersLen + 1);
    if (tail.empty()) {
        return false;
    }

    const size_t firstSlash = tail.find('/');
    if (firstSlash != std::string::npos) {
        const std::string peer = tail.substr(0, firstSlash);
        const std::string rest = tail.substr(firstSlash + 1);
        const bool connectionsCollection = rest == "connections";
        const bool connectionsItem = rest.compare(0, 12, "connections/") == 0;
        if (peer.empty() || (!connectionsCollection && !connectionsItem)) {
            return false;
        }
        if (connectionsCollection) {
            return true;
        }
        std::string connection = rest.substr(12);
        if (connection.empty() || connection.find('/') != std::string::npos) {
            return false;
        }
        const size_t connectionAction = connection.find(':');
        if (connectionAction == std::string::npos) {
            return true;
        }
        const std::string name = connection.substr(connectionAction + 1);
        return !connection.substr(0, connectionAction).empty() &&
            (name == "reconnect" || name == "abort-tunnels");
    }

    const size_t action = tail.find(':');
    if (action == std::string::npos) {
        return true;
    }
    const std::string name = tail.substr(action + 1);
    return !tail.substr(0, action).empty() &&
        (name == "enable" || name == "disable" || name == "drain" || name == "abort-tunnels");
}

bool TqIsV1AdminPath(const std::string& path) {
    return path == "/api/v1/health" ||
        path == "/api/v1/metrics" ||
        path == "/api/v1/config" ||
        path == "/api/v1/tunnels" ||
        path.compare(0, 16, "/api/v1/tunnels/") == 0 ||
        path == "/api/v1/relay/metrics" ||
        path == "/api/v1/relay/workers" ||
        path.compare(0, 22, "/api/v1/relay/workers/") == 0 ||
        path == "/api/v1/server" ||
        path == "/api/v1/server/metrics" ||
        path == "/api/v1/server/connections" ||
        path.compare(0, 27, "/api/v1/server/connections/") == 0 ||
        path == "/api/v1/server/tunnels" ||
        TqIsV1PeerPath(path);
}

bool TqIsV1Prefix(const std::string& path) {
    return path == "/api/v1" || path.compare(0, 8, "/api/v1/") == 0;
}

std::string TqV1ToLegacyPath(const std::string& path) {
    if (path.compare(0, 7, "/api/v1") == 0) {
        const std::string stripped = path.substr(7);
        return stripped.empty() ? "/" : stripped;
    }
    return path;
}

TqHttpRequest TqMakeAdminRequest(const httplib::Request& req) {
    TqHttpRequest out;
    out.Method = req.method;
    out.Path = TqV1ToLegacyPath(req.path);
    out.Body = req.body;
    for (const auto& header : req.headers) {
        out.Headers[TqLowerHeaderName(header.first)] = header.second;
    }
    return out;
}

void TqSetJson(httplib::Response& res, int status, const std::string& body) {
    res.status = status;
    res.set_header("Connection", "close");
    res.set_content(body, "application/json");
}

void TqSetLegacyResponse(httplib::Response& res, const std::string& raw) {
    const size_t lineEnd = raw.find("\r\n");
    if (lineEnd == std::string::npos || raw.compare(0, 9, "HTTP/1.1 ") != 0) {
        TqSetJson(res, 500, "{\"error\":\"bad handler response\"}");
        return;
    }
    int status = 500;
    try {
        status = std::stoi(raw.substr(9, 3));
    } catch (...) {
        status = 500;
    }
    const size_t bodyStart = raw.find("\r\n\r\n");
    const std::string body = bodyStart == std::string::npos ? std::string{} : raw.substr(bodyStart + 4);
    TqSetJson(res, status, body);
}

std::string TqUnauthorizedJson() {
    return "{\"error\":{\"code\":\"unauthorized\",\"message\":\"unauthorized\"}}";
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
    if (status != 200 &&
        status != 201 &&
        status != 202 &&
        status != 204 &&
        status != 400 &&
        status != 401 &&
        status != 404 &&
        status != 409 &&
        status != 413 &&
        status != 500 &&
        status != 503) {
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
    TqAdminHttpServer(std::move(listen), std::move(handler), TqAdminHttpServerOptions{}) {
}

TqAdminHttpServer::TqAdminHttpServer(std::string listen, TqHttpHandler handler, TqAdminHttpServerOptions options) :
    Listen(std::move(listen)),
    Handler(std::move(handler)),
    Options(std::move(options)) {
}

TqAdminHttpServer::~TqAdminHttpServer() {
    Stop();
}

bool TqAdminHttpServer::Start(std::string& err) {
    if (!TqValidateAdminBindListen(Listen, err)) {
        return false;
    }

    TqHostPort hostPort{};
    if (!TqParseHostPort(Listen, hostPort, true)) {
        err = "admin listen must be host:port";
        return false;
    }
    if (Options.AdminThreads == 0 || Options.AdminThreads > 32) {
        err = "admin threads must be in range 1..32";
        return false;
    }

    Server.reset(new httplib::Server());
    Server->new_task_queue = [threads = Options.AdminThreads]() {
        return new httplib::ThreadPool(threads);
    };
    Server->set_read_timeout(5, 0);
    Server->set_write_timeout(5, 0);
    Server->set_keep_alive_max_count(1);
    Server->set_keep_alive_timeout(1);
    Server->set_payload_max_length(Options.MaxBodyBytes);
    Server->set_error_handler([](const httplib::Request&, httplib::Response& res) {
        if (res.status == 413) {
            TqSetJson(res, 413, "{\"error\":{\"code\":\"payload_too_large\",\"message\":\"payload too large\"}}");
        } else if (res.status == 400) {
            TqSetJson(res, 400, "{\"error\":{\"code\":\"bad_request\",\"message\":\"bad request\"}}");
        } else {
            TqSetJson(res, res.status == 0 ? 404 : res.status, "{\"error\":{\"code\":\"not_found\",\"message\":\"not found\"}}");
        }
    });
    ConfigureRoutes();

    const bool tokenAuthEnabled = Options.EnableTokenAuth || !Options.TokenFile.empty();
    if (tokenAuthEnabled) {
        Auth.reset(new TqAdminAuth());
        if (!Auth->InitializeToken()) {
            err = "failed to generate admin token";
            Server.reset();
            return false;
        }
        TokenFilePath = Options.TokenFile.empty() ? TqAdminAuth::DefaultTokenFilePath() : Options.TokenFile;
    }

    int boundPort = 0;
    if (hostPort.Port == 0) {
        boundPort = Server->bind_to_any_port(hostPort.Host);
        if (boundPort <= 0) {
            err = "bind failed";
            Server.reset();
            Auth.reset();
            return false;
        }
    } else {
        if (!Server->bind_to_port(hostPort.Host, hostPort.Port)) {
            err = "bind failed";
            Server.reset();
            Auth.reset();
            return false;
        }
        boundPort = hostPort.Port;
    }
    BoundListen = hostPort.Host + ":" + std::to_string(boundPort);

    Thread = std::thread(&TqAdminHttpServer::Run, this);
    Server->wait_until_ready();
    if (!Server->is_running()) {
        err = "listen failed";
        Stop();
        return false;
    }

    if (Auth && !Auth->WriteTokenFile(TokenFilePath, BoundListen, err)) {
        Stop();
        return false;
    }
    Started = true;
    return true;
}

void TqAdminHttpServer::Stop() {
    if (Server) {
        Server->stop();
    }
    if (Thread.joinable()) {
        Thread.join();
    }
    if (Auth && !TokenFilePath.empty()) {
        (void)Auth->CleanupTokenFile(TokenFilePath);
    }
    Started = false;
}

std::string TqAdminHttpServer::ListenAddress() const {
    std::lock_guard<std::mutex> guard(Lock);
    return BoundListen;
}

std::string TqAdminHttpServer::AuthTokenForTesting() const {
    return Auth ? Auth->Token() : std::string{};
}

void TqAdminHttpServer::ConfigureRoutes() {
    auto dispatch = [this](const httplib::Request& req, httplib::Response& res) {
        const bool isLegacy = TqIsLegacyAdminPath(req.path);
        const bool isV1 = TqIsV1AdminPath(req.path);
        TqHttpRequest adminReq = TqMakeAdminRequest(req);
        if (Auth && (TqIsV1Prefix(req.path) || (isLegacy && !Options.AllowUnauthenticatedLegacy)) && !Auth->Authorize(adminReq)) {
            TqSetJson(res, 401, TqUnauthorizedJson());
            return;
        }
        if (!isLegacy && !isV1) {
            TqSetJson(res, 404, "{\"error\":{\"code\":\"not_found\",\"message\":\"not found\"}}");
            return;
        }
        TqSetLegacyResponse(res, Handler ? Handler(adminReq) : TqJsonResponse(500, "{\"error\":\"no handler\"}"));
    };

    Server->Get(R"(.*)", dispatch);
    Server->Put(R"(.*)", dispatch);
    Server->Post(R"(.*)", dispatch);
    Server->Patch(R"(.*)", dispatch);
    Server->Delete(R"(.*)", dispatch);
}

void TqAdminHttpServer::Run() {
    if (Server) {
        (void)Server->listen_after_bind();
    }
}
