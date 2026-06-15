#include "tuning.h"

#include "config.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <thread>

namespace {

constexpr uint64_t KiB = 1024ull;
constexpr uint64_t MiB = 1024ull * KiB;

uint64_t ClampU64(uint64_t value, uint64_t minValue, uint64_t maxValue) {
    return std::min(std::max(value, minValue), maxValue);
}

uint32_t ClampU32(uint64_t value, uint32_t minValue, uint32_t maxValue) {
    const uint64_t clamped = ClampU64(value, minValue, maxValue);
    return static_cast<uint32_t>(clamped);
}

uint32_t RoundUpPowerOfTwo(uint32_t value) {
    if (value <= 1) {
        return 1;
    }
    uint32_t result = 1;
    while (result < value && result < (1u << 31)) {
        result <<= 1;
    }
    return result;
}

uint64_t ComputeBdpBytes(uint32_t bandwidthMbps, uint32_t rttMs) {
    if (bandwidthMbps == 0 || rttMs == 0) {
        return 0;
    }
    return static_cast<uint64_t>(bandwidthMbps) * 1'000'000ull / 8ull * rttMs / 1000ull;
}

size_t ChooseRelayIoSize(uint32_t bandwidthMbps) {
    if (bandwidthMbps == 0 || bandwidthMbps <= 100) {
        return 256 * 1024;
    }
    if (bandwidthMbps <= 1000) {
        return 512 * 1024;
    }
    return 1024 * 1024;
}

void ApplyWanDefaults(TqTuningConfig& out) {
    out.StreamRecvWindow = 536870912u;
    out.ConnFlowControlWindow = 500000000u;
    out.InitialWindowPackets = 2000;
    out.InitialRttMs = 100;
    out.RelayIoSize = 1024 * 1024;
    out.RelayCompressIoSize = 256 * 1024;
    out.RelayDefaultIdealSend = 64ull * 1024 * 1024;
    out.RelayMaxInFlightSends = 64;
    out.RelayMaxFreeSendContexts = 64;
    out.TcpSocketBufferBytes = 4 * 1024 * 1024;
}

void ApplyLanDefaults(TqTuningConfig& out) {
    out.StreamRecvWindow = 16u * 1024 * 1024;
    out.ConnFlowControlWindow = 16u * 1024 * 1024;
    out.InitialWindowPackets = 256;
    out.InitialRttMs = 10;
    out.RelayIoSize = 256 * 1024;
    out.RelayCompressIoSize = 64 * 1024;
    out.RelayDefaultIdealSend = 4ull * 1024 * 1024;
    out.RelayMaxInFlightSends = 16;
    out.RelayMaxFreeSendContexts = 16;
    out.TcpSocketBufferBytes = 1024 * 1024;
}

void ApplyBdpTuning(uint32_t bandwidthMbps, uint32_t rttMs, TqTuningConfig& out) {
    const uint64_t bdpBytes = ComputeBdpBytes(bandwidthMbps, rttMs);
    const uint64_t windowBytes = ClampU64(2 * bdpBytes, 16 * MiB, 500 * MiB);
    const uint64_t sendBufferBytes = ClampU64(2 * bdpBytes, 16 * MiB, 500 * MiB);

    out.StreamRecvWindow = RoundUpPowerOfTwo(
        ClampU32(ClampU64(windowBytes, 1 * MiB, 512 * MiB), 1u, 536870912u));
    out.ConnFlowControlWindow = ClampU32(windowBytes, 16u * 1024 * 1024, 500000000u);
    out.InitialRttMs = rttMs > 0 ? rttMs : 100;

    const uint64_t iwBytes = std::min(bdpBytes / 16, 4 * MiB);
    const uint64_t iwPackets = (iwBytes + 1199) / 1200;
    out.InitialWindowPackets = ClampU32(iwPackets, 32, 4000);

    out.RelayIoSize = ChooseRelayIoSize(bandwidthMbps);
    out.RelayCompressIoSize = 256 * 1024;
    out.RelayDefaultIdealSend = sendBufferBytes;
    out.RelayMaxInFlightSends = ClampU32(
        (sendBufferBytes + out.RelayIoSize - 1) / out.RelayIoSize, 8, 64);
    out.RelayMaxFreeSendContexts = out.RelayMaxInFlightSends;

    const uint64_t tcpBuf = ClampU64(windowBytes, 256 * KiB, 4 * MiB);
    out.TcpSocketBufferBytes = static_cast<int>(tcpBuf);
}

void ApplyHighBdpPipelineScaling(TqTuningConfig& out) {
    constexpr uint64_t kHighBdpThreshold = 64ull * 1024 * 1024;
    if (out.RelayDefaultIdealSend < kHighBdpThreshold) {
        return;
    }

    const uint64_t pendingBytes =
        std::min<uint64_t>(out.RelayDefaultIdealSend, 512ull * 1024 * 1024);
    out.LinuxRelayPerTunnelPendingBytes = pendingBytes;
    out.LinuxRelayPerWorkerPendingBytes = std::max<uint64_t>(
        pendingBytes,
        out.LinuxRelayGlobalPendingBytes / std::max<uint32_t>(out.LinuxRelayWorkerCount, 1u));
    out.LinuxRelayWorkerByteBudgetPerTick =
        std::min<uint64_t>(pendingBytes, 512ull * 1024 * 1024);
    out.LinuxRelayReadBatchBytes =
        std::max<uint64_t>(out.LinuxRelayReadBatchBytes, 4ull * 1024 * 1024);
    out.LinuxRelayQuicRecvBatchBytes =
        std::max<uint64_t>(out.LinuxRelayQuicRecvBatchBytes, 4ull * 1024 * 1024);
    out.LinuxRelayMaxIov = std::max<uint32_t>(out.LinuxRelayMaxIov, 32);

    const uint64_t tcpBuf = std::min<uint64_t>(pendingBytes / 4, 64ull * 1024 * 1024);
    if (static_cast<uint64_t>(out.TcpSocketBufferBytes) < tcpBuf) {
        out.TcpSocketBufferBytes = static_cast<int>(std::max<uint64_t>(tcpBuf, 16ull * 1024 * 1024));
    }
}

void ApplyCustomOverrides(const TqConfig& cfg, TqTuningConfig& out) {
    if (cfg.TuningOverrideRelayIoSize > 0) {
        out.RelayIoSize = cfg.TuningOverrideRelayIoSize;
    }
    if (cfg.TuningOverrideRelayInflightBytes > 0) {
        out.RelayDefaultIdealSend = cfg.TuningOverrideRelayInflightBytes;
        if (out.RelayIoSize > 0) {
            out.RelayMaxInFlightSends = ClampU32(
                (cfg.TuningOverrideRelayInflightBytes + out.RelayIoSize - 1) / out.RelayIoSize,
                8,
                2048);
            out.RelayMaxFreeSendContexts = out.RelayMaxInFlightSends;
        }
    }
    if (cfg.TuningOverrideLinuxRelayReadChunkSize > 0) {
        out.LinuxRelayReadChunkSize = cfg.TuningOverrideLinuxRelayReadChunkSize;
    }
    if (cfg.TuningOverrideLinuxRelayWorkerSlots > 0) {
        out.LinuxRelayWorkerSlots = cfg.TuningOverrideLinuxRelayWorkerSlots;
    }
    if (cfg.TuningOverrideLinuxRelayTcpWriteMaxBytes > 0) {
        out.LinuxRelayTcpWriteMaxBytes = cfg.TuningOverrideLinuxRelayTcpWriteMaxBytes;
    }
    if (cfg.TuningOverrideLinuxRelayTcpWriteBurstBytes > 0) {
        out.LinuxRelayTcpWriteBurstBytes = cfg.TuningOverrideLinuxRelayTcpWriteBurstBytes;
    }
    if (cfg.TuningOverrideQuicFcw > 0) {
        out.ConnFlowControlWindow = cfg.TuningOverrideQuicFcw;
    }
    if (cfg.TuningOverrideQuicSrw > 0) {
        out.StreamRecvWindow = RoundUpPowerOfTwo(cfg.TuningOverrideQuicSrw);
    }
    if (cfg.TuningOverrideQuicIw > 0) {
        out.InitialWindowPackets = cfg.TuningOverrideQuicIw;
    }
    if (cfg.TuningOverrideQuicInitRttMs > 0) {
        out.InitialRttMs = cfg.TuningOverrideQuicInitRttMs;
    }
}

uint32_t TqDetectLinuxRelayWorkers() {
    const unsigned int detected = std::thread::hardware_concurrency();
    if (detected == 0) {
        return 1;
    }
    return std::max(1u, std::min(detected, 8u));
}

void TqApplyLinuxRelayDefaults(TqTuningConfig& out, TqTuningMode mode) {
    out.LinuxRelayWorkerCount = TqDetectLinuxRelayWorkers();

    if (mode == TqTuningMode::Lan) {
        out.LinuxRelayMaxIov = 8;
        out.LinuxRelayReadChunkSize = 128 * 1024;
        out.LinuxRelayWorkerSlots = 128;
        out.LinuxRelayReadBatchBytes = 256 * 1024;
        out.LinuxRelayQuicRecvBatchBytes = 256 * 1024;
        out.LinuxRelayWorkerEventBudget = 1024;
        out.LinuxRelayWorkerByteBudgetPerTick = 16ull * 1024 * 1024;
    } else {
        out.LinuxRelayMaxIov = 16;
        out.LinuxRelayReadChunkSize = 128 * 1024;
        out.LinuxRelayWorkerSlots = 128;
        out.LinuxRelayReadBatchBytes = 1024 * 1024;
        out.LinuxRelayQuicRecvBatchBytes = 1024 * 1024;
        out.LinuxRelayWorkerEventBudget = 4096;
        out.LinuxRelayWorkerByteBudgetPerTick = 64ull * 1024 * 1024;
    }

    const uint64_t configuredMemoryBytes =
        static_cast<uint64_t>(TqGetRelayMemoryBudget()) * 1024ull * 1024ull;
    const uint64_t relayMemoryBytes =
        configuredMemoryBytes == 0 ? 512ull * 1024 * 1024 : configuredMemoryBytes;
    out.LinuxRelayGlobalPendingBytes = relayMemoryBytes / 2;
    out.LinuxRelayPerTunnelPendingBytes = std::min<uint64_t>(
        16ull * 1024 * 1024,
        std::max<uint64_t>(4ull * 1024 * 1024, out.LinuxRelayReadBatchBytes * 4));
    out.LinuxRelayPerWorkerPendingBytes = std::max<uint64_t>(
        out.LinuxRelayPerTunnelPendingBytes,
        out.LinuxRelayGlobalPendingBytes / std::max<uint32_t>(out.LinuxRelayWorkerCount, 1));
}

} // namespace

