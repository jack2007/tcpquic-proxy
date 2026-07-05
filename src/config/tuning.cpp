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

void ApplyHighBdpPipelineScaling(TqTuningConfig& out) {
    constexpr uint64_t kHighBdpThreshold = 64ull * 1024 * 1024;
    if (out.RelayDefaultIdealSend < kHighBdpThreshold) {
        return;
    }

    const uint64_t pendingBytes =
        std::min<uint64_t>(out.RelayDefaultIdealSend, 512ull * 1024 * 1024);
    out.RelayPerTunnelPendingBytes = pendingBytes;
    out.MaxPendingBufferBytesPerRelay = std::max<uint64_t>(
        pendingBytes,
        out.RelayGlobalPendingBytes / std::max<uint32_t>(out.RelayWorkerCount, 1u));
    out.RelayWorkerByteBudgetPerTick =
        std::min<uint64_t>(pendingBytes, 512ull * 1024 * 1024);
    out.InitialQuicReadAheadBytes =
        std::max<uint64_t>(out.InitialQuicReadAheadBytes, pendingBytes);
    out.RelayReadBatchBytes =
        std::max<uint64_t>(out.RelayReadBatchBytes, 4ull * 1024 * 1024);
    out.RelayQuicRecvBatchBytes =
        std::max<uint64_t>(out.RelayQuicRecvBatchBytes, 4ull * 1024 * 1024);
    out.RelayMaxIov = std::max<uint32_t>(out.RelayMaxIov, 32);

    const uint64_t tcpBuf = std::min<uint64_t>(pendingBytes / 4, 64ull * 1024 * 1024);
    if (static_cast<uint64_t>(out.TcpSocketBufferBytes) < tcpBuf) {
        out.TcpSocketBufferBytes = static_cast<int>(std::max<uint64_t>(tcpBuf, 16ull * 1024 * 1024));
    }
}

void SyncLinuxRelayLegacyFields(TqTuningConfig& out) {
    out.LinuxRelayWorkerCount = out.RelayWorkerCount;
    out.LinuxRelayMaxIov = out.RelayMaxIov;
    out.LinuxRelayReadChunkSize = out.RelayReadChunkSize;
    out.LinuxRelayReadBatchBytes = out.RelayReadBatchBytes;
    out.LinuxRelayQuicRecvBatchBytes = out.RelayQuicRecvBatchBytes;
    out.LinuxRelayTcpWriteMaxBytes = out.RelayTcpWriteMaxBytes;
    out.LinuxRelayTcpWriteBurstBytes = out.RelayTcpWriteBurstBytes;
    out.LinuxRelayGlobalPendingBytes = out.RelayGlobalPendingBytes;
    out.LinuxRelayPerTunnelPendingBytes = out.RelayPerTunnelPendingBytes;
    out.LinuxRelayWorkerEventBudget = out.RelayWorkerEventBudget;
    out.LinuxRelayEventQueueCapacity = out.RelayEventQueueCapacity;
    out.LinuxRelayWorkerByteBudgetPerTick = out.RelayWorkerByteBudgetPerTick;
    out.LinuxRelayQuicReceiveCompleteBatchBytes = out.RelayQuicReceiveCompleteBatchBytes;
}

void ApplyCustomOverrides(const TqConfig& cfg, TqTuningConfig& out) {
    if (cfg.TuningOverrideRelayIoSize > 0) {
        out.RelayIoSize = cfg.TuningOverrideRelayIoSize;
    }
    if (cfg.TuningOverrideLinuxRelayReadChunkSize > 0) {
        out.RelayReadChunkSize = cfg.TuningOverrideLinuxRelayReadChunkSize;
    }
    if (cfg.TuningOverrideLinuxRelayTcpWriteMaxBytes > 0) {
        out.RelayTcpWriteMaxBytes = cfg.TuningOverrideLinuxRelayTcpWriteMaxBytes;
    }
    if (cfg.TuningOverrideLinuxRelayTcpWriteBurstBytes > 0) {
        out.RelayTcpWriteBurstBytes = cfg.TuningOverrideLinuxRelayTcpWriteBurstBytes;
    }
    if (cfg.TuningOverrideLinuxRelayEventQueueCapacity > 0) {
        out.RelayEventQueueCapacity =
            TqNormalizeLinuxRelayEventQueueCapacity(cfg.TuningOverrideLinuxRelayEventQueueCapacity);
    }
    if (cfg.TuningOverrideLinuxRelayWorkerCount > 0) {
        out.RelayWorkerCount =
            TqNormalizeRelayWorkerCount(cfg.TuningOverrideLinuxRelayWorkerCount);
    }
    if (cfg.TuningOverrideWindowsRelayWorkerCount > 0) {
        out.WindowsRelayWorkerCount =
            TqNormalizeRelayWorkerCount(cfg.TuningOverrideWindowsRelayWorkerCount);
    }
    if (cfg.TuningOverrideQuicIw > 0) {
        out.InitialWindowPackets = cfg.TuningOverrideQuicIw;
    }
    if (cfg.TuningOverrideQuicInitRttMs > 0) {
        out.InitialRttMs = cfg.TuningOverrideQuicInitRttMs;
    }
}

