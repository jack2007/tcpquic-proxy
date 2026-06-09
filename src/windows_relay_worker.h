#pragma once

#include "compress.h"
#include "msquic.hpp"
#include "platform_socket.h"
#include "relay.h"
#include "tuning.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

struct MsQuicStream;
struct QUIC_STREAM_EVENT;

class TqWindowsRelayWorker {
public:
    TqWindowsRelayWorker();
    ~TqWindowsRelayWorker();

    bool Start();
    void Stop();

    bool RegisterRelay(
        TqSocketHandle tcpFd,
        MsQuicStream* stream,
        ITqCompressor* compressor,
        ITqDecompressor* decompressor,
        TqRelayHandle* handle,
        const TqTuningConfig& tuning,
        TqCompressAlgo compressAlgo);

    void StopRelay(uint64_t relayId);
    static QUIC_STATUS QUIC_API StreamCallback(
        MsQuicStream* stream,
        void* context,
        QUIC_STREAM_EVENT* event) noexcept;

private:
    struct RelayContext;
    struct IoOperation;
    struct CallbackContext;

    void Run();
    void PostStop();

    void* Iocp_{nullptr};
    std::thread Thread_;
    std::atomic<bool> Stopping_{false};
    std::atomic<uint64_t> NextRelayId_{1};
    std::mutex Lock_;
    std::unordered_map<uint64_t, std::shared_ptr<RelayContext>> Relays_;
};

class TqWindowsRelayRuntime {
public:
    static TqWindowsRelayRuntime& Instance();

    bool Start(uint32_t workerCount);
    void Stop();
    bool RegisterRelay(
        TqSocketHandle tcpFd,
        MsQuicStream* stream,
        ITqCompressor* compressor,
        ITqDecompressor* decompressor,
        TqRelayHandle* handle,
        const TqTuningConfig& tuning,
        TqCompressAlgo compressAlgo);
    void StopRelay(TqRelayHandle* handle);

private:
    std::mutex Lock_;
    std::vector<std::unique_ptr<TqWindowsRelayWorker>> Workers_;
    std::atomic<uint64_t> NextWorker_{0};
};
