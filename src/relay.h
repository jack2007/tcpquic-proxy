#pragma once

#include <atomic>
#include <cstdint>

#include "compress.h"
#include "tuning.h"

struct MsQuicStream;
class TqTunnelRelay;

struct TqRelayHandle {
    std::atomic<bool> Stop{false};
    TqTunnelRelay* Relay{nullptr};
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
