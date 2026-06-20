#include "trace.h"

#include "msquic.hpp"
#include "relay_metrics.h"

#include <string>

const MsQuicApi* MsQuic = nullptr;

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
    quicStats.SendPathMtu = 1500;
    const std::vector<std::string> mtuQuicLines = TqFormatTraceQuicStatsLines(quicStats);
    bool foundMtuStats = false;
    for (const std::string& quicLine : mtuQuicLines) {
        if (quicLine.find("congestion_events=") == std::string::npos) {
            continue;
        }
        foundMtuStats = true;
        if (quicLine.find("mtu=1500") == std::string::npos ||
            quicLine.find("udp_payload_ipv4=1472") == std::string::npos ||
            quicLine.find("udp_payload_ipv6=1452") == std::string::npos) {
            return 16;
        }
    }
    if (!foundMtuStats) {
        return 17;
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
        10000000});

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

    const std::string relayLine = TqFormatTraceLinuxRelayStreamShutdownLine(
        TqTraceLinuxRelayStreamState{
            7,
            42,
            3,
            65536,
            2,
            1048576,
            131072,
            73400320,
            59873689,
            0,
            true,
            false,
            true,
            false,
            true,
            false});

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

    const std::string windowsRelayLine = TqFormatTraceRelayStateLine(
        "relay_stream_shutdown",
        "windows",
        TqTraceLinuxRelayStreamState{
            0,
            99,
            1,
            4096,
            0,
            0,
            8192,
            0,
            0,
            0,
            false,
            false,
            false,
            false,
            false,
            true});
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

    return 0;
}
