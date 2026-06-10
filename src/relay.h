#pragma once

#include <atomic>
#include <cstdint>

#include "compress.h"
#include "platform_socket.h"
#include "tuning.h"

struct MsQuicStream;
class TqLinuxRelayWorker;
class TqWindowsRelayWorker;

enum class TqRelayBackendType {
    None,
    LinuxWorker,
    WindowsWorker,
};

struct TqRelayHandle {
    std::atomic<bool> Stop{false};
    TqRelayBackendType Backend{TqRelayBackendType::None};
    TqLinuxRelayWorker* LinuxWorker{nullptr};
    uint64_t LinuxRelayId{0};
    TqWindowsRelayWorker* WindowsWorker{nullptr};
    uint64_t WindowsRelayId{0};
};

bool TqRelayStart(
    TqSocketHandle tcpFd,
    MsQuicStream* stream,
    ITqCompressor* compressor,
    ITqDecompressor* decompressor,
    TqRelayHandle* handle,
    const TqTuningConfig& tuning,
    TqCompressAlgo compressAlgo = TqCompressAlgo::None);

void TqRelayStop(TqRelayHandle* handle);
bool TqRelayLinuxFastPathEnabled(const TqRelayHandle* handle);
