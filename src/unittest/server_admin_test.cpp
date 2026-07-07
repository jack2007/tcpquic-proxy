#include "server_admin.h"
#include "runtime_config_file_store.h"
#include "server_runtime_config.h"
#include "quic_session.h"
#include "relay_metrics.h"
#include "tunnel_registry.h"

#include <filesystem>
#include <fstream>
#include <string>
#include <sstream>
#include <vector>

bool g_trace_stub_enabled = false;
bool g_diag_stats_stub_enabled = false;
unsigned g_trace_init_count = 0;
unsigned g_trace_shutdown_count = 0;
unsigned g_diag_stats_init_count = 0;
unsigned g_diag_stats_shutdown_count = 0;
uint32_t g_trace_stub_interval_sec = 0;
uint32_t g_diag_stats_stub_interval_sec = 0;
TqConfig::TraceLevel g_trace_stub_level = TqConfig::TraceLevel::Info;

void TqTraceProxyStubReset() {
    g_trace_stub_enabled = false;
    g_diag_stats_stub_enabled = false;
    g_trace_init_count = 0;
    g_trace_shutdown_count = 0;
    g_diag_stats_init_count = 0;
    g_diag_stats_shutdown_count = 0;
    g_trace_stub_interval_sec = 0;
    g_diag_stats_stub_interval_sec = 0;
    g_trace_stub_level = TqConfig::TraceLevel::Info;
}

bool TqTraceInit(TqMode, uint32_t intervalSec, TqConfig::TraceLevel level) {
    g_trace_stub_enabled = true;
    g_trace_stub_interval_sec = intervalSec;
    g_trace_stub_level = level;
    ++g_trace_init_count;
    return true;
}

void TqTraceShutdown() {
    g_trace_stub_enabled = false;
    ++g_trace_shutdown_count;
}

bool TqTraceEnabled() {
    return g_trace_stub_enabled;
}

bool TqDiagStatsInit(uint32_t intervalSec) {
    g_diag_stats_stub_enabled = true;
    g_diag_stats_stub_interval_sec = intervalSec;
    ++g_diag_stats_init_count;
    return true;
}

void TqDiagStatsShutdown() {
    g_diag_stats_stub_enabled = false;
    ++g_diag_stats_shutdown_count;
}

bool TqDiagStatsEnabled() {
    return g_diag_stats_stub_enabled;
}

bool TqApplyDiagnosticsRuntime(const TqConfig& cfg) {
    if (cfg.Trace) {
        if (TqTraceEnabled()) {
            TqTraceShutdown();
        }
        if (!TqTraceInit(cfg.Mode, cfg.TraceIntervalSec, cfg.TraceLogLevel)) {
            return false;
        }
    } else if (TqTraceEnabled()) {
        TqTraceShutdown();
    }
    if (cfg.DiagStats) {
        if (!TqDiagStatsInit(cfg.DiagStatsIntervalSec)) {
            return false;
        }
    } else if (TqDiagStatsEnabled()) {
        TqDiagStatsShutdown();
    }
    return true;
}

namespace {

bool g_abortCalled = false;
bool g_tunnelAbortCalled = false;
bool g_tunnelDrainCalled = false;

std::string TempServerConfigPath(const std::string& name) {
    return (std::filesystem::temp_directory_path() / (name + ".json")).string();
}

TqHttpRequest Request(const std::string& method, const std::string& path, const std::string& body = "") {
    TqHttpRequest req;
    req.Method = method;
    req.Path = path;
    req.Body = body;
    return req;
}

bool WriteText(const std::string& path, const std::string& body) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << body;
    return static_cast<bool>(out);
}

std::string ReadText(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

} // namespace

std::string TqJsonResponse(int status, const std::string& json) {
    const char* reason = status == 200 ? "OK" :
        (status == 202 ? "Accepted" :
        (status == 503 ? "Service Unavailable" : "Not Found"));
    return "HTTP/1.1 " + std::to_string(status) + " " + reason + "\r\n\r\n" + json;
}

std::string TqServerMetricsJson(const TqServerMetrics&, uint64_t) {
    return "{\"role\":\"server\"}";
}

TqRelayMetricsSnapshot TqSnapshotRelayMetrics() {
    TqRelayMetricsSnapshot metrics;
    metrics.Backend = "test";
    metrics.ActiveRelays = 7;
    metrics.PendingBytes = 1234;
    metrics.TcpReadBytes = 11;
    metrics.TcpWriteBytes = 22;
    metrics.Errors = 3;
    return metrics;
}

std::vector<TqRelayActiveSnapshot> TqSnapshotActiveRelays() {
    return {};
}

std::string TqRelayActiveRelaysJson() {
    return "{\"capabilities\":{\"detail\":false},\"relays\":[]}";
}

