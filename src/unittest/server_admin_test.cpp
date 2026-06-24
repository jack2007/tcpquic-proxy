#include "server_admin.h"
#include "quic_session.h"
#include "tunnel_registry.h"

#include <string>
#include <sstream>
#include <vector>

namespace {

bool g_abortCalled = false;

TqHttpRequest Request(const std::string& method, const std::string& path) {
    TqHttpRequest req;
    req.Method = method;
    req.Path = path;
    return req;
}

} // namespace

void TqAppendJsonString(std::ostringstream& out, const char* name, const std::string& value);

void TqAppendJsonString(std::ostringstream& out, const char* name, const std::string& value) {
    out << '"' << name << "\":\"" << value << '"';
}

std::string TqJsonResponse(int status, const std::string& json) {
    const char* reason = status == 200 ? "OK" : (status == 202 ? "Accepted" : "Not Found");
    return "HTTP/1.1 " + std::to_string(status) + " " + reason + "\r\n\r\n" + json;
}

std::string TqServerMetricsJson(const TqServerMetrics&, uint64_t) {
    return "{\"role\":\"server\"}";
}

std::vector<TqServerConnectionSnapshot> TqSnapshotServerConnections() {
    TqServerConnectionSnapshot connection;
    connection.ConnectionId = "srv-7";
    connection.RemoteAddress = "127.0.0.1:5000";
    connection.State = "connected";
    connection.ActiveStreams = 1;
    connection.TotalStreams = 3;
    connection.ActiveTunnels = 2;
    return {connection};
}

bool TqGetServerConnectionSnapshot(const std::string& connectionId, TqServerConnectionSnapshot& out) {
    if (connectionId != "srv-7") {
        return false;
    }
    out = TqSnapshotServerConnections()[0];
    return true;
}

bool TqAbortServerConnectionTunnels(const std::string& connectionId) {
    if (connectionId != "srv-7") {
        return false;
    }
    g_abortCalled = true;
    return true;
}

std::vector<TqTunnelSnapshot> TqSnapshotTunnels() {
    TqTunnelSnapshot tunnel;
    tunnel.TunnelId = "tun-5";
    tunnel.ConnectionId = "srv-7";
    tunnel.Role = "server";
    tunnel.State = "active";
    tunnel.Target = "example.com:443";
    return {tunnel};
}

int main() {
    TqServerMetrics metrics;
    metrics.Listen = "127.0.0.1:1443";

    std::string server = TqHandleServerAdmin(Request("GET", "/server"), metrics, 10);
    if (server.find("HTTP/1.1 200 OK") == std::string::npos) return 1;
    if (server.find("\"role\":\"server\"") == std::string::npos) return 2;

    std::string list = TqHandleServerAdmin(Request("GET", "/server/connections"), metrics, 10);
    if (list.find("HTTP/1.1 200 OK") == std::string::npos) return 3;
    if (list.find("\"connection_id\":\"srv-7\"") == std::string::npos) return 4;

    std::string get = TqHandleServerAdmin(Request("GET", "/server/connections/srv-7"), metrics, 10);
    if (get.find("\"remote_address\":\"127.0.0.1:5000\"") == std::string::npos) return 5;

    std::string abort = TqHandleServerAdmin(Request("POST", "/server/connections/srv-7:abort-tunnels"), metrics, 10);
    if (abort.find("HTTP/1.1 202 Accepted") == std::string::npos) return 6;
    if (!g_abortCalled) return 7;

    std::string tunnels = TqHandleServerAdmin(Request("GET", "/server/tunnels"), metrics, 10);
    if (tunnels.find("HTTP/1.1 200 OK") == std::string::npos) return 8;
    if (tunnels.find("\"tunnel_id\":\"tun-5\"") == std::string::npos) return 9;

    std::string missing = TqHandleServerAdmin(Request("GET", "/server/connections/srv-404"), metrics, 10);
    if (missing.find("HTTP/1.1 404 Not Found") == std::string::npos) return 10;
    return 0;
}
