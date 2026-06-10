#pragma once

#include "compress.h"
#include "msquic.hpp"
#include "platform_socket.h"
#include "relay.h"
#include "tuning.h"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

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

    bool PostTcpRecv(const std::shared_ptr<RelayContext>& relay);
    void HandleTcpRecv(std::unique_ptr<IoOperation> op, DWORD bytes);
    void HandleTcpSend(std::unique_ptr<IoOperation> op, DWORD bytes);
    void HandleQuicReceiveQueued(std::unique_ptr<IoOperation> op);
    bool PostTcpSend(std::unique_ptr<IoOperation> op);
    void CloseRelay(const std::shared_ptr<RelayContext>& relay);
    bool CloseRelayIfDrained(const std::shared_ptr<RelayContext>& relay);

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
