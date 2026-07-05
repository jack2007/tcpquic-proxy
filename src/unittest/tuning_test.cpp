#include "config.h"
#include "tuning.h"

#include <cassert>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <system_error>

#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

#define TQ_TEST_REQUIRE(expr) \
    do { \
        if (!(expr)) { \
            return __LINE__; \
        } \
    } while (false)

static int RequireRelayLegacyMirror(const TqTuningConfig& tuning) {
    TQ_TEST_REQUIRE(tuning.LinuxRelayWorkerCount == tuning.RelayWorkerCount);
    TQ_TEST_REQUIRE(tuning.LinuxRelayMaxIov == tuning.RelayMaxIov);
    TQ_TEST_REQUIRE(tuning.LinuxRelayReadChunkSize == tuning.RelayReadChunkSize);
    TQ_TEST_REQUIRE(tuning.LinuxRelayReadBatchBytes == tuning.RelayReadBatchBytes);
    TQ_TEST_REQUIRE(tuning.LinuxRelayQuicRecvBatchBytes == tuning.RelayQuicRecvBatchBytes);
    TQ_TEST_REQUIRE(tuning.LinuxRelayTcpWriteMaxBytes == tuning.RelayTcpWriteMaxBytes);
    TQ_TEST_REQUIRE(tuning.LinuxRelayTcpWriteBurstBytes == tuning.RelayTcpWriteBurstBytes);
    TQ_TEST_REQUIRE(tuning.LinuxRelayGlobalPendingBytes == tuning.RelayGlobalPendingBytes);
    TQ_TEST_REQUIRE(tuning.LinuxRelayPerTunnelPendingBytes == tuning.RelayPerTunnelPendingBytes);
    TQ_TEST_REQUIRE(tuning.LinuxRelayWorkerEventBudget == tuning.RelayWorkerEventBudget);
    TQ_TEST_REQUIRE(tuning.LinuxRelayEventQueueCapacity == tuning.RelayEventQueueCapacity);
    TQ_TEST_REQUIRE(tuning.LinuxRelayWorkerByteBudgetPerTick == tuning.RelayWorkerByteBudgetPerTick);
    TQ_TEST_REQUIRE(tuning.LinuxRelayQuicReceiveCompleteBatchBytes == tuning.RelayQuicReceiveCompleteBatchBytes);
    return 0;
}

static bool ParseClientOption(const char* option, const char* value, TqConfig& cfg, std::string& err) {
    const char* args[] = {
        "tcpquic-proxy",
        "client",
        "--peer",
        "127.0.0.1:4433",
        option,
        value,
        "--cert",
        "cert.pem",
        "--key",
        "key.pem",
        "--ca",
        "ca.pem"};
    return TqParseArgs(
        static_cast<int>(sizeof(args) / sizeof(args[0])),
        const_cast<char**>(args),
        cfg,
        err);
}

static int CurrentProcessId() {
#ifdef _WIN32
    return _getpid();
#else
    return getpid();
#endif
}

static std::filesystem::path UniqueTempJsonPath(const char* name) {
    static unsigned counter = 0;
    std::ostringstream pathName;
    pathName << name << "-" << CurrentProcessId() << "-" << counter++ << ".json";
    return std::filesystem::temp_directory_path() / pathName.str();
}

class ScopedTempFile {
public:
    explicit ScopedTempFile(const char* name)
        : Path(UniqueTempJsonPath(name)),
          PathString(Path.string()) {}

    ~ScopedTempFile() {
        std::error_code ignored;
        std::filesystem::remove(Path, ignored);
    }

    const char* c_str() const {
        return PathString.c_str();
    }

    const std::filesystem::path& path() const {
        return Path;
    }

private:
    std::filesystem::path Path;
    std::string PathString;
};

static bool WriteTextFile(const std::filesystem::path& path, const char* text) {
    std::ofstream out(path);
    if (!out) {
        return false;
    }
    out << text;
    return out.good();
}

