#include "trace.h"

#include "exe_path.h"
#include "msquic.hpp"

#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/spdlog.h>

#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

#if !defined(_WIN32)
#include <arpa/inet.h>
#endif

namespace fs = std::filesystem;

namespace {

std::atomic<bool> g_traceEnabled{false};
std::shared_ptr<spdlog::logger> g_logger;
std::mutex g_logMu;
std::thread g_statsThread;
std::atomic<bool> g_statsStop{false};
uint32_t g_statsIntervalSec{0};

struct TqMsgCounters {
    std::atomic<uint64_t> openTx{0};
    std::atomic<uint64_t> openRx{0};
    std::atomic<uint64_t> openOkTx{0};
    std::atomic<uint64_t> openOkRx{0};
    std::atomic<uint64_t> openFailTx{0};
    std::atomic<uint64_t> openFailRx{0};
};

struct TqGlobalCounters {
    std::atomic<uint64_t> quicConnsActive{0};
    std::atomic<uint64_t> streamsActive{0};
    std::atomic<uint64_t> relaysActive{0};
    std::atomic<uint64_t> socksActive{0};
    std::atomic<uint64_t> httpActive{0};
    std::atomic<uint64_t> targetTcpActive{0};
    std::atomic<uint64_t> openOkTotal{0};
    std::atomic<uint64_t> openFailTotal{0};
    TqMsgCounters msgs;
};

TqGlobalCounters g_global;

struct TqTunnelTraceState {
    uint64_t tunnelId{0};
    std::string target;
    std::string role;
    uint32_t connId{0};
    std::chrono::steady_clock::time_point openedAt{};
    bool relayStarted{false};
    TqOpenError lastOpenError{TqOpenError::Ok};
    bool openDone{false};
    bool openOk{false};
};

struct TqQuicConnTraceState {
    MsQuicConnection* connection{nullptr};
    uint32_t connId{0};
    std::string role;
    uint32_t slot{0};
    std::string localAddr;
    std::string peerAddr;
    std::chrono::steady_clock::time_point connectedAt{};
    TqMsgCounters msgs;
    uint64_t streamsOpened{0};
};

std::mutex g_stateMu;
std::unordered_map<uint64_t, TqTunnelTraceState> g_tunnels;
std::unordered_map<uint32_t, TqQuicConnTraceState> g_quicConnsById;
std::atomic<uint64_t> g_nextTunnelId{1};

void LogInfo(const char* fmt, ...) {
    if (!g_traceEnabled.load(std::memory_order_relaxed) || !g_logger) {
        return;
    }
    char buffer[2048];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);
    std::lock_guard<std::mutex> guard(g_logMu);
    g_logger->info("{}", buffer);
}

void LogInfoMultiline(const std::string& header, const std::vector<std::string>& lines) {
    if (!g_traceEnabled.load(std::memory_order_relaxed) || !g_logger) {
        return;
    }
    std::lock_guard<std::mutex> guard(g_logMu);
    g_logger->info("{}", header);
    for (const auto& line : lines) {
        g_logger->info("  {}", line);
    }
}

std::string FormatQuicAddr(MsQuicConnection* connection, uint32_t param) {
    if (connection == nullptr || !connection->IsValid()) {
        return "?";
    }
    QUIC_ADDR addr{};
    uint32_t len = sizeof(addr);
    if (QUIC_FAILED(connection->GetParam(param, &len, &addr))) {
        return "?";
    }
    QUIC_ADDR_STR addrStr{};
    if (!QuicAddrToString(&addr, &addrStr)) {
        return "?";
    }
    return addrStr.Address;
}

std::string CompressLabel(uint8_t flags) {
    if ((flags & TQ_FLAG_COMPRESS) == 0) {
        return "off";
    }
    return "zstd";
}

const char* OpenErrorName(TqOpenError error) {
    switch (error) {
    case TqOpenError::Ok:
        return "ok";
    case TqOpenError::AclDenied:
        return "acl_denied";
    case TqOpenError::DnsFailed:
        return "dns_failed";
    case TqOpenError::TcpTimeout:
        return "tcp_timeout";
    case TqOpenError::TcpRefused:
        return "tcp_refused";
    case TqOpenError::Internal:
    default:
        return "internal";
    }
}