int g_TqActiveTcpSocketBuffer = 4 * 1024 * 1024;
std::atomic<uint32_t> g_TqRelayMemoryBudgetMb{0};
std::atomic<uint32_t> g_TqActiveRelays{0};

struct TqRuntimeState {
    std::mutex Lock;
    TqRuntimeObservations Obs{};
    TqCompressionObservations CompObs{};
    uint64_t LastLogUnixMs{0};
};

TqRuntimeState g_Runtime;

uint64_t TqNowUnixMs() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());
}

uint32_t TqUpdateEmaU32(uint32_t current, uint32_t sample, bool hasValue) {
    if (!hasValue || current == 0) {
        return sample;
    }
    return (current * 7 + sample) / 8;
}

void TqMaybeLogRuntimeObservationsLocked() {
    const uint64_t nowMs = TqNowUnixMs();
    if (g_Runtime.Obs.SampleCount == 0 && !g_Runtime.CompObs.HasSample) {
        return;
    }
    if (g_Runtime.LastLogUnixMs != 0 && nowMs - g_Runtime.LastLogUnixMs < 30000) {
        return;
    }
    g_Runtime.LastLogUnixMs = nowMs;

    std::fprintf(stderr,
        "tcpquic-proxy runtime: rtt=%ums%s throughput=%uMbps%s ideal_send=%llu%s "
        "compress_ratio=%u.%u%%%s samples=%llu\n",
        g_Runtime.Obs.MeasuredRttMs,
        g_Runtime.Obs.HasRtt ? "" : " (pending)",
        g_Runtime.Obs.ThroughputMbps,
        g_Runtime.Obs.HasThroughput ? "" : " (pending)",
        static_cast<unsigned long long>(g_Runtime.Obs.IdealSendBytes),
        g_Runtime.Obs.HasIdealSend ? "" : " (pending)",
        g_Runtime.CompObs.HasSample ? g_Runtime.CompObs.RatioPermille / 10 : 0,
        g_Runtime.CompObs.HasSample ? g_Runtime.CompObs.RatioPermille % 10 : 0,
        g_Runtime.CompObs.HasSample ? "" : " (pending)",
        static_cast<unsigned long long>(
            g_Runtime.Obs.SampleCount + g_Runtime.CompObs.SampleCount));
}

