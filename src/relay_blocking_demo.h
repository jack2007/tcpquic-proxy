#pragma once

#include "compress.h"
#include "tuning.h"

struct MsQuicStream;

struct TqBlockingDemoRelayHandle;

TqBlockingDemoRelayHandle* TqBlockingDemoRelayStart(
    int tcpFd,
    MsQuicStream* stream,
    ITqCompressor* compressor,
    ITqDecompressor* decompressor,
    const TqTuningConfig& tuning,
    TqCompressAlgo compressAlgo);

void TqBlockingDemoRelayStop(TqBlockingDemoRelayHandle* handle);
