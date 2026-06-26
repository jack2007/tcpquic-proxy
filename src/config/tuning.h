#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>

constexpr uint32_t TqValidationFlowWindowBytes = 0x80000000u;
constexpr uint64_t TqValidationInitialIdealSendFallbackBytes = 128ull * 1024 * 1024;
constexpr uint64_t TqValidationRelaySendBufferCapBytes = 512ull * 1024 * 1024;

enum class TqTuningMode {
    Auto,
    Lan,
    Wan,
};

struct TqTuningConfig {
    uint32_t StreamRecvWindow{536870912u};
    uint32_t ConnFlowControlWindow{500000000u};
    uint32_t InitialWindowPackets{2000};
    uint32_t InitialRttMs{100};
    size_t RelayIoSize{1024 * 1024};
    size_t RelayCompressIoSize{256 * 1024};
    uint64_t RelayDefaultIdealSend{64ull * 1024 * 1024};
    uint32_t RelayMaxInFlightSends{64};
    size_t RelayMaxFreeSendContexts{64};
    int TcpSocketBufferBytes{4 * 1024 * 1024};
    uint32_t LinuxRelayWorkerCount{0};
    uint32_t LinuxRelayMaxIov{16};
    size_t LinuxRelayReadChunkSize{128 * 1024};
    size_t LinuxRelayReadBatchBytes{1024 * 1024};
    size_t LinuxRelayQuicRecvBatchBytes{1024 * 1024};
    uint64_t LinuxRelayTcpWriteMaxBytes{0};
    uint64_t LinuxRelayTcpWriteBurstBytes{0};
    uint64_t InitialQuicReadAheadBytes{1024ull * 1024};
    uint64_t LinuxRelayGlobalPendingBytes{256ull * 1024 * 1024};
    uint64_t MaxPendingBufferBytesPerRelay{32ull * 1024 * 1024};
    uint64_t LinuxRelayPerTunnelPendingBytes{4ull * 1024 * 1024};
    uint32_t LinuxRelayWorkerEventBudget{4096};
    uint64_t LinuxRelayWorkerByteBudgetPerTick{64ull * 1024 * 1024};
    uint64_t LinuxRelayQuicReceiveCompleteBatchBytes{0};
    uint64_t WindowsRelayMaxPendingQuicReceiveBytesPerRelay{16ull * 1024 * 1024};
    uint64_t WindowsRelayQuicReceiveCompleteBatchBytes{0};
    uint64_t WindowsRelayMaxBufferedQuicSendBytes{64ull * 1024 * 1024};
};

struct TqRuntimeObservations {
    uint32_t MeasuredRttMs{0};
    uint32_t ThroughputMbps{0};
    uint64_t IdealSendBytes{0};
    uint64_t SampleCount{0};
    bool HasRtt{false};
    bool HasThroughput{false};
    bool HasIdealSend{false};
};

struct TqCompressionObservations {
    uint32_t RatioPermille{0};
    uint64_t SampleCount{0};
    bool HasSample{false};
};

struct TqConfig;

TqTuningMode TqParseTuningMode(const char* value);
void TqComputeTuning(const TqConfig& cfg, TqTuningConfig& out);
void TqPrintTuning(const TqTuningConfig& tuning, FILE* out);
void TqPrintRelayMemoryBudget(FILE* out);
void TqPrintRelayBackend(FILE* out, const TqTuningConfig& tuning);
void TqSetActiveTcpSocketBuffer(int bytes);
int TqGetActiveTcpSocketBuffer();
void TqSetRelayMemoryBudget(uint32_t maxMemoryMb);
uint32_t TqGetRelayMemoryBudget();
uint32_t TqRelayRegisterActive();
void TqRelayUnregisterActive();
uint32_t TqGetActiveRelayCount();
void TqApplyRelayPoolBudget(TqTuningConfig& tuning, uint32_t activeRelays);
bool TqRuntimeTuningEnabled(const TqConfig& cfg);
void TqRecordMeasuredRtt(uint32_t rttMs);
void TqRecordRelayThroughput(
    uint64_t bytesSent,
    uint32_t elapsedMs,
    uint64_t idealSendBytes,
    uint32_t inflightSends);
void TqRecordIdealSendHint(uint64_t idealSendBytes);
void TqApplyRuntimeObservations(TqConfig& cfg);
TqRuntimeObservations TqGetRuntimeObservations();
void TqResetRuntimeObservations();
void TqRelayResetQuicReadAhead(uint64_t initialBytes);
void TqRelayUpdateQuicReadAheadFromNetworkStats(
    uint64_t bandwidthBytesPerSecond,
    uint64_t smoothedRttUs);
uint64_t TqRelayCurrentQuicReadAheadBytes();
void TqRelayResetQuicReadAheadForTest(uint64_t initialBytes);
uint64_t TqRelayCurrentQuicReadAheadBytesForTest();
bool TqCompressionAdaptiveEnabled(const TqConfig& cfg);
const char* TqResolveAutoCompress(const TqConfig& cfg);
void TqRecordCompressionSample(uint64_t rawBytes, uint64_t compressedBytes);
TqCompressionObservations TqGetCompressionObservations();
void TqResetCompressionObservations();
