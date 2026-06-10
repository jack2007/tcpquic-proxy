#include "config.h"
#include "tuning.h"

#include <cassert>
#include <cstring>

int main() {
    {
        TqConfig cfg{};
        cfg.TuningMode = TqTuningMode::Wan;
        TqComputeTuning(cfg, cfg.Tuning);

        assert(cfg.Tuning.StreamRecvWindow == 536870912u);
        assert(cfg.Tuning.ConnFlowControlWindow == 500000000u);
        assert(cfg.Tuning.InitialWindowPackets == 2000);
        assert(cfg.Tuning.InitialRttMs == 100);
        assert(cfg.Tuning.RelayIoSize == 1024 * 1024);
        assert(cfg.Tuning.RelayMaxInFlightSends == 64);
        assert(cfg.Tuning.TcpSocketBufferBytes == 4 * 1024 * 1024);
    }

    {
        TqConfig cfg{};
        cfg.TuningMode = TqTuningMode::Lan;
        TqComputeTuning(cfg, cfg.Tuning);

        assert(cfg.Tuning.StreamRecvWindow == 16u * 1024 * 1024);
        assert(cfg.Tuning.RelayIoSize == 256 * 1024);
        assert(cfg.Tuning.RelayMaxInFlightSends == 16);
    }

    {
        TqConfig cfg{};
        cfg.TuningMode = TqTuningMode::Auto;
        cfg.TargetBandwidthMbps = 100;
        cfg.TargetRttMs = 100;
        TqComputeTuning(cfg, cfg.Tuning);

        assert(cfg.Tuning.StreamRecvWindow >= 16u * 1024 * 1024);
        assert(cfg.Tuning.ConnFlowControlWindow >= 16u * 1024 * 1024);
        assert(cfg.Tuning.RelayIoSize == 256 * 1024);
        assert(cfg.Tuning.InitialRttMs == 100);
    }

    {
        TqConfig cfg{};
        cfg.TuningMode = TqTuningMode::Auto;
        cfg.TargetBandwidthMbps = 10000;
        cfg.TargetRttMs = 100;
        TqComputeTuning(cfg, cfg.Tuning);

        assert(cfg.Tuning.StreamRecvWindow >= 128u * 1024 * 1024);
        assert(cfg.Tuning.RelayIoSize == 1024 * 1024);
        assert(cfg.Tuning.RelayMaxInFlightSends == 64);
    }

    {
        TqConfig cfg{};
        cfg.TuningMode = TqTuningMode::Custom;
        cfg.TargetBandwidthMbps = 1000;
        cfg.TargetRttMs = 50;
        cfg.TuningOverrideRelayIoSize = 512 * 1024;
        cfg.TuningOverrideQuicInitRttMs = 50;
        TqComputeTuning(cfg, cfg.Tuning);

        assert(cfg.Tuning.RelayIoSize == 512 * 1024);
        assert(cfg.Tuning.InitialRttMs == 50);
    }

    {
        TqConfig cfg{};
        cfg.TuningMode = TqTuningMode::Wan;
        TqComputeTuning(cfg, cfg.Tuning);

        TqSetRelayMemoryBudget(512);
        TqApplyRelayPoolBudget(cfg.Tuning, 100);
        assert(cfg.Tuning.RelayMaxFreeSendContexts == 5);
        assert(cfg.Tuning.RelayMaxInFlightSends == 5);
        assert(cfg.Tuning.RelayDefaultIdealSend == 5ull * 1024 * 1024);

        TqComputeTuning(cfg, cfg.Tuning);
        TqApplyRelayPoolBudget(cfg.Tuning, 1);
        assert(cfg.Tuning.RelayMaxFreeSendContexts == 64);

        TqSetRelayMemoryBudget(0);
    }

    {
        TqSetRelayMemoryBudget(128);
        TqConfig cfg{};
        cfg.TuningMode = TqTuningMode::Wan;
        TqComputeTuning(cfg, cfg.Tuning);

        const uint32_t first = TqRelayRegisterActive();
        assert(first == 1);
        TqApplyRelayPoolBudget(cfg.Tuning, first);
        assert(cfg.Tuning.RelayMaxFreeSendContexts >= 1);

        TqRelayUnregisterActive();
        assert(TqGetActiveRelayCount() == 0);
        TqSetRelayMemoryBudget(0);
    }

    {
        TqResetRuntimeObservations();
        TqConfig cfg{};
        cfg.TuningMode = TqTuningMode::Wan;
        TqComputeTuning(cfg, cfg.Tuning);
        const uint32_t baselineRtt = cfg.Tuning.InitialRttMs;

        TqRecordMeasuredRtt(120);
        TqApplyRuntimeObservations(cfg);
        assert(cfg.Tuning.InitialRttMs == 120);
        assert(TqRuntimeTuningEnabled(cfg));

        TqRecordRelayThroughput(10 * 1024 * 1024, 1000, 8 * 1024 * 1024, 4);
        cfg.Tuning.InitialRttMs = baselineRtt;
        cfg.TuningMode = TqTuningMode::Auto;
        TqComputeTuning(cfg, cfg.Tuning);
        TqApplyRuntimeObservations(cfg);
        assert(cfg.Tuning.InitialRttMs == 120);
        assert(TqGetRuntimeObservations().HasThroughput);

        cfg.TuningMode = TqTuningMode::Lan;
        assert(!TqRuntimeTuningEnabled(cfg));
        TqResetRuntimeObservations();
    }

    {
        TqResetCompressionObservations();
        TqConfig cfg{};
        cfg.Compress = "auto";
        assert(std::strcmp(TqResolveAutoCompress(cfg), "lz4") == 0);

        TqRecordCompressionSample(10000, 9900);
        assert(std::strcmp(TqResolveAutoCompress(cfg), "off") == 0);

        TqResetCompressionObservations();
        TqRecordCompressionSample(10000, 8500);
        assert(std::strcmp(TqResolveAutoCompress(cfg), "lz4") == 0);

        TqResetCompressionObservations();
        TqRecordCompressionSample(10000, 5000);
        assert(std::strcmp(TqResolveAutoCompress(cfg), "zstd") == 0);
        assert(TqCompressionAdaptiveEnabled(cfg));
        TqResetCompressionObservations();
    }

    {
        TqConfig cfg{};
        std::string err;
        char arg0[] = "tcpquic-proxy";
        char arg1[] = "client";
        char arg2[] = "--quic-peer";
        char arg3[] = "127.0.0.1:4433";
        char arg4[] = "--quic-cert";
        char arg5[] = "cert.pem";
        char arg6[] = "--quic-key";
        char arg7[] = "key.pem";
        char arg8[] = "--quic-ca";
        char arg9[] = "ca.pem";
        char* argv[] = {arg0, arg1, arg2, arg3, arg4, arg5, arg6, arg7, arg8, arg9};
        assert(TqParseArgs(10, argv, cfg, err));
        assert(cfg.QuicProfile == TqQuicProfile::MaxThroughput);
    }

    {
        TqConfig cfg{};
        std::string err;
        char arg0[] = "tcpquic-proxy";
        char arg1[] = "client";
        char arg2[] = "--quic-profile";
        char arg3[] = "low-latency";
        char arg4[] = "--quic-peer";
        char arg5[] = "127.0.0.1:4433";
        char arg6[] = "--quic-cert";
        char arg7[] = "cert.pem";
        char arg8[] = "--quic-key";
        char arg9[] = "key.pem";
        char arg10[] = "--quic-ca";
        char arg11[] = "ca.pem";
        char* argv[] = {arg0, arg1, arg2, arg3, arg4, arg5, arg6, arg7, arg8, arg9, arg10, arg11};
        assert(TqParseArgs(12, argv, cfg, err));
        assert(cfg.QuicProfile == TqQuicProfile::LowLatency);
    }

    {
        TqConfig cfg{};
        std::string err;
        char arg0[] = "tcpquic-proxy";
        char arg1[] = "client";
        char arg2[] = "--quic-profile";
        char arg3[] = "invalid";
        char arg4[] = "--quic-peer";
        char arg5[] = "127.0.0.1:4433";
        char arg6[] = "--quic-cert";
        char arg7[] = "cert.pem";
        char arg8[] = "--quic-key";
        char arg9[] = "key.pem";
        char arg10[] = "--quic-ca";
        char arg11[] = "ca.pem";
        char* argv[] = {arg0, arg1, arg2, arg3, arg4, arg5, arg6, arg7, arg8, arg9, arg10, arg11};
        assert(!TqParseArgs(12, argv, cfg, err));
    }

    {
        TqSetRelayMemoryBudget(512);
        TqConfig cfg{};
        cfg.TuningMode = TqTuningMode::Wan;
        TqComputeTuning(cfg, cfg.Tuning);

        assert(cfg.Tuning.LinuxRelayWorkerCount >= 1);
        assert(cfg.Tuning.LinuxRelayMaxIov == 16);
        assert(cfg.Tuning.LinuxRelayReadChunkSize == 64 * 1024);
        assert(cfg.Tuning.LinuxRelayReadBatchBytes == 1024 * 1024);
        assert(cfg.Tuning.LinuxRelayWorkerEventBudget == 4096);
        assert(cfg.Tuning.LinuxRelayWorkerByteBudgetPerTick == 64u * 1024 * 1024);
        assert(cfg.Tuning.LinuxRelayGlobalPendingBytes == 512ull * 1024 * 1024 / 2);
        assert(cfg.Tuning.LinuxRelayPerTunnelPendingBytes == 4u * 1024 * 1024);
        assert(cfg.Tuning.LinuxRelayPerWorkerPendingBytes >= cfg.Tuning.LinuxRelayPerTunnelPendingBytes);

        TqSetRelayMemoryBudget(0);
    }

    {
        TqConfig cfg{};
        cfg.TuningMode = TqTuningMode::Lan;
        TqComputeTuning(cfg, cfg.Tuning);

        assert(cfg.Tuning.LinuxRelayMaxIov == 8);
        assert(cfg.Tuning.LinuxRelayReadChunkSize == 32 * 1024);
        assert(cfg.Tuning.LinuxRelayReadBatchBytes == 256 * 1024);
        assert(cfg.Tuning.LinuxRelayWorkerEventBudget == 1024);
        assert(cfg.Tuning.LinuxRelayWorkerByteBudgetPerTick == 16u * 1024 * 1024);
    }

    return 0;
}