std::string TqRelayActiveRelayJson(const std::string&, bool& found, bool& supported) {
    found = false;
    supported = false;
    return "{}";
}

std::string TqRelayWorkerDetailJson(const std::string& workerId, bool& found, bool& supported) {
    supported = workerId == "aggregate";
    found = supported;
    return "{\"worker_id\":\"aggregate\",\"worker_index\":0,\"errors\":3,\"relays\":[]}";
}

std::string TqRelayWorkersJson() {
    return "{\"capabilities\":{\"active_relay_detail\":false,\"worker_detail\":true,\"per_worker_active_relays\":false},\"workers\":[{\"worker_id\":\"aggregate\",\"backend\":\"test\",\"worker_index\":0,\"active_relays\":7,\"pending_bytes\":1234,\"tcp_read_bytes\":11,\"tcp_write_bytes\":22,\"errors\":3}]}";
}

std::string TqRelayMetricsFieldsJson(const TqRelayMetricsSnapshot&) {
    return "{\"relay_backend\":\"test\",\"linux_relay_backend\":\"test\",\"relay_active_relays\":7,\"linux_relay_active_relays\":7,\"relay_errors\":3,\"linux_relay_errors\":3}";
}

std::vector<TqServerConnectionSnapshot> TqSnapshotServerConnections() {
    TqServerConnectionSnapshot connection;
    connection.ConnectionId = "srv-7";
    connection.ClientName = "office-a";
    connection.RemoteAddress = "127.0.0.1:5000";
    connection.State = "connected";
    connection.ActiveStreams = 1;
    connection.TotalStreams = 3;
    connection.ActiveTunnels = 2;
    connection.Encryption = "disabled";
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

bool TqGetTunnelSnapshot(const std::string& tunnelId, TqTunnelSnapshot& out) {
    if (tunnelId == "client-tun") {
        out = TqSnapshotTunnels()[0];
        out.TunnelId = "client-tun";
        out.Role = "client";
        return true;
    }
    if (tunnelId != "tun-5") {
        return false;
    }
    out = TqSnapshotTunnels()[0];
    return true;
}

bool TqAbortTunnelById(const std::string& tunnelId) {
    if (tunnelId != "tun-5") {
        return false;
    }
    g_tunnelAbortCalled = true;
    return true;
}

bool TqDrainTunnelById(const std::string& tunnelId) {
    if (tunnelId != "tun-5") {
        return false;
    }
    g_tunnelDrainCalled = true;
    return true;
}

int TestServerRuntimeConfigStateBuildsAclPatch() {
    TqConfig cfg;
    cfg.Mode = TqMode::Server;
    cfg.ConfigPath = "server.json";
    cfg.QuicListen = "0.0.0.0:4433";
    cfg.AllowTargets = {"10.0.0.0/8"};
    cfg.DenyTargets = {};

    TqServerRuntimeConfigState state(cfg);
    TqServerConfigPatch patch;
    patch.HasAllowTargets = true;
    patch.AllowTargets = {"127.0.0.1/32"};
    patch.HasDenyTargets = true;
    patch.DenyTargets = {"169.254.0.0/16"};

    TqConfig next;
    TqAcl acl;
    std::string err;
    if (!state.BuildAclPatch(patch, next, acl, err)) return 500;
    if (next.AllowTargets.size() != 1 || next.AllowTargets[0] != "127.0.0.1/32") return 501;
    if (next.DenyTargets.size() != 1 || next.DenyTargets[0] != "169.254.0.0/16") return 502;
    if (!acl.IsAllowed("127.0.0.1", 80)) return 503;
    if (acl.IsAllowed("169.254.1.1", 80)) return 504;

    state.Commit(next, acl);
    const TqConfig snapshot = state.SnapshotConfig();
    if (snapshot.AllowTargets != next.AllowTargets) return 505;
    if (snapshot.DenyTargets != next.DenyTargets) return 506;
    return 0;
}

int TestServerRuntimeConfigStateRejectsInvalidCidrWithoutCommit() {
    TqConfig cfg;
    cfg.Mode = TqMode::Server;
    cfg.AllowTargets = {"10.0.0.0/8"};

    TqServerRuntimeConfigState state(cfg);
    TqServerConfigPatch patch;
    patch.HasAllowTargets = true;
    patch.AllowTargets = {"bad-cidr"};

    TqConfig next;
    TqAcl acl;
    std::string err;
    if (state.BuildAclPatch(patch, next, acl, err)) return 510;
    if (err.find("invalid CIDR") == std::string::npos) return 511;
    if (state.SnapshotConfig().AllowTargets != cfg.AllowTargets) return 512;
    if (!state.SnapshotAcl().IsAllowed("10.0.0.1", 80)) return 513;
    return 0;
}

int TestParseServerAclPatchAcceptsCommaSeparatedStrings() {
    TqServerConfigPatch patch;
    std::string err;
    bool unsupported = false;
    if (!TqParseServerConfigPatch(
            "{\"allow_targets\":\"127.0.0.1/32, 10.0.0.0/8\",\"deny_targets\":\"169.254.0.0/16\"}",
            patch,
            err,
            unsupported)) return 520;
    if (unsupported) return 521;
    if (!patch.HasAllowTargets || patch.AllowTargets.size() != 2) return 522;
    if (patch.AllowTargets[0] != "127.0.0.1/32" || patch.AllowTargets[1] != "10.0.0.0/8") return 523;
    if (!patch.HasDenyTargets || patch.DenyTargets.size() != 1 || patch.DenyTargets[0] != "169.254.0.0/16") return 524;
    return 0;
}

int TestParseServerAclPatchRejectsUnknownField() {
    TqServerConfigPatch patch;
    std::string err;
    bool unsupported = false;
    if (TqParseServerConfigPatch("{\"allow_targets\":[],\"extra\":true}", patch, err, unsupported)) return 530;
    if (unsupported) return 531;
    if (err.find("unknown") == std::string::npos) return 532;
    return 0;
}

int TestParseServerAclPatchReportsStartupFieldUnsupported() {
    TqServerConfigPatch patch;
    std::string err;
    bool unsupported = false;
    if (TqParseServerConfigPatch("{\"tls\":{\"cert\":\"server.crt\"}}", patch, err, unsupported)) return 540;
    if (!unsupported) return 541;
    if (err.find("requires process restart") == std::string::npos &&
        err.find("not supported") == std::string::npos) return 542;
    return 0;
}

int TestServerRuntimeConfigStateKeepsUnpatchedAclSide() {
    TqConfig cfg;
    cfg.Mode = TqMode::Server;
    cfg.AllowTargets = {"10.0.0.0/8"};
    cfg.DenyTargets = {"169.254.0.0/16"};

    TqServerRuntimeConfigState state(cfg);
    TqServerConfigPatch patch;
    patch.HasAllowTargets = true;
    patch.AllowTargets = {"127.0.0.1/32"};

    TqConfig next;
    TqAcl acl;
    std::string err;
    if (!state.BuildAclPatch(patch, next, acl, err)) return 550;
    if (next.AllowTargets != patch.AllowTargets) return 551;
    if (next.DenyTargets != cfg.DenyTargets) return 552;
    if (acl.IsAllowed("169.254.1.1", 80)) return 553;
    return 0;
}

int TestServerRuntimeConfigStateAllowsEmptyAllowList() {
    TqConfig cfg;
    cfg.Mode = TqMode::Server;
    cfg.AllowTargets = {"10.0.0.0/8"};
    cfg.DenyTargets = {};

    TqServerRuntimeConfigState state(cfg);
    TqServerConfigPatch patch;
    patch.HasAllowTargets = true;
    patch.AllowTargets = {};

    TqConfig next;
    TqAcl acl;
    std::string err;
    if (!state.BuildAclPatch(patch, next, acl, err)) return 560;
    if (!next.AllowTargets.empty()) return 561;
    if (acl.IsAllowed("10.0.0.1", 80)) return 562;
    return 0;
}

int TestServerAdminPatchServerConfigWritesUpdatesAndCommits() {
    const std::string path = TempServerConfigPath("tcpquic-server-admin-success");
    std::filesystem::remove(path);
    if (!WriteText(path, "{\"server\":{\"allow_targets\":[\"10.0.0.0/8\"],\"deny_targets\":[]}}\n")) return 570;

    TqConfig cfg;
    cfg.Mode = TqMode::Server;
    cfg.ConfigPath = path;
    cfg.QuicListen = "0.0.0.0:4433";
    cfg.AllowTargets = {"10.0.0.0/8"};
    cfg.DenyTargets = {};
    TqServerRuntimeConfigState state(cfg);
    TqRuntimeConfigFileStore store(path);
    int updateCalls = 0;
    TqAcl appliedAcl;
    TqServerMetrics metrics;

    const std::string response = TqHandleServerAdmin(
        Request("PATCH", "/server/config", "{\"allow_targets\":[\"127.0.0.1/32\"],\"deny_targets\":[\"169.254.0.0/16\"]}"),
        metrics,
        10,
        state,
        &store,
        [&](const TqAcl& acl) {
            ++updateCalls;
            appliedAcl = acl;
            return true;
        });

    if (response.find("HTTP/1.1 200 OK") == std::string::npos) return 571;
    if (response.find("\"allow_targets\":[\"127.0.0.1/32\"]") == std::string::npos) return 572;
    if (updateCalls != 1) return 573;
    if (!appliedAcl.IsAllowed("127.0.0.1", 80)) return 574;
    if (appliedAcl.IsAllowed("169.254.1.1", 80)) return 575;
    const TqConfig snapshot = state.SnapshotConfig();
    if (snapshot.AllowTargets != std::vector<std::string>{"127.0.0.1/32"}) return 576;
    if (snapshot.DenyTargets != std::vector<std::string>{"169.254.0.0/16"}) return 577;
    const std::string fileBody = ReadText(path);
    if (fileBody.find("\"allow_targets\": [\n      \"127.0.0.1/32\"") == std::string::npos) return 578;
    if (fileBody.find("\"deny_targets\": [\n      \"169.254.0.0/16\"") == std::string::npos) return 579;
    std::filesystem::remove(path);
    return 0;
}

int TestServerAdminGetServerConfigUsesStateSnapshot() {
    TqConfig cfg;
    cfg.Mode = TqMode::Server;
    cfg.ConfigPath = "server.json";
    cfg.QuicListen = "127.0.0.1:4433";
    cfg.AllowTargets = {"10.0.0.0/8"};
    cfg.DenyTargets = {};
    TqServerRuntimeConfigState state(cfg);

    TqServerConfigPatch patch;
    patch.HasAllowTargets = true;
    patch.AllowTargets = {"127.0.0.1/32"};
    TqConfig nextConfig;
    TqAcl nextAcl;
    std::string err;
    if (!state.BuildAclPatch(patch, nextConfig, nextAcl, err)) return 625;
    state.Commit(nextConfig, nextAcl);

    TqServerMetrics metrics;
    metrics.ResolvedListens = {"127.0.0.1:4433"};
    const std::string response = TqHandleServerAdmin(
        Request("GET", "/server/config"),
        metrics,
        10,
        state,
        nullptr,
        TqServerAdminUpdateAcl{});

    if (response.find("HTTP/1.1 200 OK") == std::string::npos) return 626;
    if (response.find("\"allow_targets\":[\"127.0.0.1/32\"]") == std::string::npos) return 627;
    if (response.find("\"listen\":\"127.0.0.1:4433\"") == std::string::npos) return 628;
    return 0;
}

int TestServerAdminPatchServerConfigRejectsInvalidCidrWithoutSideEffects() {
    const std::string path = TempServerConfigPath("tcpquic-server-admin-invalid-cidr");
    std::filesystem::remove(path);
    if (!WriteText(path, "{\"server\":{\"allow_targets\":[\"10.0.0.0/8\"],\"deny_targets\":[]}}\n")) return 580;

    TqConfig cfg;
    cfg.Mode = TqMode::Server;
    cfg.ConfigPath = path;
    cfg.AllowTargets = {"10.0.0.0/8"};
    TqServerRuntimeConfigState state(cfg);
    TqRuntimeConfigFileStore store(path);
    int updateCalls = 0;
    TqServerMetrics metrics;
    const std::string before = ReadText(path);

    const std::string response = TqHandleServerAdmin(
        Request("PATCH", "/server/config", "{\"allow_targets\":[\"bad-cidr\"]}"),
        metrics,
        10,
        state,
        &store,
        [&](const TqAcl&) {
            ++updateCalls;
            return true;
        });

    if (response.find("HTTP/1.1 400") == std::string::npos) return 581;
    if (updateCalls != 0) return 582;
    if (ReadText(path) != before) return 583;
    if (state.SnapshotConfig().AllowTargets != cfg.AllowTargets) return 584;
    std::filesystem::remove(path);
    return 0;
}

int TestServerAdminPatchServerConfigWithoutConfigPathReturnsNotSupported() {
    TqConfig cfg;
    cfg.Mode = TqMode::Server;
    cfg.AllowTargets = {"10.0.0.0/8"};
    TqServerRuntimeConfigState state(cfg);
    TqRuntimeConfigFileStore store("/tmp/tcpquic-server-admin-unused.json");
    int updateCalls = 0;
    TqServerMetrics metrics;

    const std::string response = TqHandleServerAdmin(
        Request("PATCH", "/server/config", "{\"allow_targets\":[\"127.0.0.1/32\"]}"),
        metrics,
        10,
        state,
        &store,
        [&](const TqAcl&) {
            ++updateCalls;
            return true;
        });

    if (response.find("HTTP/1.1 503 Service Unavailable") == std::string::npos) return 590;
    if (response.find("\"code\":\"not_supported\"") == std::string::npos) return 591;
    if (updateCalls != 0) return 592;
    if (state.SnapshotConfig().AllowTargets != cfg.AllowTargets) return 593;
    return 0;
}

int TestServerAdminPatchServerConfigWithoutStoreReturnsNotSupported() {
    TqConfig cfg;
    cfg.Mode = TqMode::Server;
    cfg.ConfigPath = "server.json";
    cfg.AllowTargets = {"10.0.0.0/8"};
    TqServerRuntimeConfigState state(cfg);
    int updateCalls = 0;
    TqServerMetrics metrics;

    const std::string response = TqHandleServerAdmin(
        Request("PATCH", "/server/config", "{\"allow_targets\":[\"127.0.0.1/32\"]}"),
        metrics,
        10,
        state,
        nullptr,
        [&](const TqAcl&) {
            ++updateCalls;
            return true;
        });

    if (response.find("HTTP/1.1 503 Service Unavailable") == std::string::npos) return 594;
    if (response.find("\"code\":\"not_supported\"") == std::string::npos) return 595;
    if (updateCalls != 0) return 596;
    if (state.SnapshotConfig().AllowTargets != cfg.AllowTargets) return 597;
    return 0;
}

int TestServerAdminPatchServerConfigStoreFailureDoesNotCommit() {
    const std::string path = TempServerConfigPath("tcpquic-server-admin-store-failure");
    std::filesystem::remove(path);

    TqConfig cfg;
    cfg.Mode = TqMode::Server;
    cfg.ConfigPath = path;
    cfg.AllowTargets = {"10.0.0.0/8"};
    TqServerRuntimeConfigState state(cfg);
    TqRuntimeConfigFileStore store(path);
    int updateCalls = 0;
    TqServerMetrics metrics;

    const std::string response = TqHandleServerAdmin(
        Request("PATCH", "/server/config", "{\"allow_targets\":[\"127.0.0.1/32\"]}"),
        metrics,
        10,
        state,
        &store,
        [&](const TqAcl&) {
            ++updateCalls;
            return true;
        });

    if (response.find("HTTP/1.1 503 Service Unavailable") == std::string::npos) return 600;
    if (updateCalls != 0) return 601;
    if (state.SnapshotConfig().AllowTargets != cfg.AllowTargets) return 602;
    return 0;
}

int TestServerAdminPatchServerConfigRejectsStartupFieldUnsupported() {
    const std::string path = TempServerConfigPath("tcpquic-server-admin-startup-field");
    std::filesystem::remove(path);
    if (!WriteText(path, "{\"server\":{\"allow_targets\":[\"10.0.0.0/8\"],\"deny_targets\":[]}}\n")) return 610;

    TqConfig cfg;
    cfg.Mode = TqMode::Server;
    cfg.ConfigPath = path;
    cfg.AllowTargets = {"10.0.0.0/8"};
    TqServerRuntimeConfigState state(cfg);
    TqRuntimeConfigFileStore store(path);
    int updateCalls = 0;
    TqServerMetrics metrics;
    const std::string before = ReadText(path);

    const std::string response = TqHandleServerAdmin(
        Request("PATCH", "/server/config", "{\"tls\":{\"cert\":\"server.crt\"}}"),
        metrics,
        10,
        state,
        &store,
        [&](const TqAcl&) {
            ++updateCalls;
            return true;
        });

    if (response.find("HTTP/1.1 503 Service Unavailable") == std::string::npos) return 611;
    if (response.find("\"code\":\"not_supported\"") == std::string::npos) return 612;
    if (updateCalls != 0) return 613;
    if (ReadText(path) != before) return 614;
    if (state.SnapshotConfig().AllowTargets != cfg.AllowTargets) return 615;
    std::filesystem::remove(path);
    return 0;
}

int TestServerAdminPatchServerConfigUpdateFailureDoesNotCommit() {
    const std::string path = TempServerConfigPath("tcpquic-server-admin-update-failure");
    std::filesystem::remove(path);
    if (!WriteText(path, "{\"server\":{\"allow_targets\":[\"10.0.0.0/8\"],\"deny_targets\":[]}}\n")) return 620;

    TqConfig cfg;
    cfg.Mode = TqMode::Server;
    cfg.ConfigPath = path;
    cfg.AllowTargets = {"10.0.0.0/8"};
    TqServerRuntimeConfigState state(cfg);
    TqRuntimeConfigFileStore store(path);
    int updateCalls = 0;
    TqServerMetrics metrics;

    const std::string response = TqHandleServerAdmin(
        Request("PATCH", "/server/config", "{\"allow_targets\":[\"127.0.0.1/32\"]}"),
        metrics,
        10,
        state,
        &store,
        [&](const TqAcl&) {
            ++updateCalls;
            return false;
        });

    if (response.find("HTTP/1.1 503 Service Unavailable") == std::string::npos) return 621;
    if (response.find("\"code\":\"not_supported\"") == std::string::npos) return 622;
    if (updateCalls != 1) return 623;
    if (state.SnapshotConfig().AllowTargets != cfg.AllowTargets) return 624;
    std::filesystem::remove(path);
    return 0;
}

int main() {
    if (int code = TestServerRuntimeConfigStateBuildsAclPatch()) return code;
    if (int code = TestServerRuntimeConfigStateRejectsInvalidCidrWithoutCommit()) return code;
    if (int code = TestParseServerAclPatchAcceptsCommaSeparatedStrings()) return code;
    if (int code = TestParseServerAclPatchRejectsUnknownField()) return code;
    if (int code = TestParseServerAclPatchReportsStartupFieldUnsupported()) return code;
    if (int code = TestServerRuntimeConfigStateKeepsUnpatchedAclSide()) return code;
    if (int code = TestServerRuntimeConfigStateAllowsEmptyAllowList()) return code;
    if (int code = TestServerAdminPatchServerConfigWritesUpdatesAndCommits()) return code;
    if (int code = TestServerAdminGetServerConfigUsesStateSnapshot()) return code;
    if (int code = TestServerAdminPatchServerConfigRejectsInvalidCidrWithoutSideEffects()) return code;
    if (int code = TestServerAdminPatchServerConfigWithoutConfigPathReturnsNotSupported()) return code;
    if (int code = TestServerAdminPatchServerConfigWithoutStoreReturnsNotSupported()) return code;
    if (int code = TestServerAdminPatchServerConfigStoreFailureDoesNotCommit()) return code;
    if (int code = TestServerAdminPatchServerConfigRejectsStartupFieldUnsupported()) return code;
    if (int code = TestServerAdminPatchServerConfigUpdateFailureDoesNotCommit()) return code;

    TqServerMetrics metrics;
    metrics.Listen = "127.0.0.1:1443";

    std::string server = TqHandleServerAdmin(Request("GET", "/server"), metrics, 10);
    if (server.find("HTTP/1.1 200 OK") == std::string::npos) return 1;
    if (server.find("\"role\":\"server\"") == std::string::npos) return 2;

    std::string list = TqHandleServerAdmin(Request("GET", "/server/connections"), metrics, 10);
    if (list.find("HTTP/1.1 200 OK") == std::string::npos) return 3;
    if (list.find("\"connection_id\":\"srv-7\"") == std::string::npos) return 4;
    if (list.find("\"client_name\":\"office-a\"") == std::string::npos) return 25;
    if (list.find("\"encryption\":\"disabled\"") == std::string::npos) return 773;

    std::string get = TqHandleServerAdmin(Request("GET", "/server/connections/srv-7"), metrics, 10);
    if (get.find("\"remote_address\":\"127.0.0.1:5000\"") == std::string::npos) return 5;
    if (get.find("\"client_name\":\"office-a\"") == std::string::npos) return 26;
    if (get.find("\"encryption\":\"disabled\"") == std::string::npos) return 774;

    std::string abort = TqHandleServerAdmin(Request("POST", "/server/connections/srv-7:abort-tunnels"), metrics, 10);
    if (abort.find("HTTP/1.1 202 Accepted") == std::string::npos) return 6;
    if (!g_abortCalled) return 7;

    std::string tunnels = TqHandleServerAdmin(Request("GET", "/server/tunnels"), metrics, 10);
    if (tunnels.find("HTTP/1.1 200 OK") == std::string::npos) return 8;
    if (tunnels.find("\"tunnel_id\":\"tun-5\"") == std::string::npos) return 9;

    std::string getTunnel = TqHandleServerAdmin(Request("GET", "/server/tunnels/tun-5"), metrics, 10);
    if (getTunnel.find("HTTP/1.1 200 OK") == std::string::npos) return 130;
    if (getTunnel.find("\"tunnel_id\":\"tun-5\"") == std::string::npos) return 131;

    std::string getEncodedTunnel = TqHandleServerAdmin(Request("GET", "/server/tunnels/tun%2d5"), metrics, 10);
    if (getEncodedTunnel.find("HTTP/1.1 200 OK") == std::string::npos) return 140;

    std::string abortTunnel = TqHandleServerAdmin(Request("POST", "/server/tunnels/tun-5:abort"), metrics, 10);
    if (abortTunnel.find("HTTP/1.1 202 Accepted") == std::string::npos) return 132;
    if (!g_tunnelAbortCalled) return 133;

    std::string drainTunnel = TqHandleServerAdmin(Request("POST", "/server/tunnels/tun-5:drain"), metrics, 10);
    if (drainTunnel.find("HTTP/1.1 202 Accepted") == std::string::npos) return 134;
    if (!g_tunnelDrainCalled) return 135;

    g_tunnelAbortCalled = false;
    std::string deleteTunnel = TqHandleServerAdmin(Request("DELETE", "/server/tunnels/tun-5"), metrics, 10);
    if (deleteTunnel.find("HTTP/1.1 202 Accepted") == std::string::npos) return 136;
    if (!g_tunnelAbortCalled) return 137;

    std::string missingTunnel = TqHandleServerAdmin(Request("GET", "/server/tunnels/missing"), metrics, 10);
    if (missingTunnel.find("HTTP/1.1 404 Not Found") == std::string::npos) return 138;

    std::string clientTunnel = TqHandleServerAdmin(Request("GET", "/server/tunnels/client-tun"), metrics, 10);
    if (clientTunnel.find("HTTP/1.1 404 Not Found") == std::string::npos) return 139;

    std::string relayMetrics = TqHandleServerAdmin(Request("GET", "/relay/metrics"), metrics, 10);
    if (relayMetrics.find("HTTP/1.1 200 OK") == std::string::npos) return 149;
    if (relayMetrics.find("\"backend\":\"test\"") == std::string::npos) return 150;
    if (relayMetrics.find("\"active_relays\":7") == std::string::npos) return 151;

    if (relayMetrics.find("\"relay_backend\":\"test\"") == std::string::npos) return 180;
    if (relayMetrics.find("\"linux_relay_backend\":\"test\"") == std::string::npos) return 181;
    if (relayMetrics.find("\"relay_active_relays\":7") == std::string::npos) return 182;
    if (relayMetrics.find("\"linux_relay_active_relays\":7") == std::string::npos) return 183;
    if (relayMetrics.find("\"relay_errors\":3") == std::string::npos) return 184;
    if (relayMetrics.find("\"linux_relay_errors\":3") == std::string::npos) return 185;

    std::string relayWorkers = TqHandleServerAdmin(Request("GET", "/relay/workers"), metrics, 10);
    if (relayWorkers.find("HTTP/1.1 200 OK") == std::string::npos) return 152;
    if (relayWorkers.find("\"worker_id\":\"aggregate\"") == std::string::npos) return 153;
    if (relayWorkers.find("\"capabilities\":{") == std::string::npos) return 154;
    if (relayWorkers.find("\"worker_detail\":true") == std::string::npos) return 155;

    std::string activeRelays = TqHandleServerAdmin(Request("GET", "/relay/active-relays"), metrics, 10);
    if (activeRelays.find("HTTP/1.1 200 OK") == std::string::npos) return 154;
    if (activeRelays.find("\"relays\":[") == std::string::npos) return 155;

    std::string aggregateWorker = TqHandleServerAdmin(Request("GET", "/relay/workers/aggregate"), metrics, 10);
    if (aggregateWorker.find("HTTP/1.1 200 OK") == std::string::npos) return 156;
    if (aggregateWorker.find("\"relays\":[") == std::string::npos) return 157;
    if (aggregateWorker.find("\"errors\":") == std::string::npos) return 158;

    std::string missingRelay = TqHandleServerAdmin(Request("GET", "/relay/active-relays/relay-missing"), metrics, 10);
    if (missingRelay.find("HTTP/1.1 503 Service Unavailable") == std::string::npos) return 157;
    if (missingRelay.find("\"code\":\"not_supported\"") == std::string::npos) return 158;

    g_tunnelAbortCalled = false;
    g_tunnelDrainCalled = false;
    std::string clientAbortTunnel = TqHandleServerAdmin(Request("POST", "/server/tunnels/client-tun:abort"), metrics, 10);
    if (clientAbortTunnel.find("HTTP/1.1 404 Not Found") == std::string::npos) return 145;
    if (g_tunnelAbortCalled) return 146;

    std::string clientDrainTunnel = TqHandleServerAdmin(Request("POST", "/server/tunnels/client-tun:drain"), metrics, 10);
    if (clientDrainTunnel.find("HTTP/1.1 404 Not Found") == std::string::npos) return 147;
    if (g_tunnelDrainCalled) return 148;

    std::string emptyTunnel = TqHandleServerAdmin(Request("GET", "/server/tunnels/"), metrics, 10);
    if (emptyTunnel.find("HTTP/1.1 404 Not Found") == std::string::npos) return 141;

    std::string nestedTunnel = TqHandleServerAdmin(Request("GET", "/server/tunnels/tun-5/extra"), metrics, 10);
    if (nestedTunnel.find("HTTP/1.1 404 Not Found") == std::string::npos) return 142;

    std::string badPercentTunnel = TqHandleServerAdmin(Request("GET", "/server/tunnels/tun%xx"), metrics, 10);
    if (badPercentTunnel.find("HTTP/1.1 404 Not Found") == std::string::npos) return 143;

    std::string slashPercentTunnel = TqHandleServerAdmin(Request("GET", "/server/tunnels/tun%2f5"), metrics, 10);
    if (slashPercentTunnel.find("HTTP/1.1 404 Not Found") == std::string::npos) return 144;

    std::string memory = TqHandleServerAdmin(Request("POST", "/memory/allocator:dump"), metrics, 10);
    if (memory.find("HTTP/1.1 200 OK") == std::string::npos) return 10;
    if (memory.find("\"status\":\"dumped\"") == std::string::npos) return 11;
    if (memory.find("\"allocator\":\"mimalloc\"") == std::string::npos) return 12;

    std::string missing = TqHandleServerAdmin(Request("GET", "/server/connections/srv-404"), metrics, 10);
    if (missing.find("HTTP/1.1 404 Not Found") == std::string::npos) return 13;
    {
        TqConfig cfg;
        cfg.Mode = TqMode::Server;
        cfg.QuicListen = "0.0.0.0:4433";
        cfg.AllowTargets = {"127.0.0.1/32"};
        TqServerMetrics configMetrics;
        configMetrics.Listen = cfg.QuicListen;
        configMetrics.ResolvedListens = {"0.0.0.0:4433"};
        std::string config = TqHandleServerAdmin(Request("GET", "/server/config"), configMetrics, 10, cfg);
        if (config.find("HTTP/1.1 200") == std::string::npos) return 120;
        if (config.find("\"allow_targets\"") == std::string::npos) return 121;

        std::string patchRuntime = TqHandleServerAdmin(
            Request("PATCH", "/runtime/config", "{\"compression\":{\"mode\":\"zstd\",\"level\":2},\"tuning\":{\"max_memory_mb\":256}}"),
            configMetrics,
            10,
            cfg);
        if (patchRuntime.find("HTTP/1.1 200 OK") == std::string::npos) return 159;
        if (patchRuntime.find("\"compress\":\"zstd\"") == std::string::npos) return 160;
        if (patchRuntime.find("\"compress_level\":2") == std::string::npos) return 161;
        if (patchRuntime.find("\"max_memory_mb\":256") == std::string::npos) return 162;

        std::string unsupportedRuntime = TqHandleServerAdmin(
            Request("PATCH", "/runtime/config", "{\"tls\":{\"cert\":\"server.crt\"}}"),
            configMetrics,
            10,
            cfg);
        if (unsupportedRuntime.find("HTTP/1.1 503 Service Unavailable") == std::string::npos) return 163;
        if (unsupportedRuntime.find("\"code\":\"not_supported\"") == std::string::npos) return 164;

        std::string diag = TqHandleServerAdmin(Request("GET", "/diagnostics"), configMetrics, 10, cfg);
        if (diag.find("\"trace\"") == std::string::npos) return 122;
        if (diag.find("\"trace_level\":\"info\"") == std::string::npos) return 145;

        TqTraceProxyStubReset();
        std::string patchDiag = TqHandleServerAdmin(
            Request(
                "PATCH",
                "/diagnostics",
                "{\"trace\":false,\"trace_interval_sec\":12,\"trace_level\":\"debug\",\"diag_stats\":true,\"diag_stats_interval_sec\":4}"),
            configMetrics,
            10,
            cfg);
        if (patchDiag.find("HTTP/1.1 200 OK") == std::string::npos) return 146;
        if (patchDiag.find("\"trace\":false") == std::string::npos) return 147;
        if (patchDiag.find("\"trace_interval_sec\":12") == std::string::npos) return 148;
        if (patchDiag.find("\"trace_level\":\"debug\"") == std::string::npos) return 149;
        if (patchDiag.find("\"diag_stats\":true") == std::string::npos) return 150;
        if (g_trace_stub_enabled) return 165;
        if (!g_diag_stats_stub_enabled) return 166;
        if (g_diag_stats_init_count != 1) return 167;
        if (g_diag_stats_stub_interval_sec != 4) return 168;

        std::string enableTrace = TqHandleServerAdmin(
            Request(
                "PATCH",
                "/diagnostics",
                "{\"trace\":true,\"trace_interval_sec\":8,\"trace_level\":\"debug\",\"diag_stats\":false}"),
            configMetrics,
            10,
            cfg);
        if (enableTrace.find("HTTP/1.1 200 OK") == std::string::npos) return 169;
        if (!g_trace_stub_enabled) return 170;
        if (g_trace_init_count != 1) return 171;
        if (g_trace_stub_interval_sec != 8) return 172;
        if (g_trace_stub_level != TqConfig::TraceLevel::Debug) return 173;
        if (g_diag_stats_stub_enabled) return 174;
        if (g_diag_stats_shutdown_count != 1) return 175;

        std::string badDiag = TqHandleServerAdmin(
            Request("PATCH", "/diagnostics", "{\"diag_stats_interval_sec\":0}"),
            configMetrics,
            10,
            cfg);
        if (badDiag.find("HTTP/1.1 400") == std::string::npos) return 151;
    }
    return 0;
}
