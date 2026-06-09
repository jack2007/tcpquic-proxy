#pragma once

#include <atomic>
#include <cstdint>

#include "compress.h"
#include "tuning.h"

struct MsQuicStream;
class TqLinuxRelayWorker;

enum class TqRelayBackendType {
    None,
    LinuxWorker,
};

struct TqRelayHandle {
    std::atomic<bool> Stop{false};
    TqRelayBackendType Backend{TqRelayBackendType::None};
    TqLinuxRelayWorker* LinuxWorker{nullptr};
    uint64_t LinuxRelayId{0};
};

bool TqRelayStart(
    int tcpFd,
    MsQuicStream* stream,
    ITqCompressor* compressor,
    ITqDecompressor* decompressor,
    TqRelayHandle* handle,
    const TqTuningConfig& tuning,
    TqCompressAlgo compressAlgo = TqCompressAlgo::None);

void TqRelayStop(TqRelayHandle* handle);
bool TqRelayLinuxFastPathEnabled(const TqRelayHandle* handle);