constexpr uint32_t kCompressRatioOffPermille = 980;

const char* TqModeFromRatioPermille(uint32_t ratioPermille) {
    if (ratioPermille >= kCompressRatioOffPermille) {
        return "off";
    }
    return "zstd";
}

void MergeDowngradeTuning(TqTuningConfig& current, const TqTuningConfig& measured) {
    current.StreamRecvWindow = std::min(current.StreamRecvWindow, measured.StreamRecvWindow);
    current.ConnFlowControlWindow =
        std::min(current.ConnFlowControlWindow, measured.ConnFlowControlWindow);
    current.InitialWindowPackets =
        std::min(current.InitialWindowPackets, measured.InitialWindowPackets);
    current.RelayDefaultIdealSend =
        std::min(current.RelayDefaultIdealSend, measured.RelayDefaultIdealSend);
    current.RelayMaxInFlightSends =
        std::min(current.RelayMaxInFlightSends, measured.RelayMaxInFlightSends);
    current.RelayMaxFreeSendContexts =
        std::min(current.RelayMaxFreeSendContexts, measured.RelayMaxFreeSendContexts);
}

TqTuningMode TqParseTuningMode(const char* value) {
    if (value == nullptr) {
        return TqTuningMode::Wan;
    }
    if (std::strcmp(value, "auto") == 0) {
        return TqTuningMode::Auto;
    }
    if (std::strcmp(value, "lan") == 0) {
        return TqTuningMode::Lan;
    }
    if (std::strcmp(value, "wan") == 0) {
        return TqTuningMode::Wan;
    }
    if (std::strcmp(value, "custom") == 0) {
        return TqTuningMode::Custom;
    }
    return TqTuningMode::Wan;
}

