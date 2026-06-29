#include "admin_http.h"
#include "admin_auth.h"
#include "admin_console.h"
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

bool TqHttpHeaderIs(const std::string& response, const std::string& name, const std::string& value) {
    const std::string header = name + ": " + value + "\r\n";
    const size_t headerEnd = response.find("\r\n\r\n");
    return headerEnd != std::string::npos && response.find(header) < headerEnd;
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
        const std::string_view html = TqAdminConsoleHtml();
        const std::string_view css = TqAdminConsoleCss();
        const std::string_view js = TqAdminConsoleJs();
        if (html.find("raypx2 Admin Console") == std::string_view::npos) return 300;
        if (html.find("role-pill") == std::string_view::npos) return 301;
        if (html.find("<strong>listen</strong>") != std::string_view::npos) return 302;
        if (html.find("Create/Edit Peer") == std::string_view::npos) return 303;
        if (html.find("remote_identity") != std::string_view::npos) return 304;
        if (html.find("transferred_bytes") != std::string_view::npos) return 305;
        if (css.find(".sidebar") == std::string_view::npos) return 306;
        if (js.find("sessionStorage") == std::string_view::npos) return 307;
        if (css.find(".span-4") == std::string_view::npos) return 308;
        if (css.find(".span-8") == std::string_view::npos) return 309;
        if (js.find("username === 'raypx2'") == std::string_view::npos) return 310;
        if (js.find("sessionStorage.setItem('tcpquic_admin_token'") == std::string_view::npos) return 311;
        if (js.find("GET /api/v1/admin") != std::string_view::npos) return 312;
        if (js.find("api('/admin'") == std::string_view::npos) return 313;
        if (js.find("setInterval(refreshCurrentPage, 3000)") == std::string_view::npos) return 314;
        if (js.find("JSON.stringify") == std::string_view::npos) return 315;
        if (js.find("typeof data.error === 'string'") == std::string_view::npos) return 316;
        if (js.find("refreshCurrentPageNow") == std::string_view::npos) return 317;
        if (js.find("catch (error) {\n        healthPill.innerHTML = '<span class=\"dot warn\"></span><strong>health</strong> degraded';\n        refreshPill.innerHTML = `<strong>refresh</strong> ${escapeHtml(error.message)}`;") == std::string_view::npos) return 318;
        if (js.find("button.onclick = () => runClientAction(() => deletePeer(button.dataset.deletePeer));") == std::string_view::npos) return 319;
        if (js.find("renderRows(tbody, rows, ['peer_id','state','enabled','quic_peer','socks_listen','http_listen','connection_count','connected_connections','active_streams','total_streams','reconnects','last_error']);\n      if (!tbody) return;") == std::string_view::npos) return 320;
        if (js.find("peerMode: 'create'") == std::string_view::npos) return 321;
        if (js.find("function beginCreatePeer()") == std::string_view::npos) return 322;
        if (js.find("document.getElementById('peer-create').onclick = beginCreatePeer;") == std::string_view::npos) return 323;
        if (js.find("document.getElementById('peer-create').onclick = () => runClientAction(createPeer)") != std::string_view::npos) return 324;
        if (js.find("if (consoleState.peerMode === 'create')") == std::string_view::npos) return 325;
        if (html.find("role-switch") != std::string_view::npos) return 414;
        if (js.find("querySelectorAll('.role-switch button')") != std::string_view::npos) return 415;
        if (js.find("sessionStorage.setItem('tcpquic.admin.role'") != std::string_view::npos) return 416;
        if (html.find("<div class=\"peer-layout\">") != std::string_view::npos) return 417;
        if (html.find("<section class=\"peer-form-panel\"") == std::string_view::npos) return 418;
        if (html.find("<div class=\"peer-create-footer\"><button class=\"btn primary\" id=\"peer-create\">Create Peer</button></div>") == std::string_view::npos) return 419;
        if (html.find("id=\"client-connections-peer\"") != std::string_view::npos) return 420;
        if (html.find("<th>actions</th>") == std::string_view::npos) return 421;
        if (html.find("id=\"client-connection-detail\"") == std::string_view::npos) return 422;
        if (js.find("async function loadAllClientConnections()") == std::string_view::npos) return 423;
        if (js.find("renderClientConnectionDetail") == std::string_view::npos) return 424;
        if (js.find("setRole('client');") != std::string_view::npos) return 425;
        if (js.find("Object.assign({}, row, { peer_id: peerId })") == std::string_view::npos) return 426;
        if (html.find("client-overview") == std::string_view::npos) return 334;
        if (html.find("client-peers") == std::string_view::npos) return 335;
        if (html.find("client-connections") == std::string_view::npos) return 336;
        if (html.find("client-tunnels") == std::string_view::npos) return 337;
        if (html.find("peers address - quic_peer") == std::string_view::npos) return 338;
        if (html.find("paths 模式") == std::string_view::npos) return 339;
        if (html.find("127.0.0.1:1080") == std::string_view::npos) return 340;
        if (html.find("127.0.0.1:8080") == std::string_view::npos) return 341;
        if (html.find("connected_at") != std::string_view::npos) return 342;
        if (html.find("disconnected_at") != std::string_view::npos) return 343;
        if (html.find("<th>source</th>") != std::string_view::npos) return 344;
        if (js.find("renderClientOverview") == std::string_view::npos) return 345;
        if (js.find("renderClientPeers") == std::string_view::npos) return 346;
        if (js.find("renderClientConnections") == std::string_view::npos) return 347;
        if (js.find("renderClientTunnels") == std::string_view::npos) return 348;
        if (html.find("server-overview") == std::string_view::npos) return 349;
        if (html.find("server-peers") == std::string_view::npos) return 350;
        if (html.find("server-connections") == std::string_view::npos) return 351;
        if (html.find("server-tunnels") == std::string_view::npos) return 352;
        if (html.find("server-acl") == std::string_view::npos) return 353;
        if (html.find("remote source") == std::string_view::npos) return 354;
        if (html.find("remote_identity") != std::string_view::npos) return 355;
        if (html.find("first_seen") != std::string_view::npos) return 356;
        if (html.find("last_seen") != std::string_view::npos) return 357;
        if (html.find("transferred_bytes") != std::string_view::npos) return 358;
        if (js.find("renderServerOverview") == std::string_view::npos) return 359;
        if (js.find("renderServerPeers") == std::string_view::npos) return 360;
        if (js.find("renderServerConnections") == std::string_view::npos) return 361;
        if (js.find("renderServerTunnels") == std::string_view::npos) return 362;
        if (js.find("renderServerAcl") == std::string_view::npos) return 363;
        if (js.find("remoteHostFromAddress") == std::string_view::npos) return 364;
        if (js.find("startsWith('[')") == std::string_view::npos) return 365;
        if (js.find("lastIndexOf(':')") == std::string_view::npos) return 366;
        if (js.find("Array.isArray(config.allow_targets)") == std::string_view::npos) return 367;
        if (js.find("renderRelay") == std::string_view::npos) return 368;
        if (js.find("renderConfig") == std::string_view::npos) return 369;
        if (js.find("renderDiagnostics") == std::string_view::npos) return 370;
        if (js.find("allocator:dump") == std::string_view::npos) return 371;
        if (js.find("not_supported") == std::string_view::npos) return 372;
        if (js.find("JSON.stringify") == std::string_view::npos) return 373;
        if (js.find("setElementText") == std::string_view::npos) return 383;
        if (js.find("setElementJson") == std::string_view::npos) return 384;
        if (js.find("if (configSave)") == std::string_view::npos) return 385;
        if (js.find("if (allocatorDump)") == std::string_view::npos) return 386;
        if (html.find("relay-backend") == std::string_view::npos) return 374;
        if (html.find("relay-active") == std::string_view::npos) return 375;
        if (html.find("relay-pending") == std::string_view::npos) return 376;
        if (html.find("relay-errors") == std::string_view::npos) return 377;
        if (html.find("config-json") == std::string_view::npos) return 378;
        if (html.find("config-save") == std::string_view::npos) return 379;
        if (html.find("diagnostics-json") == std::string_view::npos) return 380;
        if (html.find("allocator-dump") == std::string_view::npos) return 381;
        if (html.find("allocator-json") == std::string_view::npos) return 382;
        if (html.find("server overview") != std::string_view::npos) return 387;
        if (html.find("Server Overview") != std::string_view::npos) return 388;
        if (html.find("Drain</button>") != std::string_view::npos) return 389;
        if (html.find("Abort</button>") != std::string_view::npos) return 390;
        if (html.find("Add highest slot") != std::string_view::npos) return 391;
        if (html.find("<th>connected_at</th>") != std::string_view::npos) return 392;
        if (html.find("<th>disconnected_at</th>") != std::string_view::npos) return 393;
        if (html.find("<th>transferred_bytes</th>") != std::string_view::npos) return 394;
        if (html.find("<strong>listen</strong>") != std::string_view::npos) return 395;
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
        in.close();
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
    {
        TqAdminAuth::SetRuntimeBinaryName("/opt/tcpquic/bin/custom-raypx2");
        const std::filesystem::path defaultTokenFile = TqAdminAuth::DefaultTokenFilePath("client");
        if (defaultTokenFile.filename().string().find("client-admin-") != 0) return 166;
        if (defaultTokenFile.extension() != ".json") return 167;
        const std::string runtimeDir = defaultTokenFile.parent_path().filename().string();
        if (runtimeDir != "custom-raypx2" && runtimeDir.find("custom-raypx2-") != 0) return 168;
        const std::filesystem::path serverTokenFile = TqAdminAuth::DefaultTokenFilePath("server");
        if (serverTokenFile.filename().string().find("server-admin-") != 0) return 169;

        TqAdminAuth::SetRuntimeBinaryName("C:\\tcpquic\\bin\\raypx2.exe");
        const std::filesystem::path windowsTokenFile = TqAdminAuth::DefaultTokenFilePath("client");
        const std::string windowsRuntimeDir = windowsTokenFile.parent_path().filename().string();
        if (windowsRuntimeDir != "raypx2" && windowsRuntimeDir.find("raypx2-") != 0) return 170;
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
        if (!TqValidateAdminListen("0.0.0.0:19091", err)) return 14;
        if (!TqValidateAdminListen("172.16.10.80:19091", err)) return 15;
        if (!TqValidateAdminListen("192.168.1.10:19091", err)) return 310;
        if (TqValidateAdminListen("127.0.0.1:notaport", err)) return 22;
        if (TqValidateAdminListen(":19091", err)) return 311;
        if (TqValidateAdminListen("127.0.0.1:0", err)) return 23;
        if (TqValidateAdminListen("127.0.0.1:65536", err)) return 24;
        if (TqValidateAdminListen("127.0.0.1:", err)) return 25;
    }
    {
        std::string err;
        TqAdminHttpServer server("0.0.0.0:0", [&](const TqHttpRequest&) {
            return TqJsonResponse(200, "{}");
        });
        if (!server.Start(err)) return 312;
        if (server.ListenAddress().rfind("0.0.0.0:", 0) != 0) return 313;
        const uint16_t port = TqPortFromListenAddress(server.ListenAddress());
        if (port == 0) return 314;
        server.Stop();
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
        if (!TqSendAll(fd, "GET /api/v1/health HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n")) return 28;

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
        if (!TqSendAll(fd, "GET /api/v1/health HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n")) return 38;

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
        if (!TqSendAll(closedFd, "GET /api/v1/health HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n")) return 53;
        TqCloseSocket(closedFd);

        TqSocketHandle stalledFd1 = TqConnectLocal(port);
        TqSocketHandle stalledFd2 = TqConnectLocal(port);
        if (!TqSocketValid(stalledFd1) || !TqSocketValid(stalledFd2)) return 54;
        TqCloseSocket(stalledFd1);

        TqSocketHandle fd = TqConnectLocal(port);
        if (!TqSocketValid(fd)) return 55;
        if (!TqSendAll(fd, "GET /api/v1/health HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n")) return 56;
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
            if (!TqSendAll(fd, "GET /api/v1/health HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n")) return 62;
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

        TqSocketHandle rootFd = TqConnectLocal(port);
        if (!TqSocketValid(rootFd)) return 180;
        if (!TqSendAll(rootFd, "GET / HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n")) return 181;
        std::string rootResponse;
        if (!TqRecvUntilClosed(rootFd, rootResponse)) return 182;
        TqCloseSocket(rootFd);
        if (!TqHttpStatusIs(rootResponse, 302)) return 183;
        if (!TqHttpHeaderIs(rootResponse, "Location", "/console/")) return 184;

        TqSocketHandle consoleFd = TqConnectLocal(port);
        if (!TqSocketValid(consoleFd)) return 185;
        if (!TqSendAll(consoleFd, "GET /console/ HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n")) return 186;
        std::string consoleResponse;
        if (!TqRecvUntilClosed(consoleFd, consoleResponse)) return 187;
        TqCloseSocket(consoleFd);
        if (!TqHttpStatusIs(consoleResponse, 200)) return 188;
        if (!TqHttpHeaderIs(consoleResponse, "Content-Type", "text/html")) return 189;
        if (consoleResponse.find("raypx2 Admin Console") == std::string::npos) return 190;
        if (consoleResponse.find("<strong>listen</strong>") != std::string::npos) return 191;

        TqSocketHandle cssFd = TqConnectLocal(port);
        if (!TqSocketValid(cssFd)) return 192;
        if (!TqSendAll(cssFd, "GET /console/style.css HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n")) return 193;
        std::string cssResponse;
        if (!TqRecvUntilClosed(cssFd, cssResponse)) return 194;
        TqCloseSocket(cssFd);
        if (!TqHttpStatusIs(cssResponse, 200)) return 195;
        if (!TqHttpHeaderIs(cssResponse, "Content-Type", "text/css")) return 196;
        if (cssResponse.find(".sidebar") == std::string::npos) return 197;

        TqSocketHandle putCssFd = TqConnectLocal(port);
        if (!TqSocketValid(putCssFd)) return 204;
        if (!TqSendAll(putCssFd, "PUT /console/style.css HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n")) return 205;
        std::string putCssResponse;
        if (!TqRecvUntilClosed(putCssFd, putCssResponse)) return 206;
        TqCloseSocket(putCssFd);
        if (!TqHttpStatusIs(putCssResponse, 404)) return 207;
        if (putCssResponse.find(".sidebar") != std::string::npos) return 208;

        TqSocketHandle jsFd = TqConnectLocal(port);
        if (!TqSocketValid(jsFd)) return 198;
        if (!TqSendAll(jsFd, "GET /console/app.js HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n")) return 199;
        std::string jsResponse;
        if (!TqRecvUntilClosed(jsFd, jsResponse)) return 200;
        TqCloseSocket(jsFd);
        if (!TqHttpStatusIs(jsResponse, 200)) return 201;
        if (!TqHttpHeaderIs(jsResponse, "Content-Type", "application/javascript")) return 202;
        if (jsResponse.find("sessionStorage") == std::string::npos) return 203;

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
        options.AdminThreads = 2;
        options.TokenFile = TqSecureTokenFile("console-smoke").string();
        options.Role = "server";
        std::string err;
        TqAdminHttpServer server("127.0.0.1:0", [&](const TqHttpRequest& req) {
            if (req.Method == "GET" && req.Path == "/health") {
                return TqJsonResponse(200, "{\"role\":\"server\",\"status\":\"healthy\",\"uptime_seconds\":1}");
            }
            if (req.Method == "GET" && req.Path == "/metrics") {
                return TqJsonResponse(200, "{\"status\":\"healthy\",\"acl_denied\":0}");
            }
            if (req.Method == "GET" && req.Path == "/server/connections") {
                return TqJsonResponse(200, "{\"connections\":[]}");
            }
            if (req.Method == "GET" && req.Path == "/server/tunnels") {
                return TqJsonResponse(200, "{\"tunnels\":[]}");
            }
            if (req.Method == "GET" && req.Path == "/server/config") {
                return TqJsonResponse(200, "{\"role\":\"server\",\"allow_targets\":[\"0.0.0.0/0\"],\"deny_targets\":[]}");
            }
            return TqJsonResponse(404, "{}");
        }, options);
        if (!server.Start(err)) return 396;
        const uint16_t port = TqPortFromListenAddress(server.ListenAddress());
        if (port == 0) return 397;

        auto sendRequest = [&](const std::string& requestText, std::string& out) -> int {
            TqSocketHandle requestFd = TqConnectLocal(port);
            if (!TqSocketValid(requestFd)) return 398;
            if (!TqSendAll(requestFd, requestText)) {
                TqCloseSocket(requestFd);
                return 399;
            }
            if (!TqRecvUntilClosed(requestFd, out)) {
                TqCloseSocket(requestFd);
                return 400;
            }
            TqCloseSocket(requestFd);
            return 0;
        };

        int smokeResult = 0;
        std::string consoleResponse;
        smokeResult = sendRequest("GET /console/ HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n", consoleResponse);
        if (smokeResult == 0 && !TqHttpStatusIs(consoleResponse, 200)) smokeResult = 401;
        if (smokeResult == 0 && consoleResponse.find("server-acl") == std::string::npos) smokeResult = 402;

        std::string configResponse;
        if (smokeResult == 0) {
            const std::string configRequest =
                "GET /api/v1/server/config HTTP/1.1\r\nHost: 127.0.0.1\r\nAuthorization: Bearer " +
                server.AuthTokenForTesting() + "\r\n\r\n";
            smokeResult = sendRequest(configRequest, configResponse);
        }
        if (smokeResult == 0 && !TqHttpStatusIs(configResponse, 200)) smokeResult = 403;
        if (smokeResult == 0 && configResponse.find("\"allow_targets\"") == std::string::npos) smokeResult = 404;

        server.Stop();
        if (smokeResult != 0) return smokeResult;
    }
    {
        TqAdminHttpServerOptions options;
        options.AdminThreads = 1;
        options.EnableTokenAuth = true;
        options.Role = "client";
        std::string err;
        TqAdminHttpServer server("127.0.0.1:0", [&](const TqHttpRequest&) {
            return TqJsonResponse(500, "{\"handler_called\":true}");
        }, options);
        if (!server.Start(err)) return 171;
        const uint16_t port = TqPortFromListenAddress(server.ListenAddress());
        if (port == 0) return 172;

        TqSocketHandle fd = TqConnectLocal(port);
        if (!TqSocketValid(fd)) return 173;
        const std::string request = "GET /api/v1/admin HTTP/1.1\r\nHost: 127.0.0.1\r\nAuthorization: Bearer " +
            server.AuthTokenForTesting() + "\r\n\r\n";
        if (!TqSendAll(fd, request)) return 174;
        std::string response;
        if (!TqRecvUntilClosed(fd, response)) return 175;
        TqCloseSocket(fd);
        server.Stop();
        if (!TqHttpStatusIs(response, 200)) return 176;
        if (response.find("\"role\":\"client\"") == std::string::npos) return 177;
        if (response.find("\"token_present\":true") == std::string::npos) return 178;
        if (response.find("\"token\":\"") != std::string::npos) return 179;
    }
    {
        TqAdminHttpServerOptions options;
        options.AdminThreads = 1;
        options.EnableTokenAuth = true;
        options.Role = "server";
        std::string err;
        TqAdminHttpServer server("0.0.0.0:0", [&](const TqHttpRequest&) {
            return TqJsonResponse(500, "{\"handler_called\":true}");
        }, options);
        if (!server.Start(err)) return 405;
        const uint16_t port = TqPortFromListenAddress(server.ListenAddress());
        if (port == 0) return 406;

        TqSocketHandle fd = TqConnectLocal(port);
        if (!TqSocketValid(fd)) return 407;
        const std::string request = "GET /api/v1/admin HTTP/1.1\r\nHost: 127.0.0.1\r\nAuthorization: Bearer " +
            server.AuthTokenForTesting() + "\r\n\r\n";
        if (!TqSendAll(fd, request)) return 408;
        std::string response;
        if (!TqRecvUntilClosed(fd, response)) return 409;
        TqCloseSocket(fd);
        server.Stop();
        if (!TqHttpStatusIs(response, 200)) return 410;
        if (response.find("\"listen\":\"0.0.0.0:") == std::string::npos) return 411;
        if (response.find("\"loopback_only\":false") == std::string::npos) return 412;
        if (response.find("\"loopback_only\":true") != std::string::npos) return 413;
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
            if (req.Method == "GET" && req.Path == "/tunnels") {
                return TqJsonResponse(200, "{\"tunnels\":[]}");
            }
            if (req.Method == "POST" && req.Path == "/tunnels/tun-1:abort") {
                return TqJsonResponse(202, "{\"tunnel_abort\":true}");
            }
            if (req.Method == "GET" && req.Path == "/relay/active-relays") {
                return TqJsonResponse(200, "{\"active_relays\":true}");
            }
            if (req.Method == "GET" && req.Path == "/relay/active-relays/relay-1") {
                return TqJsonResponse(200, "{\"relay_id\":\"relay-1\"}");
            }
            if (req.Method == "GET" && req.Path == "/relay/active-relays/") {
                return TqJsonResponse(500, "{\"bad_active_relays_empty\":true}");
            }
            if (req.Method == "GET" && req.Path == "/relay/active-relays/relay-1/extra") {
                return TqJsonResponse(500, "{\"bad_active_relays_nested\":true}");
            }
            if (req.Method == "GET" && req.Path == "/relay/workers/aggregate") {
                return TqJsonResponse(200, "{\"worker_id\":\"aggregate\"}");
            }
            if (req.Method == "GET" && req.Path == "/relay/workers/") {
                return TqJsonResponse(500, "{\"bad_worker_empty\":true}");
            }
            if (req.Method == "GET" && req.Path == "/relay/workers/a/b") {
                return TqJsonResponse(500, "{\"bad_worker_nested\":true}");
            }
            if (req.Method == "GET" && req.Path == "/server/connections") {
                return TqJsonResponse(200, "{\"connections\":[]}");
            }
            if (req.Method == "POST" && req.Path == "/server/connections/srv-1:abort-tunnels") {
                return TqJsonResponse(202, "{\"server_abort\":true}");
            }
            if (req.Method == "GET" && req.Path == "/server/tunnels") {
                return TqJsonResponse(200, "{\"server_tunnels\":[]}");
            }
            if (req.Method == "GET" && req.Path == "/server/tunnels/tun-5") {
                return TqJsonResponse(200, "{\"server_tunnel\":\"tun-5\"}");
            }
            if (req.Method == "POST" && req.Path == "/server/tunnels/tun-5:abort") {
                return TqJsonResponse(202, "{\"server_tunnel_abort\":true}");
            }
            if (req.Method == "POST" && req.Path == "/server/tunnels/tun-5:drain") {
                return TqJsonResponse(202, "{\"server_tunnel_drain\":true}");
            }
            if (req.Method == "GET" && req.Path == "/server/tunnels/") {
                return TqJsonResponse(500, "{\"bad_server_tunnel_empty\":true}");
            }
            if (req.Method == "GET" && req.Path == "/server/tunnels/tun-5/extra") {
                return TqJsonResponse(500, "{\"bad_server_tunnel_nested\":true}");
            }
            if (req.Method == "POST" && req.Path == "/server/tunnels/tun-5:unknown") {
                return TqJsonResponse(500, "{\"bad_server_tunnel_action\":true}");
            }
            if (req.Method == "GET" && req.Path == "/server/tunnels/tun%2f5") {
                return TqJsonResponse(500, "{\"bad_server_tunnel_encoded_slash\":true}");
            }
            if (req.Method == "POST" && req.Path == "/memory/allocator:dump") {
                return TqJsonResponse(200, "{\"memory_dump\":true}");
            }
            if (req.Method == "GET" && req.Path == "/runtime/config") {
                return TqJsonResponse(200, "{\"runtime_config\":true}");
            }
            if (req.Method == "GET" && req.Path == "/client/config") {
                return TqJsonResponse(200, "{\"client_config\":true}");
            }
            if (req.Method == "GET" && req.Path == "/diagnostics") {
                return TqJsonResponse(200, "{\"diagnostics\":true}");
            }
            if (req.Method == "GET" && req.Path == "/server/config") {
                return TqJsonResponse(200, "{\"server_config\":true}");
            }
            if (req.Method == "GET" && req.Path == "/peers/agent-d/config") {
                return TqJsonResponse(200, "{\"peer_config\":true}");
            }
            if (req.Method == "GET" && req.Path == "/peers/agent-d/config/extra") {
                return TqJsonResponse(500, "{\"bad_forward\":true}");
            }
            return TqJsonResponse(404, "{}");
        }, options);
        if (!server.Start(err)) return 120;
        const uint16_t port = TqPortFromListenAddress(server.ListenAddress());
        if (port == 0) return 121;

        auto sendAuthorized = [&](const std::string& method, const std::string& path, std::string& out) -> int {
            TqSocketHandle requestFd = TqConnectLocal(port);
            if (!TqSocketValid(requestFd)) return 180;
            const std::string requestText = method + " " + path + " HTTP/1.1\r\nHost: 127.0.0.1\r\nAuthorization: Bearer " +
                server.AuthTokenForTesting() + "\r\n\r\n";
            if (!TqSendAll(requestFd, requestText)) {
                TqCloseSocket(requestFd);
                return 181;
            }
            if (!TqRecvUntilClosed(requestFd, out)) {
                TqCloseSocket(requestFd);
                return 182;
            }
            TqCloseSocket(requestFd);
            return 0;
        };

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
        if (!TqHttpStatusIs(wrongConnectionResponse, 404)) return 140;

        TqSocketHandle tunnelsFd = TqConnectLocal(port);
        if (!TqSocketValid(tunnelsFd)) return 141;
        const std::string tunnelsRequest =
            "GET /api/v1/tunnels HTTP/1.1\r\nHost: 127.0.0.1\r\nAuthorization: Bearer " +
            server.AuthTokenForTesting() + "\r\n\r\n";
        if (!TqSendAll(tunnelsFd, tunnelsRequest)) return 142;
        std::string tunnelsResponse;
        if (!TqRecvUntilClosed(tunnelsFd, tunnelsResponse)) return 143;
        TqCloseSocket(tunnelsFd);
        if (!TqHttpStatusIs(tunnelsResponse, 200)) return 144;

        TqSocketHandle tunnelAbortFd = TqConnectLocal(port);
        if (!TqSocketValid(tunnelAbortFd)) return 145;
        const std::string tunnelAbortRequest =
            "POST /api/v1/tunnels/tun-1:abort HTTP/1.1\r\nHost: 127.0.0.1\r\nAuthorization: Bearer " +
            server.AuthTokenForTesting() + "\r\nContent-Length: 0\r\n\r\n";
        if (!TqSendAll(tunnelAbortFd, tunnelAbortRequest)) return 146;
        std::string tunnelAbortResponse;
        if (!TqRecvUntilClosed(tunnelAbortFd, tunnelAbortResponse)) return 147;
        TqCloseSocket(tunnelAbortFd);
        if (!TqHttpStatusIs(tunnelAbortResponse, 202)) return 148;

        std::string activeRelaysResponse;
        if (const int code = sendAuthorized("GET", "/api/v1/relay/active-relays", activeRelaysResponse)) return code;
        if (!TqHttpStatusIs(activeRelaysResponse, 200)) return 195;
        if (activeRelaysResponse.find("\"active_relays\":true") == std::string::npos) return 196;

        std::string activeRelayResponse;
        if (const int code = sendAuthorized("GET", "/api/v1/relay/active-relays/relay-1", activeRelayResponse)) return code;
        if (!TqHttpStatusIs(activeRelayResponse, 200)) return 197;
        if (activeRelayResponse.find("\"relay_id\":\"relay-1\"") == std::string::npos) return 198;

        std::string activeRelaysEmptyResponse;
        if (const int code = sendAuthorized("GET", "/api/v1/relay/active-relays/", activeRelaysEmptyResponse)) return code;
        if (!TqHttpStatusIs(activeRelaysEmptyResponse, 404)) return 199;
        if (activeRelaysEmptyResponse.find("bad_active_relays_empty") != std::string::npos) return 200;

        std::string activeRelaysNestedResponse;
        if (const int code = sendAuthorized("GET", "/api/v1/relay/active-relays/relay-1/extra", activeRelaysNestedResponse)) return code;
        if (!TqHttpStatusIs(activeRelaysNestedResponse, 404)) return 201;
        if (activeRelaysNestedResponse.find("bad_active_relays_nested") != std::string::npos) return 202;

        TqSocketHandle relayWorkerFd = TqConnectLocal(port);
        if (!TqSocketValid(relayWorkerFd)) return 149;
        const std::string relayWorkerRequest =
            "GET /api/v1/relay/workers/aggregate HTTP/1.1\r\nHost: 127.0.0.1\r\nAuthorization: Bearer " +
            server.AuthTokenForTesting() + "\r\n\r\n";
        if (!TqSendAll(relayWorkerFd, relayWorkerRequest)) return 150;
        std::string relayWorkerResponse;
        if (!TqRecvUntilClosed(relayWorkerFd, relayWorkerResponse)) return 151;
        TqCloseSocket(relayWorkerFd);
        if (!TqHttpStatusIs(relayWorkerResponse, 200)) return 152;

        std::string relayWorkerAggregateResponse;
        if (const int code = sendAuthorized("GET", "/api/v1/relay/workers/aggregate", relayWorkerAggregateResponse)) return code;
        if (!TqHttpStatusIs(relayWorkerAggregateResponse, 200)) return 203;
        if (relayWorkerAggregateResponse.find("\"worker_id\":\"aggregate\"") == std::string::npos) return 204;

        std::string relayWorkerEmptyResponse;
        if (const int code = sendAuthorized("GET", "/api/v1/relay/workers/", relayWorkerEmptyResponse)) return code;
        if (!TqHttpStatusIs(relayWorkerEmptyResponse, 404)) return 205;
        if (relayWorkerEmptyResponse.find("bad_worker_empty") != std::string::npos) return 206;

        std::string relayWorkerNestedResponse;
        if (const int code = sendAuthorized("GET", "/api/v1/relay/workers/a/b", relayWorkerNestedResponse)) return code;
        if (!TqHttpStatusIs(relayWorkerNestedResponse, 404)) return 207;
        if (relayWorkerNestedResponse.find("bad_worker_nested") != std::string::npos) return 208;

        TqSocketHandle serverConnectionsFd = TqConnectLocal(port);
        if (!TqSocketValid(serverConnectionsFd)) return 153;
        const std::string serverConnectionsRequest =
            "GET /api/v1/server/connections HTTP/1.1\r\nHost: 127.0.0.1\r\nAuthorization: Bearer " +
            server.AuthTokenForTesting() + "\r\n\r\n";
        if (!TqSendAll(serverConnectionsFd, serverConnectionsRequest)) return 154;
        std::string serverConnectionsResponse;
        if (!TqRecvUntilClosed(serverConnectionsFd, serverConnectionsResponse)) return 155;
        TqCloseSocket(serverConnectionsFd);
        if (!TqHttpStatusIs(serverConnectionsResponse, 200)) return 156;

        TqSocketHandle serverAbortFd = TqConnectLocal(port);
        if (!TqSocketValid(serverAbortFd)) return 157;
        const std::string serverAbortRequest =
            "POST /api/v1/server/connections/srv-1:abort-tunnels HTTP/1.1\r\nHost: 127.0.0.1\r\nAuthorization: Bearer " +
            server.AuthTokenForTesting() + "\r\nContent-Length: 0\r\n\r\n";
        if (!TqSendAll(serverAbortFd, serverAbortRequest)) return 158;
        std::string serverAbortResponse;
        if (!TqRecvUntilClosed(serverAbortFd, serverAbortResponse)) return 159;
        TqCloseSocket(serverAbortFd);
        if (!TqHttpStatusIs(serverAbortResponse, 202)) return 160;

        std::string serverTunnelsResponse;
        if (const int code = sendAuthorized("GET", "/api/v1/server/tunnels", serverTunnelsResponse)) return code;
        if (!TqHttpStatusIs(serverTunnelsResponse, 200)) return 219;
        if (serverTunnelsResponse.find("\"server_tunnels\":[]") == std::string::npos) return 220;

        std::string serverTunnelResponse;
        if (const int code = sendAuthorized("GET", "/api/v1/server/tunnels/tun-5", serverTunnelResponse)) return code;
        if (!TqHttpStatusIs(serverTunnelResponse, 200)) return 209;
        if (serverTunnelResponse.find("\"server_tunnel\":\"tun-5\"") == std::string::npos) return 210;

        std::string serverTunnelAbortResponse;
        if (const int code = sendAuthorized("POST", "/api/v1/server/tunnels/tun-5:abort", serverTunnelAbortResponse)) return code;
        if (!TqHttpStatusIs(serverTunnelAbortResponse, 202)) return 211;
        if (serverTunnelAbortResponse.find("\"server_tunnel_abort\":true") == std::string::npos) return 212;

        std::string serverTunnelDrainResponse;
        if (const int code = sendAuthorized("POST", "/api/v1/server/tunnels/tun-5:drain", serverTunnelDrainResponse)) return code;
        if (!TqHttpStatusIs(serverTunnelDrainResponse, 202)) return 213;
        if (serverTunnelDrainResponse.find("\"server_tunnel_drain\":true") == std::string::npos) return 214;

        std::string serverTunnelEmptyResponse;
        if (const int code = sendAuthorized("GET", "/api/v1/server/tunnels/", serverTunnelEmptyResponse)) return code;
        if (!TqHttpStatusIs(serverTunnelEmptyResponse, 404)) return 215;
        if (serverTunnelEmptyResponse.find("bad_server_tunnel_empty") != std::string::npos) return 216;

        std::string serverTunnelNestedResponse;
        if (const int code = sendAuthorized("GET", "/api/v1/server/tunnels/tun-5/extra", serverTunnelNestedResponse)) return code;
        if (!TqHttpStatusIs(serverTunnelNestedResponse, 404)) return 217;
        if (serverTunnelNestedResponse.find("bad_server_tunnel_nested") != std::string::npos) return 218;

        std::string serverTunnelUnknownActionResponse;
        if (const int code = sendAuthorized("POST", "/api/v1/server/tunnels/tun-5:unknown", serverTunnelUnknownActionResponse)) return code;
        if (!TqHttpStatusIs(serverTunnelUnknownActionResponse, 404)) return 221;
        if (serverTunnelUnknownActionResponse.find("bad_server_tunnel_action") != std::string::npos) return 222;

        std::string serverTunnelEncodedSlashResponse;
        if (const int code = sendAuthorized("GET", "/api/v1/server/tunnels/tun%2f5", serverTunnelEncodedSlashResponse)) return code;
        if (!TqHttpStatusIs(serverTunnelEncodedSlashResponse, 404)) return 223;
        if (serverTunnelEncodedSlashResponse.find("bad_server_tunnel_encoded_slash") != std::string::npos) return 224;

        TqSocketHandle memoryDumpFd = TqConnectLocal(port);
        if (!TqSocketValid(memoryDumpFd)) return 161;
        const std::string memoryDumpRequest =
            "POST /api/v1/memory/allocator:dump HTTP/1.1\r\nHost: 127.0.0.1\r\nAuthorization: Bearer " +
            server.AuthTokenForTesting() + "\r\nContent-Length: 0\r\n\r\n";
        if (!TqSendAll(memoryDumpFd, memoryDumpRequest)) return 162;
        std::string memoryDumpResponse;
        if (!TqRecvUntilClosed(memoryDumpFd, memoryDumpResponse)) return 163;
        TqCloseSocket(memoryDumpFd);

        std::string runtimeConfigResponse;
        if (const int code = sendAuthorized("GET", "/api/v1/runtime/config", runtimeConfigResponse)) return code;
        if (!TqHttpStatusIs(runtimeConfigResponse, 200)) return 183;
        if (runtimeConfigResponse.find("\"runtime_config\":true") == std::string::npos) return 184;

        std::string clientConfigResponse;
        if (const int code = sendAuthorized("GET", "/api/v1/client/config", clientConfigResponse)) return code;
        if (!TqHttpStatusIs(clientConfigResponse, 200)) return 185;
        if (clientConfigResponse.find("\"client_config\":true") == std::string::npos) return 186;

        std::string diagnosticsResponse;
        if (const int code = sendAuthorized("GET", "/api/v1/diagnostics", diagnosticsResponse)) return code;
        if (!TqHttpStatusIs(diagnosticsResponse, 200)) return 187;
        if (diagnosticsResponse.find("\"diagnostics\":true") == std::string::npos) return 188;

        std::string serverConfigResponse;
        if (const int code = sendAuthorized("GET", "/api/v1/server/config", serverConfigResponse)) return code;
        if (!TqHttpStatusIs(serverConfigResponse, 200)) return 189;
        if (serverConfigResponse.find("\"server_config\":true") == std::string::npos) return 190;

        std::string peerConfigResponse;
        if (const int code = sendAuthorized("GET", "/api/v1/peers/agent-d/config", peerConfigResponse)) return code;
        if (!TqHttpStatusIs(peerConfigResponse, 200)) return 191;
        if (peerConfigResponse.find("\"peer_config\":true") == std::string::npos) return 192;

        std::string badPeerConfigResponse;
        if (const int code = sendAuthorized("GET", "/api/v1/peers/agent-d/config/extra", badPeerConfigResponse)) return code;
        server.Stop();
        if (!TqHttpStatusIs(badPeerConfigResponse, 404)) return 193;
        if (badPeerConfigResponse.find("bad_forward") != std::string::npos) return 194;
        if (!TqHttpStatusIs(memoryDumpResponse, 200)) return 164;
        if (memoryDumpResponse.find("\"memory_dump\":true") == std::string::npos) return 165;
    }
    {
        TqAdminHttpServerOptions options;
        options.TokenFile = TqSecureTokenFile("v1-only").string();
        std::string err;
        TqAdminHttpServer server("127.0.0.1:0", [&](const TqHttpRequest& req) {
            if (req.Method == "GET" && req.Path == "/health") {
                return TqJsonResponse(200, "{\"ok\":true}");
            }
            if (req.Method == "POST" && req.Path == "/peers/agent-d/enable") {
                return TqJsonResponse(200, "{\"legacy_enable\":true}");
            }
            return TqJsonResponse(404, "{}");
        }, options);
        if (!server.Start(err)) return 105;
        const uint16_t port = TqPortFromListenAddress(server.ListenAddress());
        if (port == 0) return 106;

        TqSocketHandle healthFd = TqConnectLocal(port);
        if (!TqSocketValid(healthFd)) return 107;
        if (!TqSendAll(healthFd, "GET /health HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n")) return 108;
        std::string healthResponse;
        if (!TqRecvUntilClosed(healthFd, healthResponse)) return 109;
        TqCloseSocket(healthFd);
        if (!TqHttpStatusIs(healthResponse, 404)) return 110;

        TqSocketHandle peerFd = TqConnectLocal(port);
        if (!TqSocketValid(peerFd)) return 111;
        if (!TqSendAll(peerFd, "POST /peers/agent-d/enable HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Length: 0\r\n\r\n")) return 112;
        std::string peerResponse;
        if (!TqRecvUntilClosed(peerFd, peerResponse)) return 113;
        TqCloseSocket(peerFd);
        server.Stop();
        if (!TqHttpStatusIs(peerResponse, 404)) return 114;
    }
    return 0;
}
