#include "trace.h"

#include "exe_path.h"
#include "msquic.hpp"
#include "relay_metrics.h"

#include <cstdio>
#include <fstream>
#include <string>

const MsQuicApi* MsQuic = nullptr;

std::string ReadFile(const char* path) {
    std::ifstream input(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
}

std::string ClientTraceLogPath() {
    std::string exeDir;
    if (!TqGetExecutableDirectory(exeDir)) {
        exeDir = ".";
    }
    return exeDir + "/log/client.log";
}

bool LogContainsRelayBackpressure(TqConfig::TraceLevel level) {
    const std::string logPath = ClientTraceLogPath();
    std::remove(logPath.c_str());
    if (!TqTraceInit(TqMode::Client, 0, level)) {
        return true;
    }
    TqTraceRelayBackpressureEvent("linux", 3, 77, "pause", "threshold", 4096, 2048, 1024, 8192);
    TqTraceShutdown();
    return ReadFile(logPath.c_str()).find("event=relay_backpressure") != std::string::npos;
}

TqTraceLinuxRelayStreamState MakeRelayStateForFormattingTest() {
    TqTraceLinuxRelayStreamState state{};
    state.WorkerIndex = 7;
    state.RelayId = 42;
    state.OutstandingQuicSends = 3;
    state.OutstandingQuicSendBytes = 65536;
    state.PendingTcpWriteQueue = 2;
    state.PendingTcpWriteBytes = 1048576;
    state.PendingQuicReceiveBytes = 131072;
    state.TcpReadBytes = 73400320;
    state.TcpWriteBytes = 59873689;
    state.TcpWriteErrno = 0;
    state.TcpReadClosed = true;
    state.TcpWriteClosed = false;
    state.QuicSendFinSubmitted = true;
    state.QuicSendFinCompleted = false;
    state.TcpWriteShutdownQueued = true;
    state.StreamDetached = false;
    return state;
}

int main() {
    QUIC_STATISTICS_V2 quicStats{};
    quicStats.SendTotalPackets = 300;
    quicStats.RecvTotalPackets = 200;
    quicStats.SendSuspectedLostPackets = 100;
    quicStats.SendSpuriousLostPackets = 7;
    quicStats.RecvReorderedPackets = 5;
    quicStats.RecvDecryptionFailures = 3;
    const std::vector<std::string> quicLines = TqFormatTraceQuicStatsLines(quicStats);
    bool foundPacketStats = false;
    for (const std::string& quicLine : quicLines) {
        if (quicLine.find("pkts:") == std::string::npos) {
            continue;
        }
        foundPacketStats = true;
        if (quicLine.find("tx=300") == std::string::npos ||
            quicLine.find("lost=100") == std::string::npos ||
            quicLine.find("loss_rate=33.33%") == std::string::npos) {
            return 12;
        }
    }
    if (!foundPacketStats) {
        return 13;
    }

    QUIC_STATISTICS_V2 zeroPacketStats{};
    const std::vector<std::string> zeroQuicLines = TqFormatTraceQuicStatsLines(zeroPacketStats);
    bool foundZeroPacketStats = false;
    for (const std::string& quicLine : zeroQuicLines) {
        if (quicLine.find("pkts:") == std::string::npos) {
            continue;
        }
        foundZeroPacketStats = true;
        if (quicLine.find("loss_rate=0.00%") == std::string::npos) {
            return 14;
        }
    }
    if (!foundZeroPacketStats) {
        return 15;
    }

    const std::string line = TqFormatTraceNetworkStatsLine(TqTraceNetworkStats{
        1234,
        5678,
        9012,
        70000,
        64000,
        10000000,
        2222,
        2,
        2,
        4096,
        320,
        512,
        200000,
        65536,
        false,
        10,
        3,
        2,
        1,
        4,
        5,
        65536,
        131072,
        6,
        7,
        8,
        9,
        10,
        11,
        123456,
        4096});

    if (line.find("net_stats:") == std::string::npos) {
        return 1;
    }
    if (line.find("bbr_bw_bytes_per_sec=10000000") == std::string::npos) {
        return 2;
    }
    if (line.find("bbr_bw_mbps=80.00") == std::string::npos) {
        return 3;
    }
    if (line.find("bytes_in_flight=1234") == std::string::npos ||
        line.find("posted=5678") == std::string::npos ||
        line.find("ideal=9012") == std::string::npos ||
        line.find("srtt=70.000ms") == std::string::npos ||
        line.find("cwnd=64000") == std::string::npos) {
        return 4;
    }
    if (line.find("bytes_in_flight_max=2222") == std::string::npos) {
        return 16;
    }
    if (line.find("flush_count=10") == std::string::npos ||
        line.find("flush_pacing_delayed=3") == std::string::npos ||
        line.find("flush_cc_blocked=2") == std::string::npos ||
        line.find("flush_scheduling=1") == std::string::npos ||
        line.find("flush_amp_blocked=4") == std::string::npos ||
        line.find("flush_no_work=5") == std::string::npos ||
        line.find("flush_last_allowance=65536") == std::string::npos ||
        line.find("flush_last_path_allowance=131072") == std::string::npos ||
        line.find("flush_last_result=6") == std::string::npos ||
        line.find("flush_last_datagrams=7") == std::string::npos ||
        line.find("out_flow_blocked=0x8") == std::string::npos ||
        line.find("loss_events=9") == std::string::npos ||
        line.find("loss_fack_packets=10") == std::string::npos ||
        line.find("loss_rack_packets=11") == std::string::npos ||
        line.find("lost_retransmittable_bytes=123456") == std::string::npos ||
        line.find("loss_last_bytes=4096") == std::string::npos) {
        return 17;
    }

    const std::string relayLine = TqFormatTraceLinuxRelayStreamShutdownLine(
        MakeRelayStateForFormattingTest());

    if (relayLine.find("event=linux_relay_stream_shutdown") == std::string::npos) {
        return 5;
    }
    if (relayLine.find("worker=7") == std::string::npos ||
        relayLine.find("relay=42") == std::string::npos ||
        relayLine.find("outstanding_quic_sends=3") == std::string::npos ||
        relayLine.find("outstanding_quic_send_bytes=65536") == std::string::npos ||
        relayLine.find("pending_tcp_write_queue=2") == std::string::npos ||
        relayLine.find("pending_tcp_write_bytes=1048576") == std::string::npos ||
        relayLine.find("pending_quic_receive_bytes=131072") == std::string::npos ||
        relayLine.find("tcp_read_bytes=73400320") == std::string::npos ||
        relayLine.find("tcp_write_bytes=59873689") == std::string::npos) {
        return 6;
    }
    if (relayLine.find("tcp_write_errno=0") == std::string::npos) {
        return 8;
    }
    if (relayLine.find("tcp_read_closed=1") == std::string::npos ||
        relayLine.find("tcp_write_closed=0") == std::string::npos ||
        relayLine.find("quic_send_fin_submitted=1") == std::string::npos ||
        relayLine.find("quic_send_fin_completed=0") == std::string::npos ||
        relayLine.find("tcp_write_shutdown_queued=1") == std::string::npos ||
        relayLine.find("stream_detached=0") == std::string::npos) {
        return 7;
    }

    TqTraceLinuxRelayStreamState windowsRelayState{};
    windowsRelayState.RelayId = 99;
    windowsRelayState.OutstandingQuicSends = 1;
    windowsRelayState.OutstandingQuicSendBytes = 4096;
    windowsRelayState.PendingQuicReceiveBytes = 8192;
    windowsRelayState.StreamDetached = true;
    const std::string windowsRelayLine = TqFormatTraceRelayStateLine(
        "relay_stream_shutdown",
        "windows",
        windowsRelayState);
    if (windowsRelayLine.find("event=relay_stream_shutdown") == std::string::npos) {
        return 9;
    }
    if (windowsRelayLine.find("backend=windows") == std::string::npos ||
        windowsRelayLine.find("relay=99") == std::string::npos) {
        return 10;
    }

    TqRelayMetricsSnapshot metrics{};
    metrics.Backend = "worker";
    metrics.PendingBytes = 12345;
    metrics.ActiveRelays = 3;
    metrics.FatalRelayResets = 1;
    const std::string metricsLine = TqFormatRelayMetricsSnapshotLine(metrics);
    if (metricsLine.find("backend=worker") == std::string::npos ||
        metricsLine.find("pending_bytes=12345") == std::string::npos ||
        metricsLine.find("active_relays=3") == std::string::npos ||
        metricsLine.find("fatal_relay_resets=1") == std::string::npos) {
        return 11;
    }
    if (LogContainsRelayBackpressure(TqConfig::TraceLevel::Info)) {
        return 18;
    }
    if (!LogContainsRelayBackpressure(TqConfig::TraceLevel::Debug)) {
        return 19;
    }

    return 0;
}