const char* ProxyProtoName(TqTraceProxyProto proto) {
    return proto == TqTraceProxyProto::Socks ? "socks5" : "http";
}

bool CollectQuicStats(MsQuicConnection* connection, QUIC_STATISTICS_V2& stats) {
    if (connection == nullptr || !connection->IsValid()) {
        return false;
    }
    std::memset(&stats, 0, sizeof(stats));
    return QUIC_SUCCEEDED(connection->GetStatistics(&stats));
}

std::vector<std::string> FormatQuicStatsLines(const QUIC_STATISTICS_V2& stats) {
    std::vector<std::string> lines;
    char line[512];
    std::snprintf(
        line,
        sizeof(line),
        "rtt: cur=%uus min=%uus max=%uus var=%uus",
        stats.Rtt,
        stats.MinRtt,
        stats.MaxRtt,
        stats.RttVariance);
    lines.emplace_back(line);
    std::snprintf(
        line,
        sizeof(line),
        "bytes: tx=%llu rx=%llu stream_tx=%llu stream_rx=%llu",
        static_cast<unsigned long long>(stats.SendTotalBytes),
        static_cast<unsigned long long>(stats.RecvTotalBytes),
        static_cast<unsigned long long>(stats.SendTotalStreamBytes),
        static_cast<unsigned long long>(stats.RecvTotalStreamBytes));
    lines.emplace_back(line);
    std::snprintf(
        line,
        sizeof(line),
        "pkts: tx=%llu rx=%llu lost=%llu spurious_lost=%llu reordered=%llu decrypt_fail=%llu",
        static_cast<unsigned long long>(stats.SendTotalPackets),
        static_cast<unsigned long long>(stats.RecvTotalPackets),
        static_cast<unsigned long long>(stats.SendSuspectedLostPackets),
        static_cast<unsigned long long>(stats.SendSpuriousLostPackets),
        static_cast<unsigned long long>(stats.RecvReorderedPackets),
        static_cast<unsigned long long>(stats.RecvDecryptionFailures));
    lines.emplace_back(line);
    std::snprintf(
        line,
        sizeof(line),
        "congestion_events=%u cwnd=%u mtu=%u",
        stats.SendCongestionCount,
        stats.SendCongestionWindow,
        stats.SendPathMtu);
    lines.emplace_back(line);
    return lines;
}

struct TqQuicConnTraceSnapshot {
    MsQuicConnection* connection{nullptr};
    uint32_t connId{0};
    std::string role;
    uint32_t slot{0};
    std::string localAddr;
    std::string peerAddr;
    uint64_t streamsOpened{0};
    uint64_t openTx{0};
    uint64_t openRx{0};
    uint64_t openOkTx{0};
    uint64_t openOkRx{0};
    uint64_t openFailTx{0};
    uint64_t openFailRx{0};
};

