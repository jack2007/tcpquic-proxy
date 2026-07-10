#pragma once

#include <atomic>
#include <cstdint>
#include <memory>

#include "compress.h"
#include "platform_socket.h"
#include "tuning.h"

struct MsQuicStream;
class TqLinuxRelayWorker;
class TqWindowsRelayWorker;
class TqDarwinRelayWorker;
class TqStreamLifetime;

enum class TqRelayBackendType {
    None,
    LinuxWorker,
    WindowsWorker,
    DarwinWorker,
};

struct TqRelayStopControl {
    std::atomic<bool> Stop{false};
};

struct TqRelayHandle {
    std::atomic<bool> Stop{false};
    std::shared_ptr<TqRelayStopControl> Control{std::make_shared<TqRelayStopControl>()};
    TqRelayBackendType Backend{TqRelayBackendType::None};
    TqLinuxRelayWorker* LinuxWorker{nullptr};
    uint64_t LinuxRelayId{0};
    TqWindowsRelayWorker* WindowsWorker{nullptr};
    uint64_t WindowsRelayId{0};
    uint32_t WindowsWorkerIndex{0};
    TqDarwinRelayWorker* DarwinWorker{nullptr};
    uint64_t DarwinRelayId{0};
};

bool TqRelayStart(
    TqSocketHandle tcpFd,
    MsQuicStream* stream,
    ITqCompressor* compressor,
    ITqDecompressor* decompressor,
    TqRelayHandle* handle,
    const TqTuningConfig& tuning,
    TqCompressAlgo compressAlgo = TqCompressAlgo::None);

bool TqRelayStartManaged(
    TqSocketHandle tcpFd,
    MsQuicStream* stream,
    std::shared_ptr<TqStreamLifetime> streamOwner,
    ITqCompressor* compressor,
    ITqDecompressor* decompressor,
    TqRelayHandle* handle,
    const TqTuningConfig& tuning,
    TqCompressAlgo compressAlgo = TqCompressAlgo::None,
    bool* tcpFdConsumed = nullptr);

bool TqRelayStartQuicReceiveSink(
    MsQuicStream* stream,
    TqRelayHandle* handle,
    const TqTuningConfig& tuning,
    std::atomic<uint64_t>* receiveBytes);

bool TqRelayStartQuicReceiveSinkManaged(
    MsQuicStream* stream,
    std::shared_ptr<TqStreamLifetime> streamOwner,
    TqRelayHandle* handle,
    const TqTuningConfig& tuning,
    std::atomic<uint64_t>* receiveBytes);

void TqRelayStop(TqRelayHandle* handle);
void TqRelaySetTraceContext(TqRelayHandle* handle, uint64_t tunnelId, const char* target);
bool TqRelayLinuxFastPathEnabled(const TqRelayHandle* handle);
void TqRelayUpdateQuicReadAheadFromNetworkStats(
    uint64_t bandwidthBytesPerSecond,
    uint64_t smoothedRttUs);
uint64_t TqRelayCurrentQuicReadAheadBytes();
void TqRelayResetQuicReadAheadForTest(uint64_t initialBytes);
uint64_t TqRelayCurrentQuicReadAheadBytesForTest();
