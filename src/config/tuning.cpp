#include "tuning.h"

#include "config.h"
#include "trace.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <mutex>
#include <thread>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace {

constexpr uint64_t KiB = 1024ull;
constexpr uint64_t MiB = 1024ull * KiB;
constexpr uint64_t kAutoRelayBudgetFallbackBytes = 512ull * MiB;
constexpr uint64_t kAutoRelayBudgetMinBytes = 256ull * MiB;
constexpr uint64_t kAutoRelayBudgetMaxBytes = 8ull * 1024ull * MiB;
constexpr uint64_t kRelayPerTunnelTargetMinBytes = 16ull * MiB;
constexpr uint64_t kRelayPerTunnelTargetMaxBytes = 512ull * MiB;

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

bool MulWouldOverflow(uint64_t left, uint64_t right) {
    return right != 0 && left > std::numeric_limits<uint64_t>::max() / right;
}

uint64_t ComputeBdpBytes(uint32_t bandwidthMbps, uint32_t rttMs) {
    if (bandwidthMbps == 0 || rttMs == 0) {
        return 0;
    }
    const uint64_t bytesPerSecond = static_cast<uint64_t>(bandwidthMbps) * 1'000'000ull / 8ull;
    if (MulWouldOverflow(bytesPerSecond, rttMs)) {
        return std::numeric_limits<uint64_t>::max();
    }
    return bytesPerSecond * rttMs / 1000ull;
}

uint64_t DetectSystemMemoryBytes() {
#if defined(_WIN32)
    MEMORYSTATUSEX status{};
    status.dwLength = sizeof(status);
    if (GlobalMemoryStatusEx(&status) == 0) {
        return 0;
    }
    return static_cast<uint64_t>(status.ullTotalPhys);
#else
    const long pageCount = sysconf(_SC_PHYS_PAGES);
    const long pageSize = sysconf(_SC_PAGESIZE);
    if (pageCount <= 0 || pageSize <= 0) {
        return 0;
    }
    const uint64_t pages = static_cast<uint64_t>(pageCount);
    const uint64_t bytesPerPage = static_cast<uint64_t>(pageSize);
    if (pages > std::numeric_limits<uint64_t>::max() / bytesPerPage) {
        return std::numeric_limits<uint64_t>::max();
    }
    return pages * bytesPerPage;
#endif
}

uint64_t ComputeSystemSoftLimitBytes() {
    const uint64_t systemMemoryBytes = DetectSystemMemoryBytes();
    if (systemMemoryBytes == 0) {
        return kAutoRelayBudgetFallbackBytes;
    }
    return ClampU64(systemMemoryBytes / 8, kAutoRelayBudgetMinBytes, kAutoRelayBudgetMaxBytes);
}

uint64_t ComputeTargetPendingPerTunnelBytes(const TqTuningConfig& tuning) {
    return ClampU64(
        tuning.RelayDefaultIdealSend,
        kRelayPerTunnelTargetMinBytes,
        kRelayPerTunnelTargetMaxBytes);
}

uint32_t BytesToMbRoundUp(uint64_t bytes) {
    const uint64_t mb = (bytes + MiB - 1) / MiB;
    return ClampU32(mb, 0, std::numeric_limits<uint32_t>::max());
}