int main() {
    {
        TQ_TEST_REQUIRE(TqValidationRelaySendBufferCapBytes == 512ull * 1024 * 1024);
    }

    {
        TqConfig cfg{};
        cfg.TuningMode = TqTuningMode::Wan;
        TqComputeTuning(cfg, cfg.Tuning);
        TQ_TEST_REQUIRE(RequireRelayLegacyMirror(cfg.Tuning) == 0);

        assert(cfg.Tuning.StreamRecvWindow == TqValidationFlowWindowBytes);
        assert(cfg.Tuning.ConnFlowControlWindow == TqValidationFlowWindowBytes);
        assert(cfg.Tuning.InitialWindowPackets == 2000);
        assert(cfg.Tuning.InitialRttMs == 100);
        assert(cfg.Tuning.RelayIoSize == 1024 * 1024);
        assert(cfg.Tuning.RelayDefaultIdealSend == 64ull * 1024 * 1024);
        assert(cfg.Tuning.InitialQuicReadAheadBytes == TqValidationInitialIdealSendFallbackBytes);
        assert(cfg.Tuning.LinuxRelayPerTunnelPendingBytes >= TqValidationRelaySendBufferCapBytes);
        assert(cfg.Tuning.MaxPendingBufferBytesPerRelay >= TqValidationRelaySendBufferCapBytes);
        assert(cfg.Tuning.LinuxRelayReadBatchBytes == 4ull * 1024 * 1024);
        assert(cfg.Tuning.LinuxRelayQuicRecvBatchBytes == 4ull * 1024 * 1024);
        assert(cfg.Tuning.LinuxRelayMaxIov == 32);
        assert(cfg.Tuning.RelayMaxInFlightSends == 64);
        assert(cfg.Tuning.TcpSocketBufferBytes >= 16 * 1024 * 1024);
        assert(cfg.Tuning.WindowsRelayMaxPendingQuicReceiveBytesPerRelay == 16ull * 1024 * 1024);
        assert(cfg.Tuning.WindowsRelayQuicReceiveCompleteBatchBytes == 0);
    }

    {
        TqConfig cfg{};
        cfg.TuningMode = TqTuningMode::Lan;
        TqComputeTuning(cfg, cfg.Tuning);
        TQ_TEST_REQUIRE(RequireRelayLegacyMirror(cfg.Tuning) == 0);

        assert(cfg.Tuning.StreamRecvWindow == TqValidationFlowWindowBytes);
        assert(cfg.Tuning.ConnFlowControlWindow == TqValidationFlowWindowBytes);
        assert(cfg.Tuning.RelayIoSize == 256 * 1024);
        assert(cfg.Tuning.RelayMaxInFlightSends == 16);
    }

    {
        TqConfig cfg{};
        cfg.TuningMode = TqTuningMode::Auto;
        TqComputeTuning(cfg, cfg.Tuning);

        assert(cfg.Tuning.StreamRecvWindow == TqValidationFlowWindowBytes);
        assert(cfg.Tuning.ConnFlowControlWindow == TqValidationFlowWindowBytes);
        assert(cfg.Tuning.RelayIoSize == 1024 * 1024);
        assert(cfg.Tuning.InitialRttMs == 100);
        assert(cfg.Tuning.RelayMaxInFlightSends == 64);
    }

    {
        TqConfig cfg{};
        cfg.TuningMode = TqTuningMode::Wan;
        cfg.TuningOverrideRelayIoSize = 512 * 1024;
        cfg.TuningOverrideLinuxRelayReadChunkSize = 256 * 1024;
        cfg.TuningOverrideQuicInitRttMs = 50;
        TqComputeTuning(cfg, cfg.Tuning);
        TQ_TEST_REQUIRE(RequireRelayLegacyMirror(cfg.Tuning) == 0);

        assert(cfg.Tuning.RelayIoSize == 512 * 1024);
        assert(cfg.Tuning.LinuxRelayReadChunkSize == 256 * 1024);
        assert(cfg.Tuning.InitialRttMs == 50);
    }

    {
        TqConfig cfg{};
        cfg.TuningMode = TqTuningMode::Wan;
        cfg.TuningOverrideRelayIoSize = 1024 * 1024;
        cfg.TuningOverrideLinuxRelayReadChunkSize = 131072;
        cfg.TuningOverrideQuicInitRttMs = 100;
        TqComputeTuning(cfg, cfg.Tuning);

        assert(cfg.Tuning.LinuxRelayReadChunkSize == 131072);
        assert(cfg.Tuning.LinuxRelayPerTunnelPendingBytes >= TqValidationRelaySendBufferCapBytes);
        assert(cfg.Tuning.InitialRttMs == 100);
    }

    {
        TqConfig cfg{};
        cfg.TuningMode = TqTuningMode::Wan;
        cfg.TuningOverrideRelayIoSize = 1024 * 1024;
        cfg.TuningOverrideQuicIw = 4000;
        cfg.TuningOverrideQuicInitRttMs = 100;
        TqComputeTuning(cfg, cfg.Tuning);

        assert(cfg.Tuning.RelayMaxInFlightSends == 64);
        assert(cfg.Tuning.RelayDefaultIdealSend == 64ull * 1024 * 1024);
        assert(cfg.Tuning.InitialWindowPackets == 4000);
        assert(cfg.Tuning.InitialRttMs == 100);
        assert(cfg.Tuning.LinuxRelayPerTunnelPendingBytes >= TqValidationRelaySendBufferCapBytes);
        assert(cfg.Tuning.TcpSocketBufferBytes >= 16 * 1024 * 1024);
    }

    {
        TqSetRelayMemoryBudget(0);
        TqConfig cfg{};
        cfg.TuningMode = TqTuningMode::Auto;
        TqComputeTuning(cfg, cfg.Tuning);

        TQ_TEST_REQUIRE(TqGetRelayMemoryBudget() >= 256);
        TQ_TEST_REQUIRE(cfg.Tuning.LinuxRelayPerTunnelPendingBytes >= 16ull * 1024 * 1024);
    }

    {
        TqSetRelayMemoryBudget(0);
        TqConfig cfg{};
        cfg.TuningMode = TqTuningMode::Auto;
        TqComputeTuning(cfg, cfg.Tuning);

        TQ_TEST_REQUIRE(cfg.Tuning.LinuxRelayPerTunnelPendingBytes >= TqValidationRelaySendBufferCapBytes);
        TQ_TEST_REQUIRE(cfg.Tuning.MaxPendingBufferBytesPerRelay >= TqValidationRelaySendBufferCapBytes);
        TQ_TEST_REQUIRE(TqGetRelayMemoryBudget() >= 512);
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
        char arg2[] = "--peer";
        char arg3[] = "127.0.0.1:4433";
        char arg4[] = "--reconnect-interval-ms";
        char arg5[] = "3000";
        char arg6[] = "--cert";
        char arg7[] = "cert.pem";
        char arg8[] = "--key";
        char arg9[] = "key.pem";
        char arg10[] = "--ca";
        char arg11[] = "ca.pem";
        char* argv[] = {
            arg0, arg1, arg2, arg3, arg4, arg5, arg6, arg7,
            arg8, arg9, arg10, arg11};
        if (TqParseArgs(12, argv, cfg, err)) {
            return 3;
        }
        if (err.find("--reconnect-interval-ms") == std::string::npos) {
            return 4;
        }
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
        char arg1[] = "client";
        char arg2[] = "--peer";
        char arg3[] = "127.0.0.1:4433";
        char arg4[] = "--cert";
        char arg5[] = "cert.pem";
        char arg6[] = "--key";
        char arg7[] = "key.pem";
        char arg8[] = "--ca";
        char arg9[] = "ca.pem";
        char arg10[] = "--linux-relay-event-queue-capacity";
        char arg11[] = "65535";
        char* argv[] = {
            arg0, arg1, arg2, arg3, arg4, arg5, arg6, arg7, arg8, arg9, arg10, arg11};
        TQ_TEST_REQUIRE(TqParseArgs(12, argv, cfg, err));
        TqFinalizeConfig(cfg);
        TQ_TEST_REQUIRE(cfg.Tuning.LinuxRelayEventQueueCapacity == 65536);
    }

    {
        const char* invalidValues[] = {"0", "1023", "1048577"};
        for (const char* invalidValue : invalidValues) {
            TqConfig cfg{};
            std::string err;
            TQ_TEST_REQUIRE(!ParseClientOption(
                "--linux-relay-event-queue-capacity",
                invalidValue,
                cfg,
                err));
            TQ_TEST_REQUIRE(
                err.find("invalid value for --linux-relay-event-queue-capacity") !=
                std::string::npos);
        }
    }

    {
        const ScopedTempFile file("tcpquic-tuning-event-queue-capacity-valid");
        TQ_TEST_REQUIRE(WriteTextFile(
            file.path(),
            "{"
            "\"tls\":{\"cert\":\"cert.pem\",\"key\":\"key.pem\",\"ca\":\"ca.pem\"},"
            "\"proto\":{\"profile\":\"max-throughput\"},"
            "\"relay\":{\"linux\":{\"event_queue_capacity\":32767}},"
            "\"client\":{},"
            "\"peers\":[{\"id\":\"p1\",\"proto_peer\":\"127.0.0.1:4433\","
            "\"socks_listen\":\"127.0.0.1:11080\"}]"
            "}"));

        TqConfig cfg{};
        std::string err;
        char arg0[] = "tcpquic-proxy";
        char arg1[] = "client";
        char arg2[] = "--config";
        char* argv[] = {arg0, arg1, arg2, const_cast<char*>(file.c_str())};
        TQ_TEST_REQUIRE(TqParseArgs(4, argv, cfg, err));
        TqFinalizeConfig(cfg);
        TQ_TEST_REQUIRE(cfg.Tuning.LinuxRelayEventQueueCapacity == 32768);
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
        char arg10[] = "--windows-relay-worker-count";
        char arg11[] = "3";
        char* argv[] = {
            arg0, arg1, arg2, arg3, arg4, arg5, arg6, arg7, arg8, arg9, arg10, arg11};
        TQ_TEST_REQUIRE(TqParseArgs(12, argv, cfg, err));
        TqFinalizeConfig(cfg);
        TQ_TEST_REQUIRE(cfg.Tuning.WindowsRelayWorkerCount == 3);
        TQ_TEST_REQUIRE(cfg.Tuning.LinuxRelayWorkerCount == TqDetectRelayWorkers());
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
        char arg10[] = "--linux-relay-worker-count";
        char arg11[] = "2";
        char* argv[] = {
            arg0, arg1, arg2, arg3, arg4, arg5, arg6, arg7, arg8, arg9, arg10, arg11};
        TQ_TEST_REQUIRE(TqParseArgs(12, argv, cfg, err));
        TqFinalizeConfig(cfg);
        TQ_TEST_REQUIRE(cfg.Tuning.LinuxRelayWorkerCount == 2);
        TQ_TEST_REQUIRE(cfg.Tuning.WindowsRelayWorkerCount == TqDetectRelayWorkers());
    }

    {
        const char* invalidValues[] = {"0", "9", "100"};
        for (const char* invalidValue : invalidValues) {
            TqConfig cfg{};
            std::string err;
            TQ_TEST_REQUIRE(!ParseClientOption(
                "--windows-relay-worker-count",
                invalidValue,
                cfg,
                err));
            TQ_TEST_REQUIRE(
                err.find("invalid value for --windows-relay-worker-count") !=
                std::string::npos);
        }
    }

    {
        const ScopedTempFile file("tcpquic-tuning-windows-worker-count");
        TQ_TEST_REQUIRE(WriteTextFile(
            file.path(),
            "{"
            "\"tls\":{\"cert\":\"cert.pem\",\"key\":\"key.pem\",\"ca\":\"ca.pem\"},"
            "\"proto\":{\"profile\":\"max-throughput\"},"
            "\"relay\":{\"windows\":{\"worker_count\":4}},"
            "\"client\":{},"
            "\"peers\":[{\"id\":\"p1\",\"proto_peer\":\"127.0.0.1:4433\","
            "\"socks_listen\":\"127.0.0.1:11080\"}]"
            "}"));

        TqConfig cfg{};
        std::string err;
        char arg0[] = "tcpquic-proxy";
        char arg1[] = "client";
        char arg2[] = "--config";
        char* argv[] = {arg0, arg1, arg2, const_cast<char*>(file.c_str())};
        TQ_TEST_REQUIRE(TqParseArgs(4, argv, cfg, err));
        TqFinalizeConfig(cfg);
        TQ_TEST_REQUIRE(cfg.Tuning.WindowsRelayWorkerCount == 4);
    }

    {
        const ScopedTempFile file("tcpquic-tuning-event-queue-capacity-invalid");
        TQ_TEST_REQUIRE(WriteTextFile(
            file.path(),
            "{"
            "\"tls\":{\"cert\":\"cert.pem\",\"key\":\"key.pem\",\"ca\":\"ca.pem\"},"
            "\"relay\":{\"linux\":{\"event_queue_capacity\":1023}},"
            "\"peers\":[{\"id\":\"p1\",\"proto_peer\":\"127.0.0.1:4433\","
            "\"socks_listen\":\"127.0.0.1:11080\"}]"
            "}"));

        TqConfig cfg{};
        std::string err;
        char arg0[] = "tcpquic-proxy";
        char arg1[] = "client";
        char arg2[] = "--config";
        char* argv[] = {arg0, arg1, arg2, const_cast<char*>(file.c_str())};
        TQ_TEST_REQUIRE(!TqParseArgs(4, argv, cfg, err));
        TQ_TEST_REQUIRE(
            err.find("invalid relay.linux.event_queue_capacity") != std::string::npos);
    }

    {
        const char* removedOptions[][2] = {
            {"--target-bandwidth-mbps", "10000"},
            {"--target-rtt-ms", "100"},
            {"--relay-inflight-bytes", "1048576"},
            {"--initial-quic-read-ahead", "2097152"},
            {"--fcw", "1073741824"},
            {"--srw", "1073741824"},
            {"--tuning", "custom"},
        };
        for (const auto& removed : removedOptions) {
            TqConfig cfg{};
            std::string err;
            TQ_TEST_REQUIRE(!ParseClientOption(removed[0], removed[1], cfg, err));
            TQ_TEST_REQUIRE(err.find(removed[0]) != std::string::npos ||
                            err.find("custom") != std::string::npos ||
                            err.find("unknown option") != std::string::npos);
        }
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
        assert(err.find("unknown option") != std::string::npos);
    }

    {
        TqSetRelayMemoryBudget(0);
        TqConfig cfg{};
        cfg.TuningMode = TqTuningMode::Wan;
        TqComputeTuning(cfg, cfg.Tuning);
        TQ_TEST_REQUIRE(RequireRelayLegacyMirror(cfg.Tuning) == 0);
        const uint32_t autoBudgetMb = TqGetRelayMemoryBudget();

        assert(cfg.Tuning.LinuxRelayWorkerCount >= 1);
        assert(cfg.Tuning.WindowsRelayWorkerCount >= 1);
        assert(cfg.Tuning.LinuxRelayWorkerCount == cfg.Tuning.WindowsRelayWorkerCount);
        assert(cfg.Tuning.LinuxRelayMaxIov == 32);
        assert(cfg.Tuning.LinuxRelayReadChunkSize == 128 * 1024);
        assert(cfg.Tuning.LinuxRelayReadBatchBytes == 4ull * 1024 * 1024);
        assert(cfg.Tuning.LinuxRelayQuicRecvBatchBytes == 4ull * 1024 * 1024);
        assert(cfg.Tuning.LinuxRelayWorkerEventBudget == 4096);
        assert(cfg.Tuning.LinuxRelayWorkerByteBudgetPerTick >= TqValidationInitialIdealSendFallbackBytes);
        TQ_TEST_REQUIRE(autoBudgetMb >= 256);
        const uint64_t reportedBudgetBytes =
            static_cast<uint64_t>(autoBudgetMb) * 1024 * 1024;
        TQ_TEST_REQUIRE(cfg.Tuning.LinuxRelayGlobalPendingBytes <= reportedBudgetBytes / 2);
        TQ_TEST_REQUIRE(cfg.Tuning.LinuxRelayGlobalPendingBytes + 1024ull * 1024 >=
                        reportedBudgetBytes / 2);
        TQ_TEST_REQUIRE(cfg.Tuning.LinuxRelayPerTunnelPendingBytes >= TqValidationRelaySendBufferCapBytes);
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
        TQ_TEST_REQUIRE(RequireRelayLegacyMirror(cfg.Tuning) == 0);

        assert(cfg.Tuning.LinuxRelayMaxIov == 8);
        assert(cfg.Tuning.LinuxRelayReadChunkSize == 128 * 1024);
        assert(cfg.Tuning.LinuxRelayReadBatchBytes == 256 * 1024);
        assert(cfg.Tuning.LinuxRelayWorkerEventBudget == 1024);
        assert(cfg.Tuning.LinuxRelayWorkerByteBudgetPerTick ==
               TqValidationInitialIdealSendFallbackBytes);
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
        assert(err.find("--warmup-mb") != std::string::npos || err.find("warmup") != std::string::npos);
    }

    return 0;
}