void DumpPeriodicStats() {
    if (!g_traceEnabled.load(std::memory_order_relaxed)) {
        return;
    }

    LogInfo("event=stats_tick %s", TqTraceGlobalSnapshot().c_str());

    std::vector<TqQuicConnTraceSnapshot> conns;
    {
        std::lock_guard<std::mutex> guard(g_stateMu);
        conns.reserve(g_quicConnsById.size());
        for (const auto& entry : g_quicConnsById) {
            const auto& conn = entry.second;
            TqQuicConnTraceSnapshot snap;
            snap.connection = conn.connection;
            snap.connId = conn.connId;
            snap.role = conn.role;
            snap.slot = conn.slot;
            snap.localAddr = conn.localAddr;
            snap.peerAddr = conn.peerAddr;
            snap.streamsOpened = conn.streamsOpened;
            snap.openTx = conn.msgs.openTx.load();
            snap.openRx = conn.msgs.openRx.load();
            snap.openOkTx = conn.msgs.openOkTx.load();
            snap.openOkRx = conn.msgs.openOkRx.load();
            snap.openFailTx = conn.msgs.openFailTx.load();
            snap.openFailRx = conn.msgs.openFailRx.load();
            conns.push_back(std::move(snap));
        }
    }

    for (const auto& conn : conns) {
        QUIC_STATISTICS_V2 stats{};
        if (!CollectQuicStats(conn.connection, stats)) {
            LogInfo(
                "event=stats_quic conn=%u role=%s peer=%s state=unavailable",
                conn.connId,
                conn.role.c_str(),
                conn.peerAddr.c_str());
            continue;
        }

        const auto lines = FormatQuicStatsLines(stats);
        LogInfo(
            "event=stats_quic conn=%u role=%s slot=%u peer=%s local=%s streams_opened=%llu app_open_tx=%llu open_rx=%llu open_ok_tx=%llu open_ok_rx=%llu open_fail_tx=%llu open_fail_rx=%llu",
            conn.connId,
            conn.role.c_str(),
            conn.slot,
            conn.peerAddr.c_str(),
            conn.localAddr.c_str(),
            static_cast<unsigned long long>(conn.streamsOpened),
            static_cast<unsigned long long>(conn.openTx),
            static_cast<unsigned long long>(conn.openRx),
            static_cast<unsigned long long>(conn.openOkTx),
            static_cast<unsigned long long>(conn.openOkRx),
            static_cast<unsigned long long>(conn.openFailTx),
            static_cast<unsigned long long>(conn.openFailRx));
        LogInfoMultiline("  quic_metrics:", lines);
    }
}

void StatsThreadMain() {
    while (!g_statsStop.load(std::memory_order_relaxed)) {
        const uint32_t interval = g_statsIntervalSec;
        if (interval == 0) {
            break;
        }
        for (uint32_t i = 0; i < interval && !g_statsStop.load(); ++i) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
        if (g_statsStop.load()) {
            break;
        }
        DumpPeriodicStats();
    }
}

TqQuicConnTraceState* FindQuicConnLocked(uint32_t connId) {
    auto it = g_quicConnsById.find(connId);
    return it == g_quicConnsById.end() ? nullptr : &it->second;
}

} // namespace

void TqTraceIncOpenTx(uint32_t connId) {
    if (!TqTraceEnabled()) {
        return;
    }
    g_global.msgs.openTx.fetch_add(1);
    std::lock_guard<std::mutex> guard(g_stateMu);
    if (auto* conn = FindQuicConnLocked(connId)) {
        conn->msgs.openTx.fetch_add(1);
    }
}

void TqTraceIncOpenRx(uint32_t connId) {
    if (!TqTraceEnabled()) {
        return;
    }
    g_global.msgs.openRx.fetch_add(1);
    std::lock_guard<std::mutex> guard(g_stateMu);
    if (auto* conn = FindQuicConnLocked(connId)) {
        conn->msgs.openRx.fetch_add(1);
    }
}

bool TqFormatSocketPeerAddr(TqSocketHandle fd, std::string& out) {
    out.clear();
    if (!TqSocketValid(fd)) {
        return false;
    }

    sockaddr_storage addr{};
    socklen_t len = sizeof(addr);
    if (getpeername(fd, reinterpret_cast<sockaddr*>(&addr), &len) != 0) {
        return false;
    }

    char host[INET6_ADDRSTRLEN]{};
    char port[16]{};
    if (addr.ss_family == AF_INET) {
        const auto* in = reinterpret_cast<const sockaddr_in*>(&addr);
        if (TqInetNtop(AF_INET, &in->sin_addr, host, sizeof(host)) == nullptr) {
            return false;
        }
        std::snprintf(port, sizeof(port), "%u", ntohs(in->sin_port));
    } else if (addr.ss_family == AF_INET6) {
        const auto* in6 = reinterpret_cast<const sockaddr_in6*>(&addr);
        if (TqInetNtop(AF_INET6, &in6->sin6_addr, host, sizeof(host)) == nullptr) {
            return false;
        }
        std::snprintf(port, sizeof(port), "%u", ntohs(in6->sin6_port));
    } else {
        return false;
    }

    out = std::string(host) + ":" + port;
    return true;
}