void TqApplyLinuxRelayDefaults(TqTuningConfig& out, TqTuningMode mode, uint64_t autoBudgetBytes) {
    const uint32_t detectedWorkers = TqDetectRelayWorkers();
    out.RelayWorkerCount = detectedWorkers;
    out.WindowsRelayWorkerCount = detectedWorkers;

    if (mode == TqTuningMode::Lan) {
        out.RelayMaxIov = 8;
        out.RelayReadChunkSize = 128 * 1024;
        out.RelayReadBatchBytes = 256 * 1024;
        out.RelayQuicRecvBatchBytes = 256 * 1024;
        out.RelayWorkerEventBudget = 1024;
        out.RelayEventQueueCapacity = 4096;
        out.RelayWorkerByteBudgetPerTick = 16ull * 1024 * 1024;
    } else {
        out.RelayMaxIov = 16;
        out.RelayReadChunkSize = 128 * 1024;
        out.RelayReadBatchBytes = 1024 * 1024;
        out.RelayQuicRecvBatchBytes = 1024 * 1024;
        out.RelayWorkerEventBudget = 4096;
        out.RelayEventQueueCapacity = 4096;
        out.RelayWorkerByteBudgetPerTick = 64ull * 1024 * 1024;
    }

    const uint64_t relayMemoryBytes =
        ClampU64(autoBudgetBytes, kAutoRelayBudgetMinBytes, kAutoRelayBudgetMaxBytes);
    const uint64_t targetPendingBytes = ComputeTargetPendingPerTunnelBytes(out);
    out.RelayGlobalPendingBytes = relayMemoryBytes / 2;
    out.RelayPerTunnelPendingBytes = targetPendingBytes;
    out.MaxPendingBufferBytesPerRelay = std::max<uint64_t>(
        out.RelayPerTunnelPendingBytes,
        out.RelayGlobalPendingBytes / std::max<uint32_t>(out.RelayWorkerCount, 1));
    SyncLinuxRelayLegacyFields(out);
}

} // namespace

uint32_t TqDetectRelayWorkers() {
    const unsigned int detected = std::thread::hardware_concurrency();
    if (detected == 0) {
        return TqRelayWorkerCountMin;
    }
    return std::max(
        TqRelayWorkerCountMin,
        std::min(static_cast<uint32_t>(detected), TqRelayWorkerCountMax));
}

uint32_t TqNormalizeRelayWorkerCount(uint32_t count) {
    return std::max(TqRelayWorkerCountMin, std::min(count, TqRelayWorkerCountMax));
}

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
    return TqTuningMode::Wan;
}

uint32_t TqNormalizeLinuxRelayEventQueueCapacity(uint32_t capacity) {
    if (capacity <= TqLinuxRelayEventQueueCapacityMin) {
        return TqLinuxRelayEventQueueCapacityMin;
    }
    if (capacity >= TqLinuxRelayEventQueueCapacityMax) {
        return TqLinuxRelayEventQueueCapacityMax;
    }

    --capacity;
    capacity |= capacity >> 1;
    capacity |= capacity >> 2;
    capacity |= capacity >> 4;
    capacity |= capacity >> 8;
    capacity |= capacity >> 16;
    ++capacity;
    return std::min(capacity, TqLinuxRelayEventQueueCapacityMax);
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
        ApplyWanDefaults(out);
        break;
    }
    ApplyCustomOverrides(cfg, out);
    const uint64_t budgetBytes = cfg.MaxMemoryMb > 0
        ? static_cast<uint64_t>(cfg.MaxMemoryMb) * MiB
        : ComputeAutomaticRelayBudgetBytes(out);
    TqSetRelayMemoryBudget(BytesToMbRoundUp(budgetBytes));
    TqApplyLinuxRelayDefaults(out, cfg.TuningMode, budgetBytes);
    ApplyHighBdpPipelineScaling(out);
    ApplyCustomOverrides(cfg, out);
    out.StreamRecvWindow = TqValidationFlowWindowBytes;
    out.ConnFlowControlWindow = TqValidationFlowWindowBytes;
    out.InitialQuicReadAheadBytes =
        std::max<uint64_t>(out.InitialQuicReadAheadBytes, TqValidationInitialIdealSendFallbackBytes);
    out.MaxPendingBufferBytesPerRelay =
        std::max<uint64_t>(out.MaxPendingBufferBytesPerRelay, TqValidationRelaySendBufferCapBytes);
    out.RelayPerTunnelPendingBytes =
        std::max<uint64_t>(out.RelayPerTunnelPendingBytes, TqValidationRelaySendBufferCapBytes);
    out.RelayWorkerByteBudgetPerTick =
        std::max<uint64_t>(out.RelayWorkerByteBudgetPerTick, TqValidationInitialIdealSendFallbackBytes);
    SyncLinuxRelayLegacyFields(out);
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
    if (cfg.TuningMode == TqTuningMode::Lan) {
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

    if (obs.HasRtt && cfg.TuningOverrideQuicInitRttMs == 0) {
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
    cfg.Tuning.StreamRecvWindow = TqValidationFlowWindowBytes;
    cfg.Tuning.ConnFlowControlWindow = TqValidationFlowWindowBytes;
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
        "initial_read_ahead=%llu relay_workers=%u ideal_backpressure=stream_event",
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
#if defined(_WIN32)
        tuning.WindowsRelayWorkerCount);
#else
        tuning.LinuxRelayWorkerCount);
#endif
#if defined(_WIN32)
    std::fprintf(out,
        " win_pending_recv_cap=%llu win_max_buffered_quic_send=%llu",
        static_cast<unsigned long long>(tuning.WindowsRelayMaxPendingQuicReceiveBytesPerRelay),
        static_cast<unsigned long long>(tuning.WindowsRelayMaxBufferedQuicSendBytes));
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
        std::max(1u, tuning.WindowsRelayWorkerCount));
#elif defined(__APPLE__)
    std::fprintf(out,
        "tcpquic-proxy relay backend: darwin-kqueue (%u workers)\n",
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
