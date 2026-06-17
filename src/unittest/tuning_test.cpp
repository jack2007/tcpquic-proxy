#include "config.h"
#include "tuning.h"

#include <cassert>
#include <cstring>

#define TQ_TEST_REQUIRE(expr) \
    do { \
        if (!(expr)) { \
            return __LINE__; \
        } \
    } while (false)

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
        assert(cfg.Tuning.WindowsRelayMaxPendingQuicReceiveBytesPerRelay == 16ull * 1024 * 1024);
        assert(cfg.Tuning.WindowsRelayQuicReceiveCompleteBatchBytes == 0);
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
        cfg.TuningOverrideLinuxRelayReadChunkSize = 256 * 1024;
        cfg.TuningOverrideQuicInitRttMs = 50;
        TqComputeTuning(cfg, cfg.Tuning);

        assert(cfg.Tuning.RelayIoSize == 512 * 1024);
        assert(cfg.Tuning.LinuxRelayReadChunkSize == 256 * 1024);
        assert(cfg.Tuning.InitialRttMs == 50);
    }

    {
        TqConfig cfg{};
        cfg.TuningMode = TqTuningMode::Custom;
        cfg.TargetBandwidthMbps = 11000;
        cfg.TargetRttMs = 200;
        cfg.TuningOverrideRelayIoSize = 1024 * 1024;
        cfg.TuningOverrideLinuxRelayReadChunkSize = 131072;
        cfg.TuningOverrideQuicInitRttMs = 100;
        TqComputeTuning(cfg, cfg.Tuning);

        assert(cfg.Tuning.LinuxRelayReadChunkSize == 131072);
        assert(cfg.Tuning.LinuxRelayPerTunnelPendingBytes == 500ull * 1024 * 1024);
        assert(cfg.Tuning.InitialRttMs == 100);
    }

    {
        TqConfig cfg{};
        cfg.TuningMode = TqTuningMode::Custom;
        cfg.TargetBandwidthMbps = 11000;
        cfg.TargetRttMs = 200;
        cfg.TuningOverrideRelayIoSize = 1024 * 1024;
        cfg.TuningOverrideQuicInitRttMs = 200;
        TqComputeTuning(cfg, cfg.Tuning);

        assert(cfg.Tuning.RelayMaxInFlightSends == 64);
        assert(cfg.Tuning.RelayDefaultIdealSend == 500ull * 1024 * 1024);
        assert(cfg.Tuning.LinuxRelayPerTunnelPendingBytes == 500ull * 1024 * 1024);
        assert(cfg.Tuning.TcpSocketBufferBytes >= 16 * 1024 * 1024);
    }

    {
        TqSetRelayMemoryBudget(0);
        TqConfig cfg{};
        cfg.TuningMode = TqTuningMode::Auto;
        cfg.TargetBandwidthMbps = 1000;
        cfg.TargetRttMs = 50;
        TqComputeTuning(cfg, cfg.Tuning);

        TQ_TEST_REQUIRE(TqGetRelayMemoryBudget() >= 256);
        TQ_TEST_REQUIRE(cfg.Tuning.LinuxRelayPerTunnelPendingBytes >= 16ull * 1024 * 1024);
    }

    {
        TqSetRelayMemoryBudget(0);
        TqConfig cfg{};
        cfg.TuningMode = TqTuningMode::Auto;
        cfg.TargetBandwidthMbps = 20000;
        cfg.TargetRttMs = 100;
        TqComputeTuning(cfg, cfg.Tuning);

        TQ_TEST_REQUIRE(cfg.Tuning.LinuxRelayPerTunnelPendingBytes == 500000000ull);
        TQ_TEST_REQUIRE(cfg.Tuning.MaxPendingBufferBytesPerRelay >= 500000000ull);
        TQ_TEST_REQUIRE(TqGetRelayMemoryBudget() >= 512);
    }

    {
        TqConfig cfg{};
        cfg.TuningMode = TqTuningMode::Auto;
        cfg.TargetBandwidthMbps = 1073741824u;
        cfg.TargetRttMs = 2147483648u;
        TqComputeTuning(cfg, cfg.Tuning);

        TQ_TEST_REQUIRE(cfg.Tuning.ConnFlowControlWindow == 500000000u);
        TQ_TEST_REQUIRE(cfg.Tuning.RelayDefaultIdealSend == 500ull * 1024 * 1024);
        TQ_TEST_REQUIRE(cfg.Tuning.LinuxRelayPerTunnelPendingBytes == 500ull * 1024 * 1024);
    }

    {
        TqConfig cfg{};
        cfg.TuningMode = TqTuningMode::Wan;
        TqComputeTuning(cfg, cfg.Tuning);

        const uint32_t autoBudgetMb = TqGetRelayMemoryBudget();
        TQ_TEST_REQUIRE(autoBudgetMb >= 256);
        TqApplyRelayPoolBudget(cfg.Tuning, 100);
        TQ_TEST_REQUIRE(cfg.Tuning.RelayMaxFreeSendContexts >= 1);
        TQ_TEST_REQUIRE(cfg.Tuning.RelayMaxInFlightSends >= 1);
        TQ_TEST_REQUIRE(cfg.Tuning.RelayDefaultIdealSend <=
                        static_cast<uint64_t>(autoBudgetMb) * 1024 * 1024 / 100);

        TqComputeTuning(cfg, cfg.Tuning);
        TqApplyRelayPoolBudget(cfg.Tuning, 1);
        TQ_TEST_REQUIRE(cfg.Tuning.RelayMaxFreeSendContexts == 64);
    }

    {
        TqConfig cfg{};
        cfg.TuningMode = TqTuningMode::Wan;
        TqComputeTuning(cfg, cfg.Tuning);

        const uint32_t first = TqRelayRegisterActive();
        TQ_TEST_REQUIRE(first == 1);
        TqApplyRelayPoolBudget(cfg.Tuning, first);
        TQ_TEST_REQUIRE(cfg.Tuning.RelayMaxFreeSendContexts >= 1);

        TqRelayUnregisterActive();
        TQ_TEST_REQUIRE(TqGetActiveRelayCount() == 0);
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
        assert(std::strcmp(TqResolveAutoCompress(cfg), "off") == 0);

        TqRecordCompressionSample(10000, 9900);
        assert(std::strcmp(TqResolveAutoCompress(cfg), "off") == 0);

        TqResetCompressionObservations();
        TqRecordCompressionSample(10000, 8500);
        assert(std::strcmp(TqResolveAutoCompress(cfg), "zstd") == 0);

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
        char arg2[] = "--peer";
        char arg3[] = "127.0.0.1:4433";
        char arg4[] = "--cert";
        char arg5[] = "cert.pem";
        char arg6[] = "--key";
        char arg7[] = "key.pem";
        char arg8[] = "--ca";
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
        char arg2[] = "--enable-encrypt";
        char arg3[] = "--peer";
        char arg4[] = "127.0.0.1:4433";
        char arg5[] = "--cert";
        char arg6[] = "cert.pem";
        char arg7[] = "--key";
        char arg8[] = "key.pem";
        char arg9[] = "--ca";
        char arg10[] = "ca.pem";
        char* argv[] = {arg0, arg1, arg2, arg3, arg4, arg5, arg6, arg7, arg8, arg9, arg10};
        if (!TqParseArgs(11, argv, cfg, err) || cfg.QuicDisable1RttEncryption) {
            return 2;
        }
    }

    {
        TqConfig cfg{};
        std::string err;
        char arg0[] = "tcpquic-proxy";
        char arg1[] = "client";
        char arg2[] = "--profile";
        char arg3[] = "low-latency";
        char arg4[] = "--peer";
        char arg5[] = "127.0.0.1:4433";
        char arg6[] = "--cert";
        char arg7[] = "cert.pem";
        char arg8[] = "--key";
        char arg9[] = "key.pem";
        char arg10[] = "--ca";
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
        char arg2[] = "--profile";
        char arg3[] = "invalid";
        char arg4[] = "--peer";
        char arg5[] = "127.0.0.1:4433";
        char arg6[] = "--cert";
        char arg7[] = "cert.pem";
        char arg8[] = "--key";
        char arg9[] = "key.pem";
        char arg10[] = "--ca";
        char arg11[] = "ca.pem";
        char* argv[] = {arg0, arg1, arg2, arg3, arg4, arg5, arg6, arg7, arg8, arg9, arg10, arg11};
        assert(!TqParseArgs(12, argv, cfg, err));
    }

    {
        TqConfig cfg{};
        std::string err;
        char arg0[] = "tcpquic-proxy";
        char arg1[] = "client";
        char arg2[] = "--peer";
        char arg3[] = "127.0.0.1:4433";
        char arg4[] = "--cert";
        char arg5[] = "cert.pem";
        char arg6[] = "--key";
        char arg7[] = "key.pem";
        char arg8[] = "--ca";
        char arg9[] = "ca.pem";
        char arg10[] = "--linux-relay-read-chunk-size";
        char arg11[] = "262144";
        char* argv[] = {
            arg0, arg1, arg2, arg3, arg4, arg5, arg6, arg7,
            arg8, arg9, arg10, arg11};
        assert(TqParseArgs(12, argv, cfg, err));
        TqFinalizeConfig(cfg);
        assert(cfg.Tuning.LinuxRelayReadChunkSize == 262144);
    }

    {
        TqConfig cfg{};
        std::string err;
        char arg0[] = "tcpquic-proxy";
        char arg1[] = "client";
        char arg2[] = "--peer";
        char arg3[] = "127.0.0.1:4433";
        char arg4[] = "--cert";
        char arg5[] = "cert.pem";
        char arg6[] = "--key";
        char arg7[] = "key.pem";
        char arg8[] = "--ca";
        char arg9[] = "ca.pem";
        char arg10[] = "--linux-relay-tcp-write-max-bytes";
        char arg11[] = "4194304";
        char arg12[] = "--linux-relay-tcp-write-burst-bytes";
        char arg13[] = "16777216";
        char* argv[] = {
            arg0, arg1, arg2, arg3, arg4, arg5, arg6, arg7,
            arg8, arg9, arg10, arg11, arg12, arg13};
        assert(TqParseArgs(14, argv, cfg, err));
        TqFinalizeConfig(cfg);
        assert(cfg.Tuning.LinuxRelayTcpWriteMaxBytes == 4194304);
        assert(cfg.Tuning.LinuxRelayTcpWriteBurstBytes == 16777216);
    }

    {
        TqConfig cfg{};
        std::string err;
        char arg0[] = "tcpquic-proxy";
        char arg1[] = "server";
        char arg2[] = "--listen";
        char arg3[] = "127.0.0.1:4433";
        char arg4[] = "--cert";
        char arg5[] = "cert.pem";
        char arg6[] = "--key";
        char arg7[] = "key.pem";
        char arg8[] = "--ca";
        char arg9[] = "ca.pem";
        char arg10[] = "--allow-targets";
        char arg11[] = "127.0.0.0/8";
        char arg12[] = "--linux-relay-read-chunk-size";
        char arg13[] = "131072";
        char* argv[] = {
            arg0, arg1, arg2, arg3, arg4, arg5, arg6, arg7, arg8,
            arg9, arg10, arg11, arg12, arg13};
        assert(TqParseArgs(14, argv, cfg, err));
        TqFinalizeConfig(cfg);
        assert(cfg.Tuning.LinuxRelayReadChunkSize == 131072);
    }

    {
        TqConfig cfg{};
        std::string err;
        char arg0[] = "tcpquic-proxy";
        char arg1[] = "client";
        char arg2[] = "--peer";
        char arg3[] = "127.0.0.1:4433";
        char arg4[] = "--cert";
        char arg5[] = "cert.pem";
        char arg6[] = "--key";
        char arg7[] = "key.pem";
        char arg8[] = "--ca";
        char arg9[] = "ca.pem";
        char arg10[] = "--linux-relay-ingress-slots";
        char arg11[] = "96";
        char* argv[] = {
            arg0, arg1, arg2, arg3, arg4, arg5, arg6, arg7, arg8, arg9, arg10, arg11};
        assert(!TqParseArgs(12, argv, cfg, err));
        assert(err.find("unsupported option") != std::string::npos);
    }

    {
        TqSetRelayMemoryBudget(0);
        TqConfig cfg{};
        cfg.TuningMode = TqTuningMode::Wan;
        TqComputeTuning(cfg, cfg.Tuning);
        const uint32_t autoBudgetMb = TqGetRelayMemoryBudget();

        assert(cfg.Tuning.LinuxRelayWorkerCount >= 1);
        assert(cfg.Tuning.LinuxRelayMaxIov == 16);
        assert(cfg.Tuning.LinuxRelayReadChunkSize == 128 * 1024);
        assert(cfg.Tuning.LinuxRelayReadBatchBytes == 1024 * 1024);
        assert(cfg.Tuning.LinuxRelayWorkerEventBudget == 4096);
        assert(cfg.Tuning.LinuxRelayWorkerByteBudgetPerTick == 64u * 1024 * 1024);
        TQ_TEST_REQUIRE(autoBudgetMb >= 256);
        TQ_TEST_REQUIRE(cfg.Tuning.LinuxRelayGlobalPendingBytes ==
                        static_cast<uint64_t>(autoBudgetMb) * 1024 * 1024 / 2);
        TQ_TEST_REQUIRE(cfg.Tuning.LinuxRelayPerTunnelPendingBytes == 64ull * 1024 * 1024);
        if (cfg.Tuning.LinuxRelayQuicReceiveCompleteBatchBytes != 0) {
            return 1;
        }
        TQ_TEST_REQUIRE(
            cfg.Tuning.MaxPendingBufferBytesPerRelay >= cfg.Tuning.LinuxRelayPerTunnelPendingBytes);
    }

    {
        TqConfig cfg{};
        cfg.TuningMode = TqTuningMode::Lan;
        TqComputeTuning(cfg, cfg.Tuning);

        assert(cfg.Tuning.LinuxRelayMaxIov == 8);
        assert(cfg.Tuning.LinuxRelayReadChunkSize == 128 * 1024);
        assert(cfg.Tuning.LinuxRelayReadBatchBytes == 256 * 1024);
        assert(cfg.Tuning.LinuxRelayWorkerEventBudget == 1024);
        assert(cfg.Tuning.LinuxRelayWorkerByteBudgetPerTick == 16u * 1024 * 1024);
    }

    {
        TqConfig cfg{};
        std::string err;
        char arg0[] = "tcpquic-proxy";
        char arg1[] = "client";
        char arg2[] = "--download-test";
        char arg3[] = "10";
        char arg4[] = "--peer";
        char arg5[] = "127.0.0.1:4433";
        char arg6[] = "--cert";
        char arg7[] = "cert.pem";
        char arg8[] = "--key";
        char arg9[] = "key.pem";
        char arg10[] = "--ca";
        char arg11[] = "ca.pem";
        char* argv[] = {arg0, arg1, arg2, arg3, arg4, arg5, arg6, arg7, arg8, arg9, arg10, arg11};
        assert(TqParseArgs(12, argv, cfg, err));
        assert(cfg.SpeedTestMode == TqSpeedTestMode::Download);
        assert(cfg.SpeedTestDurationSec == 10);
    }

    {
        TqConfig cfg{};
        std::string err;
        char arg0[] = "tcpquic-proxy";
        char arg1[] = "client";
        char arg2[] = "--upload-test";
        char arg3[] = "5";
        char arg4[] = "--peer";
        char arg5[] = "127.0.0.1:4433";
        char arg6[] = "--cert";
        char arg7[] = "cert.pem";
        char arg8[] = "--key";
        char arg9[] = "key.pem";
        char arg10[] = "--ca";
        char arg11[] = "ca.pem";
        char* argv[] = {arg0, arg1, arg2, arg3, arg4, arg5, arg6, arg7, arg8, arg9, arg10, arg11};
        assert(TqParseArgs(12, argv, cfg, err));
        assert(cfg.SpeedTestMode == TqSpeedTestMode::Upload);
        assert(cfg.SpeedTestDurationSec == 5);
    }

    {
        TqConfig cfg{};
        std::string err;
        char arg0[] = "tcpquic-proxy";
        char arg1[] = "client";
        char arg2[] = "--download-test";
        char arg3[] = "10";
        char arg4[] = "--upload-test";
        char arg5[] = "5";
        char arg6[] = "--peer";
        char arg7[] = "127.0.0.1:4433";
        char arg8[] = "--cert";
        char arg9[] = "cert.pem";
        char arg10[] = "--key";
        char arg11[] = "key.pem";
        char arg12[] = "--ca";
        char arg13[] = "ca.pem";
        char* argv[] = {arg0, arg1, arg2, arg3, arg4, arg5, arg6, arg7, arg8, arg9, arg10, arg11, arg12, arg13};
        assert(!TqParseArgs(14, argv, cfg, err));
    }

    {
        TqConfig cfg{};
        std::string err;
        char arg0[] = "tcpquic-proxy";
        char arg1[] = "client";
        char arg2[] = "--download-test";
        char arg3[] = "0";
        char arg4[] = "--peer";
        char arg5[] = "127.0.0.1:4433";
        char arg6[] = "--cert";
        char arg7[] = "cert.pem";
        char arg8[] = "--key";
        char arg9[] = "key.pem";
        char arg10[] = "--ca";
        char arg11[] = "ca.pem";
        char* argv[] = {arg0, arg1, arg2, arg3, arg4, arg5, arg6, arg7, arg8, arg9, arg10, arg11};
        assert(!TqParseArgs(12, argv, cfg, err));
    }

    {
        TqConfig cfg{};
        std::string err;
        char arg0[] = "tcpquic-proxy";
        char arg1[] = "client";
        char arg2[] = "--upload-test";
        char arg3[] = "86401";
        char arg4[] = "--peer";
        char arg5[] = "127.0.0.1:4433";
        char arg6[] = "--cert";
        char arg7[] = "cert.pem";
        char arg8[] = "--key";
        char arg9[] = "key.pem";
        char arg10[] = "--ca";
        char arg11[] = "ca.pem";
        char* argv[] = {arg0, arg1, arg2, arg3, arg4, arg5, arg6, arg7, arg8, arg9, arg10, arg11};
        assert(!TqParseArgs(12, argv, cfg, err));
    }

    {
        TqConfig cfg{};
        std::string err;
        char arg0[] = "tcpquic-proxy";
        char arg1[] = "server";
        char arg2[] = "--download-test";
        char arg3[] = "10";
        char arg4[] = "--listen";
        char arg5[] = "127.0.0.1:4433";
        char arg6[] = "--cert";
        char arg7[] = "cert.pem";
        char arg8[] = "--key";
        char arg9[] = "key.pem";
        char arg10[] = "--ca";
        char arg11[] = "ca.pem";
        char* argv[] = {arg0, arg1, arg2, arg3, arg4, arg5, arg6, arg7, arg8, arg9, arg10, arg11};
        assert(!TqParseArgs(12, argv, cfg, err));
    }

    {
        TqConfig cfg{};
        std::string err;
        char arg0[] = "tcpquic-proxy";
        char arg1[] = "client";
        char arg2[] = "--client-config";
        char arg3[] = "client-config.json";
        char arg4[] = "--download-test";
        char arg5[] = "10";
        char* argv[] = {arg0, arg1, arg2, arg3, arg4, arg5};
        assert(!TqParseArgs(6, argv, cfg, err));
    }

    {
        TqConfig cfg{};
        std::string err;
        char arg0[] = "tcpquic-proxy";
        char arg1[] = "client";
        char arg2[] = "--warmup-mb";
        char arg3[] = "1";
        char arg4[] = "--download-test";
        char arg5[] = "10";
        char arg6[] = "--peer";
        char arg7[] = "127.0.0.1:4433";
        char arg8[] = "--cert";
        char arg9[] = "cert.pem";
        char arg10[] = "--key";
        char arg11[] = "key.pem";
        char arg12[] = "--ca";
        char arg13[] = "ca.pem";
        char* argv[] = {arg0, arg1, arg2, arg3, arg4, arg5, arg6, arg7, arg8, arg9, arg10, arg11, arg12, arg13};
        assert(!TqParseArgs(14, argv, cfg, err));
    }

    return 0;
}
