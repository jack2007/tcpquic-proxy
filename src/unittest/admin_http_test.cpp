#include "admin_http.h"

#include <arpa/inet.h>
#include <cstring>
#include <netinet/in.h>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

namespace {

int TqConnectLocal(uint16_t port) {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        return -1;
    }

    timeval timeout{};
    timeout.tv_sec = 2;
    (void)::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr) != 1 ||
        ::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        ::close(fd);
        return -1;
    }

    return fd;
}

bool TqSendAll(int fd, const std::string& data) {
    size_t sent = 0;
    while (sent < data.size()) {
        ssize_t result = ::send(fd, data.data() + sent, data.size() - sent, 0);
        if (result <= 0) {
            return false;
        }
        sent += static_cast<size_t>(result);
    }
    return true;
}

bool TqRecvUntilClosed(int fd, std::string& data) {
    char buffer[256];
    for (;;) {
        ssize_t result = ::recv(fd, buffer, sizeof(buffer), 0);
        if (result < 0) {
            return false;
        }
        if (result == 0) {
            return true;
        }
        data.append(buffer, static_cast<size_t>(result));
    }
}

uint16_t TqPortFromListenAddress(const std::string& listen) {
    size_t colon = listen.rfind(':');
    if (colon == std::string::npos || colon + 1 == listen.size()) {
        return 0;
    }
    return static_cast<uint16_t>(std::stoul(listen.substr(colon + 1)));
}

} // namespace

int main() {
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
        if (TqJsonResponse(404, "{}").find("HTTP/1.1 404 Not Found\r\n") != 0) return 17;
        if (TqJsonResponse(500, "{}").find("HTTP/1.1 500 Internal Server Error\r\n") != 0) return 18;
    }
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
        std::string raw = "PUT /config HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Length: 18446744073709551615\r\n\r\n{}";
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

        int fd = TqConnectLocal(port);
        if (fd < 0) return 27;
        if (!TqSendAll(fd, "GET /health HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n")) return 28;

        std::string response;
        if (!TqRecvUntilClosed(fd, response)) return 29;
        ::close(fd);
        server.Stop();
        if (response != expected) return 30;
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

        int stalledFd = TqConnectLocal(port);
        if (stalledFd < 0) return 36;

        int fd = TqConnectLocal(port);
        if (fd < 0) return 37;
        if (!TqSendAll(fd, "GET /health HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n")) return 38;

        std::string response;
        if (!TqRecvUntilClosed(fd, response)) return 39;
        ::close(fd);
        ::close(stalledFd);
        server.Stop();
        if (response != expected) return 40;
    }
    {
        std::string err;
        TqAdminHttpServer server("127.0.0.1:0", [&](const TqHttpRequest&) {
            return TqJsonResponse(200, "{}");
        });
        if (!server.Start(err)) return 41;
        const uint16_t port = TqPortFromListenAddress(server.ListenAddress());
        if (port == 0) return 42;
        int stalledFd = TqConnectLocal(port);
        if (stalledFd < 0) return 43;
        std::thread stopper([&server]() { server.Stop(); });
        stopper.join();
        ::close(stalledFd);
    }
    {
        std::string err;
        TqAdminHttpServer server("127.0.0.1:0", [&](const TqHttpRequest&) {
            return TqJsonResponse(200, "{}");
        });
        if (!server.Start(err)) return 44;
        const uint16_t port = TqPortFromListenAddress(server.ListenAddress());
        if (port == 0) return 45;

        int fd = TqConnectLocal(port);
        if (fd < 0) return 46;
        if (!TqSendAll(fd, "PUT /config HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Length: 18446744073709551615\r\n\r\n")) return 47;

        std::string response;
        if (!TqRecvUntilClosed(fd, response)) return 48;
        ::close(fd);
        server.Stop();
        if (response.find("HTTP/1.1 400 Bad Request\r\n") != 0) return 49;
    }
    {
        std::string err;
        TqAdminHttpServer server("127.0.0.1:0", [&](const TqHttpRequest&) {
            return TqJsonResponse(200, "{}");
        });
        if (!server.Start(err)) return 66;
        const uint16_t port = TqPortFromListenAddress(server.ListenAddress());
        if (port == 0) return 67;

        int fd = TqConnectLocal(port);
        if (fd < 0) return 68;
        if (!TqSendAll(fd, "PUT /config HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Length: 500\r\nTransfer-Encoding: chunked\r\n\r\n")) return 69;

        std::string response;
        if (!TqRecvUntilClosed(fd, response)) return 70;
        ::close(fd);
        server.Stop();
        if (response.find("HTTP/1.1 400 Bad Request\r\n") != 0) return 71;
    }
    {
        std::string err;
        TqAdminHttpServer server("127.0.0.1:0", [&](const TqHttpRequest&) {
            return TqJsonResponse(200, "{}");
        });
        if (!server.Start(err)) return 72;
        const uint16_t port = TqPortFromListenAddress(server.ListenAddress());
        if (port == 0) return 73;

        int fd = TqConnectLocal(port);
        if (fd < 0) return 74;
        if (!TqSendAll(fd, "PUT /config HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Length: 500\r\nContent-Length: 1\r\n\r\n")) return 75;

        std::string response;
        if (!TqRecvUntilClosed(fd, response)) return 76;
        ::close(fd);
        server.Stop();
        if (response.find("HTTP/1.1 400 Bad Request\r\n") != 0) return 77;
    }
    {
        std::string err;
        TqAdminHttpServer server("127.0.0.1:0", [&](const TqHttpRequest&) {
            return TqJsonResponse(200, "{\"ok\":true}");
        });
        if (!server.Start(err)) return 50;
        const uint16_t port = TqPortFromListenAddress(server.ListenAddress());
        if (port == 0) return 51;

        int closedFd = TqConnectLocal(port);
        if (closedFd < 0) return 52;
        if (!TqSendAll(closedFd, "GET /health HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n")) return 53;
        ::close(closedFd);

        int stalledFd1 = TqConnectLocal(port);
        int stalledFd2 = TqConnectLocal(port);
        if (stalledFd1 < 0 || stalledFd2 < 0) return 54;
        ::close(stalledFd1);

        int fd = TqConnectLocal(port);
        if (fd < 0) return 55;
        if (!TqSendAll(fd, "GET /health HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n")) return 56;
        std::string response;
        if (!TqRecvUntilClosed(fd, response)) return 57;
        ::close(fd);
        ::close(stalledFd2);
        server.Stop();
        if (response.find("HTTP/1.1 200 OK\r\n") != 0) return 58;
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
            int fd = TqConnectLocal(port);
            if (fd < 0) return 61;
            if (!TqSendAll(fd, "GET /health HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n")) return 62;
            std::string response;
            if (!TqRecvUntilClosed(fd, response)) return 63;
            ::close(fd);
            if (response.find("HTTP/1.1 200 OK\r\n") != 0) return 64;
        }

        int stalledFd = TqConnectLocal(port);
        if (stalledFd < 0) return 65;
        std::thread stopper([&server]() { server.Stop(); });
        stopper.join();
        ::close(stalledFd);
    }
    return 0;
}
