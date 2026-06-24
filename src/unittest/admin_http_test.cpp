#include "admin_http.h"
#include "admin_auth.h"
#include "platform_socket.h"

#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>

namespace {

TqSocketHandle TqConnectLocal(uint16_t port) {
    TqSocketHandle fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (!TqSocketValid(fd)) {
        return TqInvalidSocket;
    }

#if !defined(_WIN32)
    timeval timeout{};
    timeout.tv_sec = 2;
    (void)::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
#else
    DWORD timeoutMs = 2000;
    (void)::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeoutMs), sizeof(timeoutMs));
#endif

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (!TqInetPton(AF_INET, "127.0.0.1", &addr.sin_addr) ||
        ::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        TqCloseSocket(fd);
        return TqInvalidSocket;
    }

    return fd;
}

bool TqSendAll(TqSocketHandle fd, const std::string& data) {
    size_t sent = 0;
    while (sent < data.size()) {
        const int result = TqSend(fd, data.data() + sent, data.size() - sent, TqSendFlags::None);
        if (result <= 0) {
            return false;
        }
        sent += static_cast<size_t>(result);
    }
    return true;
}

bool TqRecvUntilClosed(TqSocketHandle fd, std::string& data) {
    char buffer[256];
    for (;;) {
        const int result = TqRecv(fd, buffer, sizeof(buffer), TqRecvFlags::None);
        if (result < 0) {
            return data.find("\r\n\r\n") != std::string::npos;
        }
        if (result == 0) {
            return true;
        }
        data.append(buffer, static_cast<size_t>(result));
        const size_t headerEnd = data.find("\r\n\r\n");
        if (headerEnd != std::string::npos) {
            size_t contentLength = 0;
            const std::string marker = "Content-Length: ";
            const size_t contentLengthPos = data.find(marker);
            if (contentLengthPos != std::string::npos && contentLengthPos < headerEnd) {
                const size_t valueStart = contentLengthPos + marker.size();
                const size_t valueEnd = data.find("\r\n", valueStart);
                contentLength = static_cast<size_t>(std::stoul(data.substr(valueStart, valueEnd - valueStart)));
                if (data.size() >= headerEnd + 4 + contentLength) {
                    return true;
                }
            }
        }
    }
}

uint16_t TqPortFromListenAddress(const std::string& listen) {
    size_t colon = listen.rfind(':');
    if (colon == std::string::npos || colon + 1 == listen.size()) {
        return 0;
    }
    return static_cast<uint16_t>(std::stoul(listen.substr(colon + 1)));
}

bool TqHttpStatusIs(const std::string& response, int status) {
    const std::string prefix = "HTTP/1.1 " + std::to_string(status) + " ";
    return response.find(prefix) == 0;
}

std::filesystem::path TqSecureTokenFile(const std::string& name) {
    static unsigned counter = 0;
    std::filesystem::path dir = std::filesystem::temp_directory_path() /
        ("tcpquic-admin-secure-" + name + "-" + std::to_string(counter++));
    std::filesystem::create_directories(dir);
    std::filesystem::permissions(dir,
        std::filesystem::perms::owner_all,
        std::filesystem::perm_options::replace);
    return dir / "admin-token.json";
}

} // namespace