void TqComputeTuning(const TqConfig& cfg, TqTuningConfig& out) {
    switch (cfg.TuningMode) {
    case TqTuningMode::Lan:
        ApplyLanDefaults(out);
        break;
    case TqTuningMode::Wan:
        ApplyWanDefaults(out);
        break;
    case TqTuningMode::Auto:
        if (cfg.TargetBandwidthMbps > 0 && cfg.TargetRttMs > 0) {
            ApplyBdpTuning(cfg.TargetBandwidthMbps, cfg.TargetRttMs, out);
        } else {
            ApplyWanDefaults(out);
        }
        break;
    case TqTuningMode::Custom:
        if (cfg.TargetBandwidthMbps > 0 && cfg.TargetRttMs > 0) {
            ApplyBdpTuning(cfg.TargetBandwidthMbps, cfg.TargetRttMs, out);
        } else {
            ApplyWanDefaults(out);
        }
        ApplyCustomOverrides(cfg, out);
        break;
    }
    TqApplyLinuxRelayDefaults(out, cfg.TuningMode);
    if (cfg.TuningMode == TqTuningMode::Custom) {
        ApplyHighBdpPipelineScaling(out);
        ApplyCustomOverrides(cfg, out);
    }
}

void TqSetRelayMemoryBudget(uint32_t maxMemoryMb) {
    g_TqRelayMemoryBudgetMb.store(maxMemoryMb, std::memory_order_relaxed);
}