uint64_t ComputeAutomaticRelayBudgetBytes(const TqTuningConfig& tuning) {
    const uint64_t targetPendingBytes = ComputeTargetPendingPerTunnelBytes(tuning);
    const uint64_t budgetBytes = std::max(
        std::max(ComputeSystemSoftLimitBytes(), targetPendingBytes),
        kAutoRelayBudgetFallbackBytes);
    return ClampU64(budgetBytes, kAutoRelayBudgetMinBytes, kAutoRelayBudgetMaxBytes);
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
    out.MaxPendingBufferBytesPerRelay = std::max<uint64_t>(
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
    if (cfg.TuningOverrideLinuxRelayReadChunkSize > 0) {
        out.LinuxRelayReadChunkSize = cfg.TuningOverrideLinuxRelayReadChunkSize;
    }
    if (cfg.TuningOverrideLinuxRelayTcpWriteMaxBytes > 0) {
        out.LinuxRelayTcpWriteMaxBytes = cfg.TuningOverrideLinuxRelayTcpWriteMaxBytes;
    }
    if (cfg.TuningOverrideLinuxRelayTcpWriteBurstBytes > 0) {
        out.LinuxRelayTcpWriteBurstBytes = cfg.TuningOverrideLinuxRelayTcpWriteBurstBytes;
    }
    if (cfg.TuningOverrideInitialQuicReadAheadBytes > 0) {
        out.InitialQuicReadAheadBytes = cfg.TuningOverrideInitialQuicReadAheadBytes;
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

void TqApplyLinuxRelayDefaults(TqTuningConfig& out, TqTuningMode mode, uint64_t autoBudgetBytes) {
    out.LinuxRelayWorkerCount = TqDetectLinuxRelayWorkers();

    if (mode == TqTuningMode::Lan) {
        out.LinuxRelayMaxIov = 8;
        out.LinuxRelayReadChunkSize = 128 * 1024;
        out.LinuxRelayReadBatchBytes = 256 * 1024;
        out.LinuxRelayQuicRecvBatchBytes = 256 * 1024;
        out.LinuxRelayWorkerEventBudget = 1024;
        out.LinuxRelayWorkerByteBudgetPerTick = 16ull * 1024 * 1024;
    } else {
        out.LinuxRelayMaxIov = 16;
        out.LinuxRelayReadChunkSize = 128 * 1024;
        out.LinuxRelayReadBatchBytes = 1024 * 1024;
        out.LinuxRelayQuicRecvBatchBytes = 1024 * 1024;
        out.LinuxRelayWorkerEventBudget = 4096;
        out.LinuxRelayWorkerByteBudgetPerTick = 64ull * 1024 * 1024;
    }

    const uint64_t relayMemoryBytes =
        ClampU64(autoBudgetBytes, kAutoRelayBudgetMinBytes, kAutoRelayBudgetMaxBytes);
    const uint64_t targetPendingBytes = ComputeTargetPendingPerTunnelBytes(out);
    out.LinuxRelayGlobalPendingBytes = relayMemoryBytes / 2;
    out.LinuxRelayPerTunnelPendingBytes = targetPendingBytes;
    out.MaxPendingBufferBytesPerRelay = std::max<uint64_t>(
        out.LinuxRelayPerTunnelPendingBytes,
        out.LinuxRelayGlobalPendingBytes / std::max<uint32_t>(out.LinuxRelayWorkerCount, 1));
}

} // namespace

int g_TqActiveTcpSocketBuffer = 4 * 1024 * 1024;
std::atomic<uint32_t> g_TqRelayMemoryBudgetMb{0};
std::atomic<uint32_t> g_TqActiveRelays{0};
std::atomic<uint64_t> g_TqQuicReadAheadInitialBytes{1ull * MiB};
std::atomic<uint64_t> g_TqQuicReadAheadBytes{1ull * MiB};

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

    char line[320];
    std::snprintf(
        line,
        sizeof(line),
        "tcpquic-proxy runtime: rtt=%ums%s throughput=%uMbps%s ideal_send=%llu%s "
        "compress_ratio=%u.%u%%%s samples=%llu",
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
    std::fprintf(stderr, "%s\n", line);
    if (TqTraceEnabled()) {
        TqTraceLogLine(line);
    }
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
    const uint64_t autoBudgetBytes = ComputeAutomaticRelayBudgetBytes(out);
    TqSetRelayMemoryBudget(BytesToMbRoundUp(autoBudgetBytes));
    TqApplyLinuxRelayDefaults(out, cfg.TuningMode, autoBudgetBytes);
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
    const uint32_t budgetMb = TqGetRelayMemoryBudget();
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

    if (obs.HasIdealSend && obs.IdealSendBytes < cfg.Tuning.RelayDefaultIdealSend) {
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

void TqRelayResetQuicReadAhead(uint64_t initialBytes) {
    const uint64_t effectiveInitial = std::max<uint64_t>(initialBytes, 1);
    g_TqQuicReadAheadInitialBytes.store(effectiveInitial, std::memory_order_release);
    g_TqQuicReadAheadBytes.store(effectiveInitial, std::memory_order_release);
}

void TqRelayUpdateQuicReadAheadFromNetworkStats(
    uint64_t bandwidthBytesPerSecond,
    uint64_t smoothedRttUs) {
    if (bandwidthBytesPerSecond == 0 || smoothedRttUs == 0) {
        return;
    }
    const long double bdpBytes =
        (static_cast<long double>(bandwidthBytesPerSecond) *
         static_cast<long double>(smoothedRttUs)) /
        1000000.0L;
    const long double doubledBdp = bdpBytes * 2.0L;
    const uint64_t targetBytes =
        doubledBdp > static_cast<long double>(std::numeric_limits<uint64_t>::max())
        ? std::numeric_limits<uint64_t>::max()
        : static_cast<uint64_t>(doubledBdp);
    const uint64_t initialBytes =
        g_TqQuicReadAheadInitialBytes.load(std::memory_order_acquire);
    g_TqQuicReadAheadBytes.store(
        std::max(targetBytes, initialBytes),
        std::memory_order_release);
}

uint64_t TqRelayCurrentQuicReadAheadBytes() {
    return g_TqQuicReadAheadBytes.load(std::memory_order_acquire);
}

void TqRelayResetQuicReadAheadForTest(uint64_t initialBytes) {
    TqRelayResetQuicReadAhead(initialBytes);
}

uint64_t TqRelayCurrentQuicReadAheadBytesForTest() {
    return TqRelayCurrentQuicReadAheadBytes();
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
        "read_chunk=%zu max_pending_buffer=%llu "
        "tcp_write_max=%llu tcp_write_burst=%llu quic_complete_batch=%llu "
        "initial_read_ahead=%llu relay_workers=%u",
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
        static_cast<unsigned long long>(tuning.MaxPendingBufferBytesPerRelay),
        static_cast<unsigned long long>(tuning.LinuxRelayTcpWriteMaxBytes),
        static_cast<unsigned long long>(tuning.LinuxRelayTcpWriteBurstBytes),
        static_cast<unsigned long long>(tuning.LinuxRelayQuicReceiveCompleteBatchBytes),
        static_cast<unsigned long long>(tuning.InitialQuicReadAheadBytes),
        tuning.LinuxRelayWorkerCount);
#if defined(_WIN32)
    std::fprintf(out,
        " win_pending_recv_cap=%llu",
        static_cast<unsigned long long>(tuning.WindowsRelayMaxPendingQuicReceiveBytesPerRelay));
#endif
    std::fputc('\n', out);
}

void TqPrintRelayMemoryBudget(FILE* out) {
    const uint32_t budgetMb = TqGetRelayMemoryBudget();
    if (budgetMb == 0) {
        return;
    }
    std::fprintf(out,
        "tcpquic-proxy relay memory budget: %u MB (pool scales by active tunnels)\n",
        budgetMb);
}

void TqPrintRelayBackend(FILE* out, const TqTuningConfig& tuning) {
#if defined(__linux__)
    std::fprintf(out,
        "tcpquic-proxy relay backend: linux-epoll (%u workers)\n",
        std::max(1u, tuning.LinuxRelayWorkerCount));
#elif defined(_WIN32)
    std::fprintf(out,
        "tcpquic-proxy relay backend: windows-iocp (%u workers)\n",
        std::max(1u, tuning.LinuxRelayWorkerCount));
#else
    std::fprintf(out, "tcpquic-proxy relay backend: unsupported\n");
#endif
}

void TqSetActiveTcpSocketBuffer(int bytes) {
    if (bytes > 0) {
        g_TqActiveTcpSocketBuffer = bytes;
    }
}

int TqGetActiveTcpSocketBuffer() {
    return g_TqActiveTcpSocketBuffer;
}
