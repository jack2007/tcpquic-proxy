#include "trace.h"

#include "exe_path.h"
#include "msquic.hpp"
#include "relay_metrics.h"

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

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <cerrno>
#include <limits.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#if !defined(_WIN32)
#include <arpa/inet.h>
#endif

namespace fs = std::filesystem;

namespace {

std::atomic<bool> g_traceEnabled{false};
std::atomic<bool> g_diagStatsEnabled{false};
std::shared_ptr<spdlog::logger> g_logger;
std::mutex g_logMu;
std::thread g_statsThread;
std::atomic<bool> g_statsStop{false};
uint32_t g_statsIntervalSec{0};
std::thread g_diagStatsThread;
std::atomic<bool> g_diagStatsStop{false};
uint32_t g_diagStatsIntervalSec{0};

struct TqMsgCounters {
    std::atomic<uint64_t> openTx{0};
    std::atomic<uint64_t> openRx{0};
    std::atomic<uint64_t> openOkTx{0};
    std::atomic<uint64_t> openOkRx{0};
    std::atomic<uint64_t> openFailTx{0};
    std::atomic<uint64_t> openFailRx{0};
};

struct TqGlobalCounters {
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
    bool hasNetStats{false};
    TqTraceNetworkStats netStats;
};

std::mutex g_stateMu;
std::unordered_map<uint64_t, TqTunnelTraceState> g_tunnels;
std::unordered_map<uint32_t, TqQuicConnTraceState> g_quicConnsById;
std::atomic<uint64_t> g_nextTunnelId{1};
std::mutex g_diagStatsMu;
bool g_diagHasNetStats{false};
TqTraceNetworkStats g_diagNetStats;

void LogInfo(const char* fmt, ...) {
    if (!g_traceEnabled.load(std::memory_order_relaxed) || !g_logger) {
        return;
    }
    char buffer[2048];
    va_list args;
    va_start(args, fmt);
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wformat-nonliteral"
#endif
    std::vsnprintf(buffer, sizeof(buffer), fmt, args);
#if defined(__clang__)
#pragma clang diagnostic pop
#endif
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

} // namespace

std::vector<std::string> TqFormatTraceQuicStatsLines(const QUIC_STATISTICS_V2& stats) {
    std::vector<std::string> lines;
    char line[512];
    std::snprintf(
        line,
        sizeof(line),
        "rtt: cur=%.3fms min=%.3fms max=%.3fms var=%.3fms",
        static_cast<double>(stats.Rtt) / 1000.0,
        static_cast<double>(stats.MinRtt) / 1000.0,
        static_cast<double>(stats.MaxRtt) / 1000.0,
        static_cast<double>(stats.RttVariance) / 1000.0);
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
    const double lossRate =
        stats.SendTotalPackets == 0
            ? 0.0
            : static_cast<double>(stats.SendSuspectedLostPackets) * 100.0 /
                  static_cast<double>(stats.SendTotalPackets);
    std::snprintf(
        line,
        sizeof(line),
        "pkts: tx=%llu rx=%llu lost=%llu loss_rate=%.2f%% spurious_lost=%llu reordered=%llu decrypt_fail=%llu",
        static_cast<unsigned long long>(stats.SendTotalPackets),
        static_cast<unsigned long long>(stats.RecvTotalPackets),
        static_cast<unsigned long long>(stats.SendSuspectedLostPackets),
        lossRate,
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

namespace {

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
    bool hasNetStats{false};
    TqTraceNetworkStats netStats;
};

void DumpPeriodicStats() {
    if (!g_traceEnabled.load(std::memory_order_relaxed)) {
        return;
    }

    LogInfo("event=stats_tick %s", TqTraceGlobalSnapshot().c_str());

    const TqRelayMetricsSnapshot relayMetrics = TqSnapshotRelayMetrics();
    LogInfo("event=stats_relay %s", TqFormatRelayMetricsSnapshotLine(relayMetrics).c_str());

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
            snap.hasNetStats = conn.hasNetStats;
            snap.netStats = conn.netStats;
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

        auto lines = TqFormatTraceQuicStatsLines(stats);
        if (conn.hasNetStats) {
            lines.push_back(TqFormatTraceNetworkStatsLine(conn.netStats));
        }
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

void DumpDiagStats() {
    TqTraceNetworkStats netStats;
    bool hasNetStats = false;
    {
        std::lock_guard<std::mutex> guard(g_diagStatsMu);
        hasNetStats = g_diagHasNetStats;
        netStats = g_diagNetStats;
    }

    const TqRelayMetricsSnapshot relayMetrics = TqSnapshotRelayMetrics();
    std::fprintf(
        stderr,
        "tcpquic-proxy diag_stats: %s",
        TqFormatRelayMetricsSnapshotLine(relayMetrics).c_str());
    if (hasNetStats) {
        std::fprintf(stderr, " %s", TqFormatTraceNetworkStatsLine(netStats).c_str());
    } else {
        std::fprintf(stderr, " net_stats: unavailable");
    }
    std::fprintf(stderr, "\n");
    std::fflush(stderr);
}

void DiagStatsThreadMain() {
    while (!g_diagStatsStop.load(std::memory_order_relaxed)) {
        for (uint32_t i = 0; i < g_diagStatsIntervalSec; ++i) {
            if (g_diagStatsStop.load(std::memory_order_relaxed)) {
                return;
            }
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
        if (!g_diagStatsStop.load(std::memory_order_relaxed)) {
            DumpDiagStats();
        }
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

TqQuicConnTraceState* FindQuicConnLocked(MsQuicConnection* connection) {
    if (connection == nullptr) {
        return nullptr;
    }
    for (auto& entry : g_quicConnsById) {
        if (entry.second.connection == connection) {
            return &entry.second;
        }
    }
    return nullptr;
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

std::string JoinTracePath(const std::string& base, const char* child) {
    if (child == nullptr || child[0] == '\0') {
        return base;
    }
    if (base.empty()) {
        return child;
    }
    if (base.back() == '/' || base.back() == '\\') {
        return base + child;
    }
    return base + "/" + child;
}

bool EnsureTraceLogDirectory(const std::string& logDir) {
#if defined(_WIN32)
    if (CreateDirectoryA(logDir.c_str(), nullptr) != 0) {
        return true;
    }
    return GetLastError() == ERROR_ALREADY_EXISTS;
#else
    if (mkdir(logDir.c_str(), 0755) == 0) {
        return true;
    }
    return errno == EEXIST;
#endif
}

bool TqTraceInit(TqMode mode, uint32_t statsIntervalSec) {
    if (g_traceEnabled.load()) {
        return true;
    }

    char exeDir[PATH_MAX]{};
    if (!TqGetExecutableDirectory(exeDir, sizeof(exeDir))) {
        std::strncpy(exeDir, ".", sizeof(exeDir) - 1);
    }

#if defined(_WIN32)
    wchar_t savedCwd[MAX_PATH]{};
    const bool haveSavedCwd = GetCurrentDirectoryW(MAX_PATH, savedCwd) != 0;
    bool changedCwd = false;
    if (std::strcmp(exeDir, ".") != 0) {
        int required = MultiByteToWideChar(CP_UTF8, 0, exeDir, -1, nullptr, 0, nullptr, nullptr);
        if (required > 0) {
            std::wstring wideExeDir(static_cast<size_t>(required - 1), L'\0');
            if (MultiByteToWideChar(
                    CP_UTF8,
                    0,
                    exeDir,
                    -1,
                    wideExeDir.data(),
                    required) > 0) {
                changedCwd = SetCurrentDirectoryW(wideExeDir.c_str()) != 0;
            }
        }
    }
#else
    if (std::strcmp(exeDir, ".") != 0 && chdir(exeDir) != 0) {
        std::strncpy(exeDir, ".", sizeof(exeDir) - 1);
    }
#endif

    const char* logName = mode == TqMode::Client ? "client.log" : "server.log";
    char logPathForMessage[PATH_MAX]{};
    if (std::strcmp(exeDir, ".") == 0) {
        std::snprintf(logPathForMessage, sizeof(logPathForMessage), "./log/%s", logName);
    } else {
        std::snprintf(logPathForMessage, sizeof(logPathForMessage), "%s/log/%s", exeDir, logName);
    }

    const std::string logDir = "./log";
    if (!EnsureTraceLogDirectory(logDir)) {
        std::fprintf(stderr, "tcpquic-proxy: failed to create trace log dir %s\n", logDir.c_str());
        return false;
    }

    const std::string logPath = JoinTracePath(logDir, logName);

    try {
        auto sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(logPath, true);
        g_logger = std::make_shared<spdlog::logger>("tcpquic-trace", sink);
        g_logger->set_level(spdlog::level::info);
        g_logger->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%l] %v");
        g_logger->flush_on(spdlog::level::info);
    } catch (const std::exception& ex) {
        std::fprintf(
            stderr,
            "tcpquic-proxy: failed to open trace log %s: %s\n",
            logPathForMessage,
            ex.what());
        g_logger.reset();
        return false;
    }

    g_statsIntervalSec = statsIntervalSec;
    g_statsStop.store(false);
    g_traceEnabled.store(true);

    LogInfo(
        "event=trace_started role=%s log=%s interval=%us",
        mode == TqMode::Client ? "client" : "server",
        logPathForMessage,
        statsIntervalSec);

    if (statsIntervalSec > 0) {
        g_statsThread = std::thread(StatsThreadMain);
    }

#if defined(_WIN32)
    if (haveSavedCwd && changedCwd) {
        SetCurrentDirectoryW(savedCwd);
    }
#endif

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

bool TqDiagStatsInit(uint32_t statsIntervalSec) {
    if (statsIntervalSec == 0) {
        statsIntervalSec = 5;
    }
    TqDiagStatsShutdown();
    {
        std::lock_guard<std::mutex> guard(g_diagStatsMu);
        g_diagHasNetStats = false;
        g_diagNetStats = TqTraceNetworkStats{};
    }
    g_diagStatsIntervalSec = statsIntervalSec;
    g_diagStatsStop.store(false, std::memory_order_relaxed);
    g_diagStatsEnabled.store(true, std::memory_order_release);
    g_diagStatsThread = std::thread(DiagStatsThreadMain);
    return true;
}

void TqDiagStatsShutdown() {
    g_diagStatsEnabled.store(false, std::memory_order_release);
    g_diagStatsStop.store(true, std::memory_order_relaxed);
    if (g_diagStatsThread.joinable()) {
        g_diagStatsThread.join();
    }
}

bool TqDiagStatsEnabled() {
    return g_diagStatsEnabled.load(std::memory_order_relaxed);
}

std::string FormatTraceLinuxRelayStateLine(
    const char* eventName,
    const TqTraceLinuxRelayStreamState& state) {
    char buffer[768];
    std::snprintf(
        buffer,
        sizeof(buffer),
        "event=%s worker=%u relay=%llu outstanding_quic_sends=%llu outstanding_quic_send_bytes=%llu pending_tcp_write_queue=%llu pending_tcp_write_bytes=%llu pending_quic_receive_bytes=%llu tcp_read_bytes=%llu tcp_write_bytes=%llu tcp_write_errno=%llu tcp_read_closed=%d tcp_write_closed=%d quic_send_fin_submitted=%d quic_send_fin_completed=%d tcp_write_shutdown_queued=%d stream_detached=%d",
        eventName != nullptr ? eventName : "linux_relay_state",
        state.WorkerIndex,
        static_cast<unsigned long long>(state.RelayId),
        static_cast<unsigned long long>(state.OutstandingQuicSends),
        static_cast<unsigned long long>(state.OutstandingQuicSendBytes),
        static_cast<unsigned long long>(state.PendingTcpWriteQueue),
        static_cast<unsigned long long>(state.PendingTcpWriteBytes),
        static_cast<unsigned long long>(state.PendingQuicReceiveBytes),
        static_cast<unsigned long long>(state.TcpReadBytes),
        static_cast<unsigned long long>(state.TcpWriteBytes),
        static_cast<unsigned long long>(state.TcpWriteErrno),
        state.TcpReadClosed ? 1 : 0,
        state.TcpWriteClosed ? 1 : 0,
        state.QuicSendFinSubmitted ? 1 : 0,
        state.QuicSendFinCompleted ? 1 : 0,
        state.TcpWriteShutdownQueued ? 1 : 0,
        state.StreamDetached ? 1 : 0);
    return buffer;
}

std::string TqFormatTraceLinuxRelayStreamShutdownLine(
    const TqTraceLinuxRelayStreamState& state) {
    return FormatTraceLinuxRelayStateLine("linux_relay_stream_shutdown", state);
}

std::string TqFormatTraceRelayStateLine(
    const char* eventName,
    const char* backend,
    const TqTraceLinuxRelayStreamState& state) {
    char buffer[832];
    std::snprintf(
        buffer,
        sizeof(buffer),
        "%s backend=%s",
        FormatTraceLinuxRelayStateLine(eventName != nullptr ? eventName : "relay_state", state).c_str(),
        backend != nullptr ? backend : "?");
    return buffer;
}

void TqTraceRelayStreamEvent(
    const char* backend,
    uint32_t workerIndex,
    uint64_t relayId,
    const char* streamEvent,
    uint64_t errorCode,
    uint32_t status,
    uint64_t absoluteOffset,
    uint64_t totalBufferLength,
    uint32_t bufferCount,
    uint32_t receiveFlags,
    bool fin,
    const TqTraceLinuxRelayStreamState& state) {
    if (!TqTraceEnabled()) {
        return;
    }
    TqTraceLinuxRelayStreamState traceState = state;
    traceState.WorkerIndex = workerIndex;
    traceState.RelayId = relayId;
    LogInfo(
        "%s stream_event=%s error_code=%llu status=0x%x absolute_offset=%llu total_buffer_length=%llu buffer_count=%u receive_flags=0x%x fin=%d",
        TqFormatTraceRelayStateLine("relay_stream_event", backend, traceState).c_str(),
        streamEvent != nullptr ? streamEvent : "?",
        static_cast<unsigned long long>(errorCode),
        status,
        static_cast<unsigned long long>(absoluteOffset),
        static_cast<unsigned long long>(totalBufferLength),
        bufferCount,
        receiveFlags,
        fin ? 1 : 0);
}

void TqTraceRelayStopCondition(
    const char* backend,
    uint32_t workerIndex,
    const char* trigger,
    const TqTraceLinuxRelayStreamState& state) {
    if (!TqTraceEnabled()) {
        return;
    }
    TqTraceLinuxRelayStreamState traceState = state;
    traceState.WorkerIndex = workerIndex;
    LogInfo(
        "%s trigger=%s",
        TqFormatTraceRelayStateLine("relay_stop_condition", backend, traceState).c_str(),
        trigger != nullptr ? trigger : "?");
}

void TqTraceRelayBackpressureEvent(
    const char* backend,
    uint32_t workerIndex,
    uint64_t relayId,
    const char* action,
    const char* reason,
    uint64_t outstandingQuicSendBytes,
    uint64_t pauseThreshold,
    uint64_t resumeThreshold,
    uint64_t readAheadBytes) {
    if (!TqTraceEnabled()) {
        return;
    }
    LogInfo(
        "event=relay_backpressure backend=%s worker=%u relay=%llu action=%s reason=%s outstanding_quic_send_bytes=%llu pause_threshold=%llu resume_threshold=%llu read_ahead=%llu",
        backend != nullptr ? backend : "?",
        workerIndex,
        static_cast<unsigned long long>(relayId),
        action != nullptr ? action : "?",
        reason != nullptr ? reason : "?",
        static_cast<unsigned long long>(outstandingQuicSendBytes),
        static_cast<unsigned long long>(pauseThreshold),
        static_cast<unsigned long long>(resumeThreshold),
        static_cast<unsigned long long>(readAheadBytes));
}

void TqTraceRelayStreamShutdown(
    const char* backend,
    const TqTraceLinuxRelayStreamState& state) {
    if (!TqTraceEnabled()) {
        return;
    }
    LogInfo("%s", TqFormatTraceRelayStateLine("relay_stream_shutdown", backend, state).c_str());
}

void TqTraceRelayUnregister(
    const char* backend,
    const TqTraceLinuxRelayStreamState& state) {
    if (!TqTraceEnabled()) {
        return;
    }
    LogInfo("%s", TqFormatTraceRelayStateLine("relay_unregister", backend, state).c_str());
}

void TqTraceRelayFatalError(
    const char* backend,
    const char* reason,
    uint64_t relayId,
    uint64_t socketOrFd,
    uint64_t pendingQuicReceiveBytes,
    uint64_t pendingQuicReceiveQueue,
    uint64_t pendingQuicSends,
    uint64_t inflightQuicSends,
    uint64_t inflightTcpSends) {
    spdlog::error(
        "{} relay unrecoverable error reason={} relay_id={} socket={} pending_quic_receive_bytes={} pending_quic_receive_queue={} pending_quic_sends={} inflight_quic_sends={} inflight_tcp_sends={}",
        backend != nullptr ? backend : "relay",
        reason != nullptr ? reason : "unknown",
        relayId,
        socketOrFd,
        pendingQuicReceiveBytes,
        pendingQuicReceiveQueue,
        pendingQuicSends,
        inflightQuicSends,
        inflightTcpSends);
    if (!TqTraceEnabled()) {
        return;
    }
    char buffer[512];
    std::snprintf(
        buffer,
        sizeof(buffer),
        "event=relay_fatal backend=%s reason=%s relay_id=%llu socket=%llu pending_quic_receive_bytes=%llu pending_quic_receive_queue=%llu pending_quic_sends=%llu inflight_quic_sends=%llu inflight_tcp_sends=%llu",
        backend != nullptr ? backend : "?",
        reason != nullptr ? reason : "unknown",
        static_cast<unsigned long long>(relayId),
        static_cast<unsigned long long>(socketOrFd),
        static_cast<unsigned long long>(pendingQuicReceiveBytes),
        static_cast<unsigned long long>(pendingQuicReceiveQueue),
        static_cast<unsigned long long>(pendingQuicSends),
        static_cast<unsigned long long>(inflightQuicSends),
        static_cast<unsigned long long>(inflightTcpSends));
    LogInfo("%s", buffer);
}

std::string TqFormatRelayMetricsSnapshotLine(const TqRelayMetricsSnapshot& metrics) {
    char buffer[2048];
    std::snprintf(
        buffer,
        sizeof(buffer),
        "backend=%s pending_bytes=%llu active_relays=%llu active_quic_send_relays=%llu "
        "relay_buffer_bytes=%llu tcp_read_bytes=%llu tcp_write_bytes=%llu "
        "quic_send_ops=%llu outstanding_quic_sends=%llu outstanding_quic_send_bytes=%llu "
        "ideal_send_threshold_bytes=%llu tcp_read_disabled_relays=%llu pending_tcp_write_bytes=%llu "
        "hot_relay=%llu hot_worker=%u hot_tcp_read_bytes=%llu hot_tcp_write_bytes=%llu "
        "hot_outstanding_quic_sends=%llu hot_outstanding_quic_send_bytes=%llu "
        "hot_pending_quic_send_retries=%llu hot_ideal_send_bytes=%llu "
        "hot_pending_quic_receive_bytes=%llu hot_tcp_read_armed=%d hot_tcp_write_armed=%d "
        "deferred_receive_complete_bytes=%llu max_pending_quic_receive_bytes=%llu "
        "quic_receive_paused=%llu quic_receive_resumed=%llu quic_send_backpressure=%llu "
        "fatal_relay_resets=%llu tcp_hard_errors=%llu graceful_drains=%llu errors=%llu "
        "event_queue_full_errors=%llu tcp_read_buffer_acquire_failures=%llu "
        "tcp_to_quic_buffer_acquire_failures=%llu quic_send_failures=%llu "
        "quic_send_api_failures=%llu quic_send_fatal_errors=%llu last_quic_send_status=%lld",
        metrics.Backend != nullptr ? metrics.Backend : "?",
        static_cast<unsigned long long>(metrics.PendingBytes),
        static_cast<unsigned long long>(metrics.ActiveRelays),
        static_cast<unsigned long long>(metrics.ActiveQuicSendRelays),
        static_cast<unsigned long long>(metrics.RelayBufferBytesInUse),
        static_cast<unsigned long long>(metrics.TcpReadBytes),
        static_cast<unsigned long long>(metrics.TcpWriteBytes),
        static_cast<unsigned long long>(metrics.QuicSendOperations),
        static_cast<unsigned long long>(metrics.OutstandingQuicSends),
        static_cast<unsigned long long>(metrics.OutstandingQuicSendBytes),
        static_cast<unsigned long long>(metrics.MaxBufferedQuicSendBytes),
        static_cast<unsigned long long>(metrics.TcpReadDisabledRelays),
        static_cast<unsigned long long>(metrics.PendingTcpWriteBytes),
        static_cast<unsigned long long>(metrics.HotRelayId),
        metrics.HotRelayWorkerIndex,
        static_cast<unsigned long long>(metrics.HotRelayTcpReadBytes),
        static_cast<unsigned long long>(metrics.HotRelayTcpWriteBytes),
        static_cast<unsigned long long>(metrics.HotRelayOutstandingQuicSends),
        static_cast<unsigned long long>(metrics.HotRelayOutstandingQuicSendBytes),
        static_cast<unsigned long long>(metrics.HotRelayPendingQuicSendRetries),
        static_cast<unsigned long long>(metrics.HotRelayIdealSendBytes),
        static_cast<unsigned long long>(metrics.HotRelayPendingQuicReceiveBytes),
        metrics.HotRelayTcpReadArmed ? 1 : 0,
        metrics.HotRelayTcpWriteArmed ? 1 : 0,
        static_cast<unsigned long long>(metrics.DeferredReceiveCompleteBytes),
        static_cast<unsigned long long>(metrics.MaxPendingQuicReceiveBytes),
        static_cast<unsigned long long>(metrics.QuicReceivePausedCount),
        static_cast<unsigned long long>(metrics.QuicReceiveResumedCount),
        static_cast<unsigned long long>(metrics.QuicSendBackpressureEvents),
        static_cast<unsigned long long>(metrics.FatalRelayResets),
        static_cast<unsigned long long>(metrics.TcpHardErrors),
        static_cast<unsigned long long>(metrics.GracefulRelayDrains),
        static_cast<unsigned long long>(metrics.Errors),
        static_cast<unsigned long long>(metrics.EventQueueFullErrors),
        static_cast<unsigned long long>(metrics.TcpReadBufferAcquireFailures),
        static_cast<unsigned long long>(metrics.TcpToQuicBufferAcquireFailures),
        static_cast<unsigned long long>(metrics.QuicSendFailures),
        static_cast<unsigned long long>(metrics.QuicSendApiFailures),
        static_cast<unsigned long long>(metrics.QuicSendFatalErrors),
        static_cast<long long>(metrics.LastQuicSendStatus));
    return buffer;
}

void TqTraceLinuxRelayStreamShutdown(const TqTraceLinuxRelayStreamState& state) {
    if (!TqTraceEnabled()) {
        return;
    }
    LogInfo("%s", TqFormatTraceLinuxRelayStreamShutdownLine(state).c_str());
}

void TraceLinuxRelayUnregister(const TqTraceLinuxRelayStreamState& state) {
    if (!TqTraceEnabled()) {
        return;
    }
    LogInfo("%s", FormatTraceLinuxRelayStateLine("linux_relay_unregister", state).c_str());
}

extern "C" void TqTraceLinuxRelayStreamShutdownEvent(
    uint32_t workerIndex,
    uint64_t relayId,
    uint64_t outstandingQuicSends,
    uint64_t outstandingQuicSendBytes,
    uint64_t pendingTcpWriteQueue,
    uint64_t pendingTcpWriteBytes,
    uint64_t pendingQuicReceiveBytes,
    uint64_t tcpReadBytes,
    uint64_t tcpWriteBytes,
    uint64_t tcpWriteErrno,
    bool tcpReadClosed,
    bool tcpWriteClosed,
    bool quicSendFinSubmitted,
    bool quicSendFinCompleted,
    bool tcpWriteShutdownQueued,
    bool streamDetached) {
    TqTraceLinuxRelayStreamShutdown(TqTraceLinuxRelayStreamState{
        workerIndex,
        relayId,
        outstandingQuicSends,
        outstandingQuicSendBytes,
        pendingTcpWriteQueue,
        pendingTcpWriteBytes,
        pendingQuicReceiveBytes,
        tcpReadBytes,
        tcpWriteBytes,
        tcpWriteErrno,
        tcpReadClosed,
        tcpWriteClosed,
        quicSendFinSubmitted,
        quicSendFinCompleted,
        tcpWriteShutdownQueued,
        streamDetached});
}

extern "C" void TqTraceLinuxRelayUnregisterEvent(
    uint32_t workerIndex,
    uint64_t relayId,
    uint64_t outstandingQuicSends,
    uint64_t outstandingQuicSendBytes,
    uint64_t pendingTcpWriteQueue,
    uint64_t pendingTcpWriteBytes,
    uint64_t pendingQuicReceiveBytes,
    uint64_t tcpReadBytes,
    uint64_t tcpWriteBytes,
    uint64_t tcpWriteErrno,
    bool tcpReadClosed,
    bool tcpWriteClosed,
    bool quicSendFinSubmitted,
    bool quicSendFinCompleted,
    bool tcpWriteShutdownQueued,
    bool streamDetached) {
    TraceLinuxRelayUnregister(TqTraceLinuxRelayStreamState{
        workerIndex,
        relayId,
        outstandingQuicSends,
        outstandingQuicSendBytes,
        pendingTcpWriteQueue,
        pendingTcpWriteBytes,
        pendingQuicReceiveBytes,
        tcpReadBytes,
        tcpWriteBytes,
        tcpWriteErrno,
        tcpReadClosed,
        tcpWriteClosed,
        quicSendFinSubmitted,
        quicSendFinCompleted,
        tcpWriteShutdownQueued,
        streamDetached});
}

extern "C" void TqTraceLinuxRelayStopConditionEvent(
    uint32_t workerIndex,
    uint64_t relayId,
    const char* trigger,
    uint64_t outstandingQuicSends,
    uint64_t outstandingQuicSendBytes,
    uint64_t pendingTcpWriteQueue,
    uint64_t pendingTcpWriteBytes,
    uint64_t pendingQuicReceiveBytes,
    uint64_t tcpReadBytes,
    uint64_t tcpWriteBytes,
    uint64_t tcpWriteErrno,
    bool tcpReadClosed,
    bool tcpWriteClosed,
    bool quicSendFinSubmitted,
    bool quicSendFinCompleted,
    bool tcpWriteShutdownQueued,
    bool streamDetached) {
    if (!TqTraceEnabled()) {
        return;
    }
    const TqTraceLinuxRelayStreamState state{
        workerIndex,
        relayId,
        outstandingQuicSends,
        outstandingQuicSendBytes,
        pendingTcpWriteQueue,
        pendingTcpWriteBytes,
        pendingQuicReceiveBytes,
        tcpReadBytes,
        tcpWriteBytes,
        tcpWriteErrno,
        tcpReadClosed,
        tcpWriteClosed,
        quicSendFinSubmitted,
        quicSendFinCompleted,
        tcpWriteShutdownQueued,
        streamDetached};
    LogInfo(
        "%s trigger=%s",
        FormatTraceLinuxRelayStateLine("linux_relay_stop_condition", state).c_str(),
        trigger != nullptr ? trigger : "?");
}

extern "C" void TqTraceLinuxRelayStreamEvent(
    uint32_t workerIndex,
    uint64_t relayId,
    const char* streamEvent,
    uint64_t errorCode,
    uint32_t status,
    uint64_t absoluteOffset,
    uint64_t totalBufferLength,
    uint32_t bufferCount,
    uint32_t receiveFlags,
    bool fin,
    uint64_t outstandingQuicSends,
    uint64_t outstandingQuicSendBytes,
    uint64_t pendingTcpWriteQueue,
    uint64_t pendingTcpWriteBytes,
    uint64_t pendingQuicReceiveBytes,
    uint64_t tcpReadBytes,
    uint64_t tcpWriteBytes,
    uint64_t tcpWriteErrno,
    bool tcpReadClosed,
    bool tcpWriteClosed,
    bool quicSendFinSubmitted,
    bool quicSendFinCompleted,
    bool tcpWriteShutdownQueued,
    bool streamDetached) {
    if (!TqTraceEnabled()) {
        return;
    }
    const TqTraceLinuxRelayStreamState state{
        workerIndex,
        relayId,
        outstandingQuicSends,
        outstandingQuicSendBytes,
        pendingTcpWriteQueue,
        pendingTcpWriteBytes,
        pendingQuicReceiveBytes,
        tcpReadBytes,
        tcpWriteBytes,
        tcpWriteErrno,
        tcpReadClosed,
        tcpWriteClosed,
        quicSendFinSubmitted,
        quicSendFinCompleted,
        tcpWriteShutdownQueued,
        streamDetached};
    LogInfo(
        "%s stream_event=%s error_code=%llu status=0x%x absolute_offset=%llu total_buffer_length=%llu buffer_count=%u receive_flags=0x%x fin=%d",
        FormatTraceLinuxRelayStateLine("linux_relay_stream_event", state).c_str(),
        streamEvent != nullptr ? streamEvent : "?",
        static_cast<unsigned long long>(errorCode),
        status,
        static_cast<unsigned long long>(absoluteOffset),
        static_cast<unsigned long long>(totalBufferLength),
        bufferCount,
        receiveFlags,
        fin ? 1 : 0);
}

extern "C" void TqTraceLinuxRelayBackpressureEvent(
    uint32_t workerIndex,
    uint64_t relayId,
    const char* action,
    const char* reason,
    uint64_t outstandingQuicSendBytes,
    uint64_t pauseThreshold,
    uint64_t resumeThreshold,
    uint64_t readAheadBytes) {
    if (!TqTraceEnabled()) {
        return;
    }
    LogInfo(
        "event=relay_backpressure worker=%u relay=%llu action=%s reason=%s outstanding_quic_send_bytes=%llu pause_threshold=%llu resume_threshold=%llu read_ahead=%llu",
        workerIndex,
        static_cast<unsigned long long>(relayId),
        action != nullptr ? action : "?",
        reason != nullptr ? reason : "?",
        static_cast<unsigned long long>(outstandingQuicSendBytes),
        static_cast<unsigned long long>(pauseThreshold),
        static_cast<unsigned long long>(resumeThreshold),
        static_cast<unsigned long long>(readAheadBytes));
}

extern "C" void TqTraceLinuxRelayIdealSendBufferEvent(
    uint32_t workerIndex,
    uint64_t relayId,
    uint64_t idealSendBytes,
    uint64_t outstandingQuicSendBytes) {
    if (!TqTraceEnabled()) {
        return;
    }
    LogInfo(
        "event=relay_ideal_send_buffer worker=%u relay=%llu ideal_send_bytes=%llu outstanding_quic_send_bytes=%llu",
        workerIndex,
        static_cast<unsigned long long>(relayId),
        static_cast<unsigned long long>(idealSendBytes),
        static_cast<unsigned long long>(outstandingQuicSendBytes));
}

std::string TqFormatTraceNetworkStatsLine(const TqTraceNetworkStats& stats) {
    char buffer[2048];
    const double bandwidthMbps =
        static_cast<double>(stats.BandwidthBytesPerSecond) * 8.0 / 1000000.0;
    std::snprintf(
        buffer,
        sizeof(buffer),
        "net_stats: bbr_bw_bytes_per_sec=%llu bbr_bw_mbps=%.2f bytes_in_flight=%u bytes_in_flight_max=%u posted=%llu ideal=%llu srtt=%.3fms cwnd=%u bbr_state=%u bbr_recovery_state=%u recovery_window=%u pacing_gain=%u cwnd_gain=%u bbr_min_rtt=%.3fms send_quantum=%llu app_limited=%u flush_count=%llu flush_pacing_delayed=%llu flush_cc_blocked=%llu flush_scheduling=%llu flush_amp_blocked=%llu flush_no_work=%llu flush_last_allowance=%u flush_last_path_allowance=%u flush_last_result=%u flush_last_datagrams=%u out_flow_blocked=0x%x loss_events=%llu loss_fack_packets=%llu loss_rack_packets=%llu lost_retransmittable_bytes=%llu loss_last_bytes=%u",
        static_cast<unsigned long long>(stats.BandwidthBytesPerSecond),
        bandwidthMbps,
        stats.BytesInFlight,
        stats.BytesInFlightMax,
        static_cast<unsigned long long>(stats.PostedBytes),
        static_cast<unsigned long long>(stats.IdealBytes),
        static_cast<double>(stats.SmoothedRttUs) / 1000.0,
        stats.CongestionWindow,
        stats.BbrState,
        stats.BbrRecoveryState,
        stats.BbrRecoveryWindow,
        stats.BbrPacingGain,
        stats.BbrCwndGain,
        static_cast<double>(stats.BbrMinRttUs) / 1000.0,
        static_cast<unsigned long long>(stats.BbrSendQuantum),
        stats.BbrAppLimited ? 1u : 0u,
        static_cast<unsigned long long>(stats.SendFlushCount),
        static_cast<unsigned long long>(stats.SendFlushPacingDelayedCount),
        static_cast<unsigned long long>(stats.SendFlushCcBlockedCount),
        static_cast<unsigned long long>(stats.SendFlushSchedulingCount),
        static_cast<unsigned long long>(stats.SendFlushAmplificationBlockedCount),
        static_cast<unsigned long long>(stats.SendFlushNoWorkCount),
        stats.SendFlushLastAllowance,
        stats.SendFlushLastPathAllowance,
        stats.SendFlushLastResult,
        stats.SendFlushLastDatagrams,
        stats.OutFlowBlockedReasons,
        static_cast<unsigned long long>(stats.LossDetectionEventCount),
        static_cast<unsigned long long>(stats.LossDetectionFackPacketCount),
        static_cast<unsigned long long>(stats.LossDetectionRackPacketCount),
        static_cast<unsigned long long>(stats.LostRetransmittableBytes),
        stats.LastLostRetransmittableBytes);
    return buffer;
}

std::string TqTraceGlobalSnapshot() {
    char buffer[512];
    size_t quicConns = 0;
    {
        std::lock_guard<std::mutex> guard(g_stateMu);
        quicConns = g_quicConnsById.size();
    }
    std::snprintf(
        buffer,
        sizeof(buffer),
        "global: quic_conns=%llu streams=%llu relays=%llu socks=%llu http=%llu target_tcp=%llu open_ok=%llu open_fail=%llu open_tx=%llu open_rx=%llu open_ok_tx=%llu open_ok_rx=%llu open_fail_tx=%llu open_fail_rx=%llu",
        static_cast<unsigned long long>(quicConns),
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

void TqTraceLogLine(const char* line) {
    if (!TqTraceEnabled() || line == nullptr || line[0] == '\0') {
        return;
    }
    LogInfo("%s", line);
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

    QUIC_STATISTICS_V2 stats{};
    const bool haveStats = CollectQuicStats(connection, stats);
    const auto lines = haveStats ? TqFormatTraceQuicStatsLines(stats) : std::vector<std::string>{};

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
    bool hasNetStats = false;
    TqTraceNetworkStats netStats;
    bool found = false;
    {
        std::lock_guard<std::mutex> guard(g_stateMu);
        if (auto* entry = FindQuicConnLocked(connId)) {
            slot = entry->slot;
            peerAddr = entry->peerAddr;
            streamsOpened = entry->streamsOpened;
            connectedAt = entry->connectedAt;
            hasNetStats = entry->hasNetStats;
            netStats = entry->netStats;
            found = true;
            g_quicConnsById.erase(connId);
        }
    }

    const auto now = std::chrono::steady_clock::now();
    const double lifetimeSec = found
        ? std::chrono::duration<double>(now - connectedAt).count()
        : 0.0;

    QUIC_STATISTICS_V2 stats{};
    const bool haveStats = CollectQuicStats(connection, stats);
    auto lines = haveStats ? TqFormatTraceQuicStatsLines(stats) : std::vector<std::string>{};
    if (hasNetStats) {
        lines.push_back(TqFormatTraceNetworkStatsLine(netStats));
    }

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

void TqTraceQuicNetworkStats(MsQuicConnection* connection, const TqTraceNetworkStats& stats) {
    if (!TqTraceEnabled() && !TqDiagStatsEnabled()) {
        return;
    }

    if (TqDiagStatsEnabled()) {
        std::lock_guard<std::mutex> guard(g_diagStatsMu);
        g_diagHasNetStats = true;
        g_diagNetStats = stats;
    }

    if (TqTraceEnabled()) {
        std::lock_guard<std::mutex> guard(g_stateMu);
        if (auto* conn = FindQuicConnLocked(connection)) {
            conn->hasNetStats = true;
            conn->netStats = stats;
        }
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

void TqTraceRelayStopping(
    uint64_t tunnelId,
    const char* role,
    const char* target,
    const char* backend,
    uint64_t relayId,
    const char* reason) {
    if (!TqTraceEnabled() || tunnelId == 0) {
        return;
    }

    LogInfo(
        "event=relay_stopping role=%s tunnel=%llu target=%s backend=%s relay_id=%llu reason=%s %s",
        role != nullptr ? role : "?",
        static_cast<unsigned long long>(tunnelId),
        target != nullptr ? target : "?",
        backend != nullptr ? backend : "none",
        static_cast<unsigned long long>(relayId),
        reason != nullptr ? reason : "?",
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

void TqTraceProxyRejected(TqTraceProxyProto proto, TqSocketHandle fd, int status, const char* reason) {
    if (!TqTraceEnabled()) {
        return;
    }

    std::string peer;
    if (!TqFormatSocketPeerAddr(fd, peer)) {
        peer = "?";
    }

    LogInfo(
        "event=proxy_rejected proto=%s fd=%d peer=%s status=%d reason=%s %s",
        ProxyProtoName(proto),
        static_cast<int>(fd),
        peer.c_str(),
        status,
        reason != nullptr ? reason : "?",
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