uint32_t TqGetRelayMemoryBudget() {
    return g_TqRelayMemoryBudgetMb.load(std::memory_order_relaxed);
}

uint32_t TqRelayRegisterActive() {
    return g_TqActiveRelays.fetch_add(1, std::memory_order_acq_rel) + 1;
}

void TqRelayUnregisterActive() {
    uint32_t prev = g_TqActiveRelays.load(std::memory_order_relaxed);
    while (prev > 0) {
        if (g_TqActiveRelays.compare_exchange_weak(
                prev, prev - 1, std::memory_order_acq_rel, std::memory_order_relaxed)) {
            return;
        }
    }
}

uint32_t TqGetActiveRelayCount() {
    return g_TqActiveRelays.load(std::memory_order_relaxed);
}

void TqApplyRelayPoolBudget(TqTuningConfig& tuning, uint32_t activeRelays) {
    const uint32_t budgetMb = g_TqRelayMemoryBudgetMb.load(std::memory_order_relaxed);
    if (budgetMb == 0 || activeRelays == 0 || tuning.RelayIoSize == 0) {
        return;
    }

    const uint64_t perTunnelBytes =
        static_cast<uint64_t>(budgetMb) * MiB / static_cast<uint64_t>(activeRelays);
    const uint32_t maxContexts = ClampU32(
        perTunnelBytes / tuning.RelayIoSize, 1, tuning.RelayMaxInFlightSends);

    tuning.RelayMaxFreeSendContexts = maxContexts;
    tuning.RelayMaxInFlightSends =
        std::min(tuning.RelayMaxInFlightSends, maxContexts);

    const uint64_t poolBytes =
        static_cast<uint64_t>(tuning.RelayIoSize) * maxContexts;
    const uint64_t cappedIdeal = std::min(perTunnelBytes, poolBytes);
    if (cappedIdeal < tuning.RelayDefaultIdealSend) {
        tuning.RelayDefaultIdealSend = cappedIdeal;
    }
}

bool TqRuntimeTuningEnabled(const TqConfig& cfg) {
    if (cfg.TuningMode == TqTuningMode::Lan || cfg.TuningMode == TqTuningMode::Custom) {
        return false;
    }
    return true;
}

void TqRecordMeasuredRtt(uint32_t rttMs) {
    if (rttMs == 0) {
        return;
    }

    std::lock_guard<std::mutex> guard(g_Runtime.Lock);
    g_Runtime.Obs.MeasuredRttMs =
        TqUpdateEmaU32(g_Runtime.Obs.MeasuredRttMs, rttMs, g_Runtime.Obs.HasRtt);
    g_Runtime.Obs.HasRtt = true;
    ++g_Runtime.Obs.SampleCount;
    TqMaybeLogRuntimeObservationsLocked();
}

void TqRecordRelayThroughput(
    uint64_t bytesSent,
    uint32_t elapsedMs,
    uint64_t idealSendBytes,
    uint32_t inflightSends) {
    (void)inflightSends;
    if (bytesSent == 0 || elapsedMs < 100) {
        return;
    }

    const uint64_t bits = bytesSent * 8;
    const uint32_t mbps = static_cast<uint32_t>((bits * 1000) / (static_cast<uint64_t>(elapsedMs) * 1'000'000));
    if (mbps == 0) {
        return;
    }

    {
        std::lock_guard<std::mutex> guard(g_Runtime.Lock);
        g_Runtime.Obs.ThroughputMbps = TqUpdateEmaU32(
            g_Runtime.Obs.ThroughputMbps, mbps, g_Runtime.Obs.HasThroughput);
        g_Runtime.Obs.HasThroughput = true;
        ++g_Runtime.Obs.SampleCount;
    }

    if (idealSendBytes > 0) {
        TqRecordIdealSendHint(idealSendBytes);
    }
}