bool TqTraceInit(TqMode mode, uint32_t statsIntervalSec) {
    if (g_traceEnabled.load()) {
        return true;
    }

    std::string exeDir;
    if (!TqGetExecutableDirectory(exeDir)) {
        exeDir = ".";
    }

    const fs::path logDir = fs::path(exeDir) / "log";
    std::error_code ec;
    fs::create_directories(logDir, ec);

    const char* logName = mode == TqMode::Client ? "client.log" : "server.log";
    const fs::path logPath = logDir / logName;

    try {
        auto sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(logPath.string(), true);
        g_logger = std::make_shared<spdlog::logger>("tcpquic-trace", sink);
        g_logger->set_level(spdlog::level::info);
        g_logger->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%l] %v");
        g_logger->flush_on(spdlog::level::info);
    } catch (const std::exception& ex) {
        std::fprintf(stderr, "tcpquic-proxy: failed to open trace log %s: %s\n", logPath.string().c_str(), ex.what());
        g_logger.reset();
        return false;
    }

    g_statsIntervalSec = statsIntervalSec;
    g_statsStop.store(false);
    g_traceEnabled.store(true);

    LogInfo(
        "event=trace_started role=%s log=%s interval=%us",
        mode == TqMode::Client ? "client" : "server",
        logPath.string().c_str(),
        statsIntervalSec);

    if (statsIntervalSec > 0) {
        g_statsThread = std::thread(StatsThreadMain);
    }

    return true;
}

void TqTraceShutdown() {
    if (!g_traceEnabled.exchange(false)) {
        return;
    }

    g_statsStop.store(true);
    if (g_statsThread.joinable()) {
        g_statsThread.join();
    }

    DumpPeriodicStats();
    LogInfo("event=trace_stopped");

    {
        std::lock_guard<std::mutex> guard(g_logMu);
        if (g_logger) {
            g_logger->flush();
            spdlog::drop(g_logger->name());
            g_logger.reset();
        }
    }

    {
        std::lock_guard<std::mutex> guard(g_stateMu);
        g_tunnels.clear();
        g_quicConnsById.clear();
    }
}

bool TqTraceEnabled() {
    return g_traceEnabled.load(std::memory_order_relaxed);
}

std::string TqTraceGlobalSnapshot() {
    char buffer[512];
    std::snprintf(
        buffer,
        sizeof(buffer),
        "global: quic_conns=%llu streams=%llu relays=%llu socks=%llu http=%llu target_tcp=%llu open_ok=%llu open_fail=%llu open_tx=%llu open_rx=%llu open_ok_tx=%llu open_ok_rx=%llu open_fail_tx=%llu open_fail_rx=%llu",
        static_cast<unsigned long long>(g_global.quicConnsActive.load()),
        static_cast<unsigned long long>(g_global.streamsActive.load()),
        static_cast<unsigned long long>(g_global.relaysActive.load()),
        static_cast<unsigned long long>(g_global.socksActive.load()),
        static_cast<unsigned long long>(g_global.httpActive.load()),
        static_cast<unsigned long long>(g_global.targetTcpActive.load()),
        static_cast<unsigned long long>(g_global.openOkTotal.load()),
        static_cast<unsigned long long>(g_global.openFailTotal.load()),
        static_cast<unsigned long long>(g_global.msgs.openTx.load()),
        static_cast<unsigned long long>(g_global.msgs.openRx.load()),
        static_cast<unsigned long long>(g_global.msgs.openOkTx.load()),
        static_cast<unsigned long long>(g_global.msgs.openOkRx.load()),
        static_cast<unsigned long long>(g_global.msgs.openFailTx.load()),
        static_cast<unsigned long long>(g_global.msgs.openFailRx.load()));
    return buffer;
}

void TqTraceQuicConnecting(const char* role, uint32_t slot, const char* peer) {
    if (!TqTraceEnabled()) {
        return;
    }
    LogInfo("event=quic_connecting role=%s slot=%u peer=%s %s", role, slot, peer, TqTraceGlobalSnapshot().c_str());
}

void TqTraceQuicIncoming() {
    if (!TqTraceEnabled()) {
        return;
    }
    LogInfo("event=quic_incoming role=server %s", TqTraceGlobalSnapshot().c_str());
}