int main() {
    TqSocketStartup startup;
    if (!startup.Ok()) {
        return 1;
    }

    {
        TqHttpRequest req;
        std::string err;
        if (!TqParseHttpRequest("GET /health HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n", req, err)) return 1;
        if (req.Method != "GET") return 2;
        if (req.Path != "/health") return 3;
        if (!req.Body.empty()) return 4;
    }
    {
        TqHttpRequest req;
        std::string err;
        std::string raw = "PUT /config HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Length: 13\r\n\r\n{\"version\":1}";
        if (!TqParseHttpRequest(raw, req, err)) return 5;
        if (req.Method != "PUT") return 6;
        if (req.Path != "/config") return 7;
        if (req.Body != "{\"version\":1}") return 8;
    }
    {
        std::string resp = TqJsonResponse(200, "{\"status\":\"healthy\"}");
        if (resp.find("HTTP/1.1 200 OK\r\n") != 0) return 9;
        if (resp.find("Content-Type: application/json\r\n") == std::string::npos) return 10;
        if (resp.find("Content-Length: 20\r\n") == std::string::npos) return 11;
    }
    {
        if (TqJsonResponse(400, "{}").find("HTTP/1.1 400 Bad Request\r\n") != 0) return 16;
        if (TqJsonResponse(401, "{}").find("HTTP/1.1 401 Unauthorized\r\n") != 0) return 78;
        if (TqJsonResponse(201, "{}").find("HTTP/1.1 201 Created\r\n") != 0) return 114;
        if (TqJsonResponse(202, "{}").find("HTTP/1.1 202 Accepted\r\n") != 0) return 115;
        if (TqJsonResponse(204, "{}").find("HTTP/1.1 204 No Content\r\n") != 0) return 116;
        if (TqJsonResponse(409, "{}").find("HTTP/1.1 409 Conflict\r\n") != 0) return 117;
        if (TqJsonResponse(413, "{}").find("HTTP/1.1 413 Payload Too Large\r\n") != 0) return 79;
        if (TqJsonResponse(404, "{}").find("HTTP/1.1 404 Not Found\r\n") != 0) return 17;
        if (TqJsonResponse(503, "{}").find("HTTP/1.1 503 Service Unavailable\r\n") != 0) return 80;
        if (TqJsonResponse(500, "{}").find("HTTP/1.1 500 Internal Server Error\r\n") != 0) return 18;
    }
    {
        TqAdminAuth auth;
        if (!auth.InitializeToken()) return 81;
        if (auth.Token().size() < 64) return 82;
        TqHttpRequest req;
        req.Headers["authorization"] = "Bearer " + auth.Token();
        if (!auth.Authorize(req)) return 83;
        req.Headers["authorization"] = "Bearer wrong";
        if (auth.Authorize(req)) return 84;
        req.Headers["authorization"] = "Basic " + auth.Token();
        if (auth.Authorize(req)) return 85;
    }
    {
        TqAdminAuth auth;
        if (!auth.InitializeToken()) return 86;
        const std::filesystem::path dir = std::filesystem::temp_directory_path() / "tcpquic-admin-auth-test";
        std::filesystem::create_directories(dir);
        const std::filesystem::path tokenFile = dir / "admin-token.json";
        std::string err;
        if (!auth.WriteTokenFile(tokenFile.string(), "127.0.0.1:19091", err)) return 87;
        std::ifstream in(tokenFile);
        std::string body((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        if (body.find("\"token\":\"" + auth.Token() + "\"") == std::string::npos) return 88;
        if (body.find("\"listen\":\"127.0.0.1:19091\"") == std::string::npos) return 89;
#if !defined(_WIN32)
        const auto perms = std::filesystem::status(tokenFile).permissions();
        if ((perms & std::filesystem::perms::group_read) != std::filesystem::perms::none) return 90;
        if ((perms & std::filesystem::perms::others_read) != std::filesystem::perms::none) return 91;
#endif
        if (!auth.CleanupTokenFile(tokenFile.string())) return 92;
        if (std::filesystem::exists(tokenFile)) return 93;
    }
#if !defined(_WIN32)
    {
        TqAdminAuth auth;
        if (!auth.InitializeToken()) return 118;
        const std::filesystem::path rootTokenFile = std::filesystem::path("/") / "tcpquic-admin-auth-root-token.json";
        std::string err;
        if (auth.WriteTokenFile(rootTokenFile.string(), "127.0.0.1:19091", err)) return 119;

        const std::filesystem::path dir = std::filesystem::temp_directory_path() / "tcpquic-admin-auth-fix-perms";
        std::filesystem::create_directories(dir);
        std::filesystem::permissions(dir,
            std::filesystem::perms::owner_all | std::filesystem::perms::group_read | std::filesystem::perms::group_exec,
            std::filesystem::perm_options::replace);
        const std::filesystem::path tokenFile = dir / "admin-token.json";
        if (!auth.WriteTokenFile(tokenFile.string(), "127.0.0.1:19091", err)) return 126;
        const auto dirPerms = std::filesystem::status(dir).permissions();
        if ((dirPerms & std::filesystem::perms::group_all) != std::filesystem::perms::none) return 127;
        if ((dirPerms & std::filesystem::perms::others_all) != std::filesystem::perms::none) return 128;
        std::filesystem::permissions(dir,
            std::filesystem::perms::owner_all,
            std::filesystem::perm_options::replace);
        std::filesystem::remove(tokenFile);
        std::filesystem::remove(dir);
    }
#endif
    {
        TqHttpRequest req;
        std::string err;
        std::string raw = "POST /config HTTP/1.1\r\nHost: 127.0.0.1\r\nTransfer-Encoding: chunked\r\n\r\n0\r\n\r\n";
        if (TqParseHttpRequest(raw, req, err)) return 19;
        if (err.find("framing") == std::string::npos) return 20;
    }
    {
        TqHttpRequest req;
        std::string err;
        std::string raw = "PUT /config HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Length: 200000\r\n\r\n{}";
        if (TqParseHttpRequest(raw, req, err)) return 31;
        if (err.find("too large") == std::string::npos) return 32;
    }
    {
        std::string err;
        if (!TqValidateAdminListen("127.0.0.1:19091", err)) return 12;
        if (!TqValidateAdminListen("localhost:19091", err)) return 13;
        if (!TqValidateAdminListen("::1:19091", err)) return 21;
        if (TqValidateAdminListen("0.0.0.0:19091", err)) return 14;
        if (err.find("loopback") == std::string::npos) return 15;
        if (TqValidateAdminListen("127.0.0.1:notaport", err)) return 22;
        if (TqValidateAdminListen("127.0.0.1:0", err)) return 23;
        if (TqValidateAdminListen("127.0.0.1:65536", err)) return 24;
        if (TqValidateAdminListen("127.0.0.1:", err)) return 25;
    }
    {
        const std::string expected = TqJsonResponse(200, "{\"ok\":true}");
        std::string err;
        TqAdminHttpServer server("127.0.0.1:0", [&](const TqHttpRequest& req) {
            if (req.Method != "GET" || req.Path != "/health") {
                return TqJsonResponse(404, "{}");
            }
            return expected;
        });
        if (!server.Start(err)) return 26;
        const uint16_t port = TqPortFromListenAddress(server.ListenAddress());
        if (port == 0) return 33;

        TqSocketHandle fd = TqConnectLocal(port);
        if (!TqSocketValid(fd)) return 27;
        if (!TqSendAll(fd, "GET /health HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n")) return 28;

        std::string response;
        if (!TqRecvUntilClosed(fd, response)) return 29;
        TqCloseSocket(fd);
        server.Stop();
        if (!TqHttpStatusIs(response, 200)) return 30;
        if (response.find("\"ok\":true") == std::string::npos) return 112;
    }
    {
        const std::string expected = TqJsonResponse(200, "{\"second\":true}");
        std::string err;
        TqAdminHttpServer server("127.0.0.1:0", [&](const TqHttpRequest&) {
            return expected;
        });
        if (!server.Start(err)) return 34;
        const uint16_t port = TqPortFromListenAddress(server.ListenAddress());
        if (port == 0) return 35;

        TqSocketHandle stalledFd = TqConnectLocal(port);
        if (!TqSocketValid(stalledFd)) return 36;

        TqSocketHandle fd = TqConnectLocal(port);
        if (!TqSocketValid(fd)) return 37;
        if (!TqSendAll(fd, "GET /health HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n")) return 38;

        std::string response;
        if (!TqRecvUntilClosed(fd, response)) return 39;
        TqCloseSocket(fd);
        TqCloseSocket(stalledFd);
        server.Stop();
        if (!TqHttpStatusIs(response, 200)) return 40;
        if (response.find("\"second\":true") == std::string::npos) return 113;
    }
    {
        std::string err;
        TqAdminHttpServer server("127.0.0.1:0", [&](const TqHttpRequest&) {
            return TqJsonResponse(200, "{}");
        });
        if (!server.Start(err)) return 41;
        const uint16_t port = TqPortFromListenAddress(server.ListenAddress());
        if (port == 0) return 42;
        TqSocketHandle stalledFd = TqConnectLocal(port);
        if (!TqSocketValid(stalledFd)) return 43;
        std::thread stopper([&server]() { server.Stop(); });
        stopper.join();
        TqCloseSocket(stalledFd);
    }
    {
        std::string err;
        TqAdminHttpServerOptions options;
        options.MaxBodyBytes = 1024;
        TqAdminHttpServer server("127.0.0.1:0", [&](const TqHttpRequest&) {
            return TqJsonResponse(200, "{}");
        }, options);
        if (!server.Start(err)) return 44;
        const uint16_t port = TqPortFromListenAddress(server.ListenAddress());
        if (port == 0) return 45;

        TqSocketHandle fd = TqConnectLocal(port);
        if (!TqSocketValid(fd)) return 46;
        const std::string body(2048, 'x');
        if (!TqSendAll(fd, "PUT /config HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Length: " +
                std::to_string(body.size()) + "\r\n\r\n" + body)) return 47;

        std::string response;
        if (!TqRecvUntilClosed(fd, response)) return 48;
        TqCloseSocket(fd);
        server.Stop();
        if (!TqHttpStatusIs(response, 413)) return 49;
    }
    {
        std::string err;
        TqAdminHttpServer server("127.0.0.1:0", [&](const TqHttpRequest&) {
            return TqJsonResponse(200, "{}");
        });
        if (!server.Start(err)) return 66;
        const uint16_t port = TqPortFromListenAddress(server.ListenAddress());
        if (port == 0) return 67;

        TqSocketHandle fd = TqConnectLocal(port);
        if (!TqSocketValid(fd)) return 68;
        if (!TqSendAll(fd, "PUT /config HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Length: 500\r\nTransfer-Encoding: chunked\r\n\r\n")) return 69;

        std::string response;
        if (!TqRecvUntilClosed(fd, response)) return 70;
        TqCloseSocket(fd);
        server.Stop();
        if (!TqHttpStatusIs(response, 400)) return 71;
    }
    {
        std::string err;
        TqAdminHttpServer server("127.0.0.1:0", [&](const TqHttpRequest&) {
            return TqJsonResponse(200, "{}");
        });
        if (!server.Start(err)) return 72;
        const uint16_t port = TqPortFromListenAddress(server.ListenAddress());
        if (port == 0) return 73;

        TqSocketHandle fd = TqConnectLocal(port);
        if (!TqSocketValid(fd)) return 74;
        if (!TqSendAll(fd, "PUT /config HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Length: 500\r\nContent-Length: 1\r\n\r\n")) return 75;

        std::string response;
        if (!TqRecvUntilClosed(fd, response)) return 76;
        TqCloseSocket(fd);
        server.Stop();
        if (!TqHttpStatusIs(response, 400)) return 77;
    }
    {
        std::string err;
        TqAdminHttpServer server("127.0.0.1:0", [&](const TqHttpRequest&) {
            return TqJsonResponse(200, "{\"ok\":true}");
        });
        if (!server.Start(err)) return 50;
        const uint16_t port = TqPortFromListenAddress(server.ListenAddress());
        if (port == 0) return 51;

        TqSocketHandle closedFd = TqConnectLocal(port);
        if (!TqSocketValid(closedFd)) return 52;
        if (!TqSendAll(closedFd, "GET /health HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n")) return 53;
        TqCloseSocket(closedFd);

        TqSocketHandle stalledFd1 = TqConnectLocal(port);
        TqSocketHandle stalledFd2 = TqConnectLocal(port);
        if (!TqSocketValid(stalledFd1) || !TqSocketValid(stalledFd2)) return 54;
        TqCloseSocket(stalledFd1);

        TqSocketHandle fd = TqConnectLocal(port);
        if (!TqSocketValid(fd)) return 55;
        if (!TqSendAll(fd, "GET /health HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n")) return 56;
        std::string response;
        if (!TqRecvUntilClosed(fd, response)) return 57;
        TqCloseSocket(fd);
        TqCloseSocket(stalledFd2);
        server.Stop();
        if (!TqHttpStatusIs(response, 200)) return 58;
    }
    {
        std::string err;
        TqAdminHttpServer server("127.0.0.1:0", [&](const TqHttpRequest&) {
            return TqJsonResponse(200, "{\"many\":true}");
        });
        if (!server.Start(err)) return 59;
        const uint16_t port = TqPortFromListenAddress(server.ListenAddress());
        if (port == 0) return 60;

        for (int i = 0; i < 1024; ++i) {
            TqSocketHandle fd = TqConnectLocal(port);
            if (!TqSocketValid(fd)) return 61;
            if (!TqSendAll(fd, "GET /health HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n")) return 62;
            std::string response;
            if (!TqRecvUntilClosed(fd, response)) return 63;
            TqCloseSocket(fd);
            if (!TqHttpStatusIs(response, 200)) return 64;
        }

        TqSocketHandle stalledFd = TqConnectLocal(port);
        if (!TqSocketValid(stalledFd)) return 65;
        std::thread stopper([&server]() { server.Stop(); });
        stopper.join();
        TqCloseSocket(stalledFd);
    }
    {
        TqAdminHttpServerOptions options;
        options.AdminThreads = 2;
        options.TokenFile = TqSecureTokenFile("auth").string();
        options.AllowUnauthenticatedLegacy = false;
        std::string err;
        TqAdminHttpServer server("127.0.0.1:0", [&](const TqHttpRequest& req) {
            if (req.Method == "GET" && req.Path == "/health") {
                return TqJsonResponse(200, "{\"ok\":true}");
            }
            return TqJsonResponse(404, "{}");
        }, options);
        if (!server.Start(err)) return 94;
        const uint16_t port = TqPortFromListenAddress(server.ListenAddress());
        if (port == 0) return 95;

        TqSocketHandle unauthFd = TqConnectLocal(port);
        if (!TqSocketValid(unauthFd)) return 96;
        if (!TqSendAll(unauthFd, "GET /api/v1/health HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n")) return 97;
        std::string unauthResponse;
        if (!TqRecvUntilClosed(unauthFd, unauthResponse)) return 98;
        TqCloseSocket(unauthFd);
        if (!TqHttpStatusIs(unauthResponse, 401)) return 99;

        TqSocketHandle authFd = TqConnectLocal(port);
        if (!TqSocketValid(authFd)) return 100;
        const std::string request = "GET /api/v1/health HTTP/1.1\r\nHost: 127.0.0.1\r\nAuthorization: Bearer " +
            server.AuthTokenForTesting() + "\r\n\r\n";
        if (!TqSendAll(authFd, request)) return 101;
        std::string authResponse;
        if (!TqRecvUntilClosed(authFd, authResponse)) return 102;
        TqCloseSocket(authFd);
        server.Stop();
        if (!TqHttpStatusIs(authResponse, 200)) return 103;
        if (authResponse.find("\"ok\":true") == std::string::npos) return 104;
    }
    {
        TqAdminHttpServerOptions options;
        options.TokenFile = TqSecureTokenFile("v1-peers").string();
        std::string err;
        TqAdminHttpServer server("127.0.0.1:0", [&](const TqHttpRequest& req) {
            if (req.Method == "POST" && req.Path == "/peers/agent-d:enable") {
                return TqJsonResponse(202, "{\"right_v1_shape\":true}");
            }
            if (req.Method == "POST" && req.Path == "/peers/agent-d/enable") {
                return TqJsonResponse(200, "{\"wrong_v1_shape\":true}");
            }
            if (req.Method == "POST" && req.Path == "/peers/agent-d/connections/conn-1:reconnect") {
                return TqJsonResponse(202, "{\"connection_action\":true}");
            }
            if (req.Method == "POST" && req.Path == "/peers/agent-d/connections/conn-1/reconnect") {
                return TqJsonResponse(200, "{\"wrong_connection_shape\":true}");
            }
            return TqJsonResponse(404, "{}");
        }, options);
        if (!server.Start(err)) return 120;
        const uint16_t port = TqPortFromListenAddress(server.ListenAddress());
        if (port == 0) return 121;

        TqSocketHandle fd = TqConnectLocal(port);
        if (!TqSocketValid(fd)) return 122;
        const std::string request = "POST /api/v1/peers/agent-d/enable HTTP/1.1\r\nHost: 127.0.0.1\r\nAuthorization: Bearer " +
            server.AuthTokenForTesting() + "\r\nContent-Length: 0\r\n\r\n";
        if (!TqSendAll(fd, request)) return 123;
        std::string response;
        if (!TqRecvUntilClosed(fd, response)) return 124;
        TqCloseSocket(fd);
        if (!TqHttpStatusIs(response, 404)) return 125;

        TqSocketHandle rightFd = TqConnectLocal(port);
        if (!TqSocketValid(rightFd)) return 129;
        const std::string rightRequest = "POST /api/v1/peers/agent-d:enable HTTP/1.1\r\nHost: 127.0.0.1\r\nAuthorization: Bearer " +
            server.AuthTokenForTesting() + "\r\nContent-Length: 0\r\n\r\n";
        if (!TqSendAll(rightFd, rightRequest)) return 130;
        std::string rightResponse;
        if (!TqRecvUntilClosed(rightFd, rightResponse)) return 131;
        TqCloseSocket(rightFd);
        if (!TqHttpStatusIs(rightResponse, 202)) return 132;

        TqSocketHandle connectionFd = TqConnectLocal(port);
        if (!TqSocketValid(connectionFd)) return 133;
        const std::string connectionRequest =
            "POST /api/v1/peers/agent-d/connections/conn-1:reconnect HTTP/1.1\r\nHost: 127.0.0.1\r\nAuthorization: Bearer " +
            server.AuthTokenForTesting() + "\r\nContent-Length: 0\r\n\r\n";
        if (!TqSendAll(connectionFd, connectionRequest)) return 134;
        std::string connectionResponse;
        if (!TqRecvUntilClosed(connectionFd, connectionResponse)) return 135;
        TqCloseSocket(connectionFd);
        if (!TqHttpStatusIs(connectionResponse, 202)) return 136;

        TqSocketHandle wrongConnectionFd = TqConnectLocal(port);
        if (!TqSocketValid(wrongConnectionFd)) return 137;
        const std::string wrongConnectionRequest =
            "POST /api/v1/peers/agent-d/connections/conn-1/reconnect HTTP/1.1\r\nHost: 127.0.0.1\r\nAuthorization: Bearer " +
            server.AuthTokenForTesting() + "\r\nContent-Length: 0\r\n\r\n";
        if (!TqSendAll(wrongConnectionFd, wrongConnectionRequest)) return 138;
        std::string wrongConnectionResponse;
        if (!TqRecvUntilClosed(wrongConnectionFd, wrongConnectionResponse)) return 139;
        TqCloseSocket(wrongConnectionFd);
        server.Stop();
        if (!TqHttpStatusIs(wrongConnectionResponse, 404)) return 140;
    }
    {
        TqAdminHttpServerOptions options;
        options.TokenFile = TqSecureTokenFile("legacy").string();
        options.AllowUnauthenticatedLegacy = true;
        std::string err;
        TqAdminHttpServer server("127.0.0.1:0", [&](const TqHttpRequest& req) {
            if (req.Method == "GET" && req.Path == "/health") {
                return TqJsonResponse(200, "{\"legacy\":true}");
            }
            return TqJsonResponse(404, "{}");
        }, options);
        if (!server.Start(err)) return 105;
        const uint16_t port = TqPortFromListenAddress(server.ListenAddress());
        if (port == 0) return 106;
        TqSocketHandle fd = TqConnectLocal(port);
        if (!TqSocketValid(fd)) return 107;
        if (!TqSendAll(fd, "GET /health HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n")) return 108;
        std::string response;
        if (!TqRecvUntilClosed(fd, response)) return 109;
        TqCloseSocket(fd);
        server.Stop();
        if (!TqHttpStatusIs(response, 200)) return 110;
        if (response.find("\"legacy\":true") == std::string::npos) return 111;
    }
    {
        TqAdminHttpServerOptions options;
        options.TokenFile = TqSecureTokenFile("legacy-peer-scope").string();
        options.AllowUnauthenticatedLegacy = true;
        std::string err;
        TqAdminHttpServer server("127.0.0.1:0", [&](const TqHttpRequest& req) {
            if (req.Method == "POST" && req.Path == "/peers/agent-d/enable") {
                return TqJsonResponse(200, "{\"legacy_enable\":true}");
            }
            if (req.Path.compare(0, 7, "/peers/") == 0) {
                return TqJsonResponse(200, "{\"legacy_exposed\":true}");
            }
            return TqJsonResponse(404, "{}");
        }, options);
        if (!server.Start(err)) return 133;
        const uint16_t port = TqPortFromListenAddress(server.ListenAddress());
        if (port == 0) return 134;

        TqSocketHandle fd = TqConnectLocal(port);
        if (!TqSocketValid(fd)) return 135;
        if (!TqSendAll(fd, "PATCH /peers/agent-d HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Length: 2\r\n\r\n{}")) return 136;
        std::string patchResponse;
        if (!TqRecvUntilClosed(fd, patchResponse)) return 137;
        TqCloseSocket(fd);
        if (!TqHttpStatusIs(patchResponse, 404)) return 138;

        TqSocketHandle legacyFd = TqConnectLocal(port);
        if (!TqSocketValid(legacyFd)) return 139;
        if (!TqSendAll(legacyFd, "POST /peers/agent-d/enable HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Length: 0\r\n\r\n")) return 140;
        std::string legacyResponse;
        if (!TqRecvUntilClosed(legacyFd, legacyResponse)) return 141;
        TqCloseSocket(legacyFd);
        server.Stop();
        if (!TqHttpStatusIs(legacyResponse, 200)) return 142;
    }
    return 0;
}