void TqRecordIdealSendHint(uint64_t idealSendBytes) {
    if (idealSendBytes == 0) {
        return;
    }

    std::lock_guard<std::mutex> guard(g_Runtime.Lock);
    if (!g_Runtime.Obs.HasIdealSend || idealSendBytes > g_Runtime.Obs.IdealSendBytes) {
        g_Runtime.Obs.IdealSendBytes = idealSendBytes;
    }
    g_Runtime.Obs.HasIdealSend = true;
    ++g_Runtime.Obs.SampleCount;
    TqMaybeLogRuntimeObservationsLocked();
}

void TqApplyRuntimeObservations(TqConfig& cfg) {
    if (!TqRuntimeTuningEnabled(cfg)) {
        return;
    }

    TqRuntimeObservations obs;
    {
        std::lock_guard<std::mutex> guard(g_Runtime.Lock);
        obs = g_Runtime.Obs;
    }

    if (obs.SampleCount == 0) {
        return;
    }

    const TqTuningConfig profile = cfg.Tuning;

    if (obs.HasRtt && cfg.TargetRttMs == 0 && cfg.TuningOverrideQuicInitRttMs == 0) {
        cfg.Tuning.InitialRttMs = ClampU32(obs.MeasuredRttMs, 1, 60000);
    }

    if (obs.HasIdealSend && cfg.TuningOverrideRelayInflightBytes == 0 &&
        obs.IdealSendBytes < cfg.Tuning.RelayDefaultIdealSend) {
        cfg.Tuning.RelayDefaultIdealSend = obs.IdealSendBytes;
        if (cfg.Tuning.RelayIoSize > 0) {
            const uint32_t maxContexts = ClampU32(
                (obs.IdealSendBytes + cfg.Tuning.RelayIoSize - 1) / cfg.Tuning.RelayIoSize,
                1,
                profile.RelayMaxInFlightSends);
            cfg.Tuning.RelayMaxInFlightSends =
                std::min(cfg.Tuning.RelayMaxInFlightSends, maxContexts);
            cfg.Tuning.RelayMaxFreeSendContexts =
                std::min(cfg.Tuning.RelayMaxFreeSendContexts, static_cast<size_t>(maxContexts));
        }
    }

    if (cfg.TuningMode == TqTuningMode::Auto && obs.HasThroughput && obs.HasRtt &&
        cfg.TargetBandwidthMbps == 0) {
        TqConfig measuredCfg = cfg;
        measuredCfg.TargetBandwidthMbps = std::max(obs.ThroughputMbps, 1u);
        measuredCfg.TargetRttMs = obs.MeasuredRttMs;
        TqTuningConfig measured{};
        TqComputeTuning(measuredCfg, measured);
        MergeDowngradeTuning(cfg.Tuning, measured);
    } else if (cfg.TuningMode == TqTuningMode::Wan && obs.HasRtt && obs.HasThroughput) {
        TqConfig measuredCfg = cfg;
        measuredCfg.TuningMode = TqTuningMode::Auto;
        measuredCfg.TargetBandwidthMbps = std::max(obs.ThroughputMbps, 1u);
        measuredCfg.TargetRttMs = obs.MeasuredRttMs;
        TqTuningConfig measured{};
        TqComputeTuning(measuredCfg, measured);
        MergeDowngradeTuning(cfg.Tuning, measured);
    }
}

TqRuntimeObservations TqGetRuntimeObservations() {
    std::lock_guard<std::mutex> guard(g_Runtime.Lock);
    return g_Runtime.Obs;
}