void TqTraceQuicConnected(
    MsQuicConnection* connection,
    uint32_t connId,
    const char* role,
    uint32_t slot) {
    if (!TqTraceEnabled()) {
        return;
    }

    const std::string localAddr = FormatQuicAddr(connection, QUIC_PARAM_CONN_LOCAL_ADDRESS);
    const std::string peerAddr = FormatQuicAddr(connection, QUIC_PARAM_CONN_REMOTE_ADDRESS);
    const auto connectedAt = std::chrono::steady_clock::now();

    {
        std::lock_guard<std::mutex> guard(g_stateMu);
        auto& state = g_quicConnsById[connId];
        state.connection = connection;
        state.connId = connId;
        state.role = role;
        state.slot = slot;
        state.localAddr = localAddr;
        state.peerAddr = peerAddr;
        state.connectedAt = connectedAt;
    }

    g_global.quicConnsActive.fetch_add(1);

    QUIC_STATISTICS_V2 stats{};
    const bool haveStats = CollectQuicStats(connection, stats);
    const auto lines = haveStats ? FormatQuicStatsLines(stats) : std::vector<std::string>{};

    LogInfo(
        "event=quic_connected role=%s conn=%u slot=%u local=%s peer=%s %s",
        role,
        connId,
        slot,
        localAddr.c_str(),
        peerAddr.c_str(),
        TqTraceGlobalSnapshot().c_str());
    if (!lines.empty()) {
        LogInfoMultiline("  quic_metrics:", lines);
    }
}

void TqTraceQuicShutdownTransport(
    MsQuicConnection* connection,
    uint32_t connId,
    const char* role,
    uint32_t status,
    uint64_t errorCode) {
    if (!TqTraceEnabled()) {
        return;
    }
    (void)connection;
    LogInfo(
        "event=quic_shutdown_transport role=%s conn=%u status=0x%x code=%llu %s",
        role,
        connId,
        status,
        static_cast<unsigned long long>(errorCode),
        TqTraceGlobalSnapshot().c_str());
}

void TqTraceQuicShutdownPeer(
    MsQuicConnection* connection,
    uint32_t connId,
    const char* role,
    uint64_t errorCode) {
    if (!TqTraceEnabled()) {
        return;
    }
    (void)connection;
    LogInfo(
        "event=quic_shutdown_peer role=%s conn=%u code=%llu %s",
        role,
        connId,
        static_cast<unsigned long long>(errorCode),
        TqTraceGlobalSnapshot().c_str());
}

void TqTraceQuicDisconnected(
    MsQuicConnection* connection,
    uint32_t connId,
    const char* role) {
    if (!TqTraceEnabled()) {
        return;
    }

    uint32_t slot = 0;
    std::string peerAddr;
    uint64_t streamsOpened = 0;
    std::chrono::steady_clock::time_point connectedAt{};
    bool found = false;
    {
        std::lock_guard<std::mutex> guard(g_stateMu);
        if (auto* entry = FindQuicConnLocked(connId)) {
            slot = entry->slot;
            peerAddr = entry->peerAddr;
            streamsOpened = entry->streamsOpened;
            connectedAt = entry->connectedAt;
            found = true;
            g_quicConnsById.erase(connId);
        }
    }

    if (g_global.quicConnsActive.load() > 0) {
        g_global.quicConnsActive.fetch_sub(1);
    }

    const auto now = std::chrono::steady_clock::now();
    const double lifetimeSec = found
        ? std::chrono::duration<double>(now - connectedAt).count()
        : 0.0;

    QUIC_STATISTICS_V2 stats{};
    const bool haveStats = CollectQuicStats(connection, stats);
    const auto lines = haveStats ? FormatQuicStatsLines(stats) : std::vector<std::string>{};

    LogInfo(
        "event=quic_disconnected role=%s conn=%u slot=%u peer=%s lifetime=%.1fs streams_opened=%llu %s",
        role,
        connId,
        found ? slot : 0u,
        found ? peerAddr.c_str() : "?",
        lifetimeSec,
        found ? static_cast<unsigned long long>(streamsOpened) : 0ULL,
        TqTraceGlobalSnapshot().c_str());
    if (!lines.empty()) {
        LogInfoMultiline("  quic_final_metrics:", lines);
    }
}

