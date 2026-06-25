#pragma once

#include <atomic>
#include <cstdint>

#include "compress.h"
#include "platform_socket.h"
#include "tuning.h"

struct MsQuicStream;
class TqLinuxRelayWorker;
class TqWindowsRelayWorker;
class TqDarwinRelayWorker;

enum class TqRelayBackendType {
    None,
    LinuxWorker,
    WindowsWorker,
    DarwinWorker,
};

struct TqRelayHandle {
    std::atomic<bool> Stop{false};
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

bool TqRelayStartQuicReceiveSink(
    MsQuicStream* stream,
    TqRelayHandle* handle,
    const TqTuningConfig& tuning,
    std::atomic<uint64_t>* receiveBytes);

void TqRelayStop(TqRelayHandle* handle);
bool TqRelayLinuxFastPathEnabled(const TqRelayHandle* handle);
void TqRelayUpdateQuicReadAheadFromNetworkStats(
    uint64_t bandwidthBytesPerSecond,
    uint64_t smoothedRttUs);
uint64_t TqRelayCurrentQuicReadAheadBytes();
void TqRelayResetQuicReadAheadForTest(uint64_t initialBytes);
uint64_t TqRelayCurrentQuicReadAheadBytesForTest();