void TqResetRuntimeObservations() {
    std::lock_guard<std::mutex> guard(g_Runtime.Lock);
    g_Runtime.Obs = TqRuntimeObservations{};
    g_Runtime.CompObs = TqCompressionObservations{};
    g_Runtime.LastLogUnixMs = 0;
}

bool TqCompressionAdaptiveEnabled(const TqConfig& cfg) {
    return cfg.Compress == "auto";
}

const char* TqResolveAutoCompress(const TqConfig& cfg) {
    if (cfg.Compress != "auto") {
        return cfg.Compress.c_str();
    }

    TqCompressionObservations obs;
    {
        std::lock_guard<std::mutex> guard(g_Runtime.Lock);
        obs = g_Runtime.CompObs;
    }

    if (!obs.HasSample || obs.SampleCount == 0) {
        return "off";
    }
    return TqModeFromRatioPermille(obs.RatioPermille);
}

void TqRecordCompressionSample(uint64_t rawBytes, uint64_t compressedBytes) {
    if (rawBytes < 4096 || compressedBytes == 0) {
        return;
    }

    const uint32_t samplePermille = static_cast<uint32_t>(
        std::min<uint64_t>((compressedBytes * 1000) / rawBytes, 2000));

    std::lock_guard<std::mutex> guard(g_Runtime.Lock);
    g_Runtime.CompObs.RatioPermille = TqUpdateEmaU32(
        g_Runtime.CompObs.RatioPermille,
        samplePermille,
        g_Runtime.CompObs.HasSample);
    g_Runtime.CompObs.HasSample = true;
    ++g_Runtime.CompObs.SampleCount;
    TqMaybeLogRuntimeObservationsLocked();
}

TqCompressionObservations TqGetCompressionObservations() {
    std::lock_guard<std::mutex> guard(g_Runtime.Lock);
    return g_Runtime.CompObs;
}

void TqResetCompressionObservations() {
    std::lock_guard<std::mutex> guard(g_Runtime.Lock);
    g_Runtime.CompObs = TqCompressionObservations{};
}

void TqPrintTuning(const TqTuningConfig& tuning, FILE* out) {
    std::fprintf(out,
        "tcpquic-proxy tuning: srw=%u fcw=%u iw=%u initrtt=%ums "
        "relay_io=%zu ideal_send=%llu inflight=%u tcp_buf=%d "
        "relay_pending=%llu tick_budget=%llu read_batch=%llu "
        "read_chunk=%zu worker_slots=%u "
        "tcp_write_max=%llu tcp_write_burst=%llu quic_complete_batch=%llu\n",
        tuning.StreamRecvWindow,
        tuning.ConnFlowControlWindow,
        tuning.InitialWindowPackets,
        tuning.InitialRttMs,
        tuning.RelayIoSize,
        static_cast<unsigned long long>(tuning.RelayDefaultIdealSend),
        tuning.RelayMaxInFlightSends,
        tuning.TcpSocketBufferBytes,
        static_cast<unsigned long long>(tuning.LinuxRelayPerTunnelPendingBytes),
        static_cast<unsigned long long>(tuning.LinuxRelayWorkerByteBudgetPerTick),
        static_cast<unsigned long long>(tuning.LinuxRelayReadBatchBytes),
        tuning.LinuxRelayReadChunkSize,
        tuning.LinuxRelayWorkerSlots,
        static_cast<unsigned long long>(tuning.LinuxRelayTcpWriteMaxBytes),
        static_cast<unsigned long long>(tuning.LinuxRelayTcpWriteBurstBytes),
        static_cast<unsigned long long>(tuning.LinuxRelayQuicReceiveCompleteBatchBytes));
}

void TqSetActiveTcpSocketBuffer(int bytes) {
    if (bytes > 0) {
        g_TqActiveTcpSocketBuffer = bytes;
    }
}

int TqGetActiveTcpSocketBuffer() {
    return g_TqActiveTcpSocketBuffer;
}