uint64_t TqTraceStreamStarted(
    MsQuicConnection* connection,
    uint32_t connId,
    const char* role,
    const char* target,
    uint8_t compressFlags) {
    if (!TqTraceEnabled()) {
        return 0;
    }
    (void)connection;

    const uint64_t tunnelId = g_nextTunnelId.fetch_add(1);
    TqTunnelTraceState tunnel;
    tunnel.tunnelId = tunnelId;
    tunnel.target = target != nullptr ? target : "?";
    tunnel.role = role;
    tunnel.connId = connId;
    tunnel.openedAt = std::chrono::steady_clock::now();

    {
        std::lock_guard<std::mutex> guard(g_stateMu);
        g_tunnels[tunnelId] = tunnel;
        if (auto* conn = FindQuicConnLocked(connId)) {
            conn->streamsOpened++;
        }
    }

    g_global.streamsActive.fetch_add(1);

    LogInfo(
        "event=stream_started role=%s conn=%u tunnel=%llu target=%s compress=%s %s",
        role,
        connId,
        static_cast<unsigned long long>(tunnelId),
        tunnel.target.c_str(),
        CompressLabel(compressFlags).c_str(),
        TqTraceGlobalSnapshot().c_str());
    return tunnelId;
}

void TqTraceOpenResult(uint64_t tunnelId, bool ok, TqOpenError error, uint32_t connIdField) {
    if (!TqTraceEnabled() || tunnelId == 0) {
        return;
    }

    {
        std::lock_guard<std::mutex> guard(g_stateMu);
        auto it = g_tunnels.find(tunnelId);
        if (it != g_tunnels.end()) {
            it->second.openDone = true;
            it->second.openOk = ok;
            it->second.lastOpenError = error;
        }
    }

    if (ok) {
        g_global.openOkTotal.fetch_add(1);
    } else {
        g_global.openFailTotal.fetch_add(1);
    }

    LogInfo(
        "event=open_result tunnel=%llu ok=%d error=%s conn_id_field=%u %s",
        static_cast<unsigned long long>(tunnelId),
        ok ? 1 : 0,
        OpenErrorName(error),
        connIdField,
        TqTraceGlobalSnapshot().c_str());
}

void TqTraceRelayStarted(uint64_t tunnelId) {
    if (!TqTraceEnabled() || tunnelId == 0) {
        return;
    }

    {
        std::lock_guard<std::mutex> guard(g_stateMu);
        auto it = g_tunnels.find(tunnelId);
        if (it != g_tunnels.end()) {
            it->second.relayStarted = true;
        }
    }

    g_global.relaysActive.fetch_add(1);
    LogInfo(
        "event=relay_started tunnel=%llu %s",
        static_cast<unsigned long long>(tunnelId),
        TqTraceGlobalSnapshot().c_str());
}

void TqTraceStreamClosed(
    uint64_t tunnelId,
    const char* role,
    const char* target,
    bool relayStarted,
    TqOpenError closeReason) {
    if (!TqTraceEnabled() || tunnelId == 0) {
        return;
    }

    double durationSec = 0.0;
    {
        std::lock_guard<std::mutex> guard(g_stateMu);
        auto it = g_tunnels.find(tunnelId);
        if (it != g_tunnels.end()) {
            durationSec = std::chrono::duration<double>(
                std::chrono::steady_clock::now() - it->second.openedAt).count();
            g_tunnels.erase(it);
        }
    }

    if (g_global.streamsActive.load() > 0) {
        g_global.streamsActive.fetch_sub(1);
    }
    if (relayStarted && g_global.relaysActive.load() > 0) {
        g_global.relaysActive.fetch_sub(1);
    }

    LogInfo(
        "event=stream_closed role=%s tunnel=%llu target=%s relay=%d duration=%.1fs reason=%s %s",
        role,
        static_cast<unsigned long long>(tunnelId),
        target != nullptr ? target : "?",
        relayStarted ? 1 : 0,
        durationSec,
        OpenErrorName(closeReason),
        TqTraceGlobalSnapshot().c_str());
}

void TqTraceProxyAccepted(TqTraceProxyProto proto, TqSocketHandle fd) {
    if (!TqTraceEnabled()) {
        return;
    }

    std::string peer;
    if (!TqFormatSocketPeerAddr(fd, peer)) {
        peer = "?";
    }

    if (proto == TqTraceProxyProto::Socks) {
        g_global.socksActive.fetch_add(1);
    } else {
        g_global.httpActive.fetch_add(1);
    }

    LogInfo(
        "event=proxy_tcp_accepted proto=%s fd=%d peer=%s %s",
        ProxyProtoName(proto),
        static_cast<int>(fd),
        peer.c_str(),
        TqTraceGlobalSnapshot().c_str());
}

void TqTraceProxyTunnelOk(TqTraceProxyProto proto, const char* target, uint64_t tunnelId) {
    if (!TqTraceEnabled()) {
        return;
    }
    LogInfo(
        "event=proxy_tunnel_ok proto=%s target=%s tunnel=%llu %s",
        ProxyProtoName(proto),
        target != nullptr ? target : "?",
        static_cast<unsigned long long>(tunnelId),
        TqTraceGlobalSnapshot().c_str());
}

void TqTraceProxyTunnelFail(TqTraceProxyProto proto, const char* target, TqOpenError error) {
    if (!TqTraceEnabled()) {
        return;
    }
    LogInfo(
        "event=proxy_tunnel_fail proto=%s target=%s error=%s %s",
        ProxyProtoName(proto),
        target != nullptr ? target : "?",
        OpenErrorName(error),
        TqTraceGlobalSnapshot().c_str());
}

void TqTraceProxyClosed(TqTraceProxyProto proto, TqSocketHandle fd) {
    if (!TqTraceEnabled()) {
        return;
    }

    if (proto == TqTraceProxyProto::Socks) {
        if (g_global.socksActive.load() > 0) {
            g_global.socksActive.fetch_sub(1);
        }
    } else if (g_global.httpActive.load() > 0) {
        g_global.httpActive.fetch_sub(1);
    }

    LogInfo(
        "event=proxy_tcp_closed proto=%s fd=%d %s",
        ProxyProtoName(proto),
        static_cast<int>(fd),
        TqTraceGlobalSnapshot().c_str());
}

void TqTraceTargetTcpDialing(uint64_t tunnelId, const char* target) {
    if (!TqTraceEnabled() || tunnelId == 0) {
        return;
    }
    LogInfo(
        "event=target_tcp_dialing tunnel=%llu target=%s %s",
        static_cast<unsigned long long>(tunnelId),
        target != nullptr ? target : "?",
        TqTraceGlobalSnapshot().c_str());
}

void TqTraceTargetTcpConnected(uint64_t tunnelId, TqSocketHandle fd) {
    if (!TqTraceEnabled() || tunnelId == 0) {
        return;
    }
    g_global.targetTcpActive.fetch_add(1);
    LogInfo(
        "event=target_tcp_connected tunnel=%llu fd=%d %s",
        static_cast<unsigned long long>(tunnelId),
        static_cast<int>(fd),
        TqTraceGlobalSnapshot().c_str());
}

void TqTraceTargetTcpFailed(uint64_t tunnelId, TqOpenError error) {
    if (!TqTraceEnabled() || tunnelId == 0) {
        return;
    }
    LogInfo(
        "event=target_tcp_failed tunnel=%llu error=%s %s",
        static_cast<unsigned long long>(tunnelId),
        OpenErrorName(error),
        TqTraceGlobalSnapshot().c_str());
}

void TqTraceTargetTcpClosed(uint64_t tunnelId) {
    if (!TqTraceEnabled() || tunnelId == 0) {
        return;
    }
    if (g_global.targetTcpActive.load() > 0) {
        g_global.targetTcpActive.fetch_sub(1);
    }
    LogInfo(
        "event=target_tcp_closed tunnel=%llu %s",
        static_cast<unsigned long long>(tunnelId),
        TqTraceGlobalSnapshot().c_str());
}
