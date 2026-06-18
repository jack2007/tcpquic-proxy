#pragma once

#include "compress.h"
#include "msquic.hpp"
#include "platform_socket.h"
#include "relay.h"
#include "relay_error.h"
#include "trace.h"
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
#include <list>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

struct MsQuicStream;
struct QUIC_STREAM_EVENT;
struct TqWindowsPendingQuicReceive;

struct TqWindowsRelayWorkerSnapshot {
    uint64_t DeferredReceiveQueued{0};
    uint64_t DeferredReceiveBytesQueued{0};
    uint64_t DeferredReceiveCompleteBytes{0};
    uint64_t DeferredReceiveCompletes{0};
    uint64_t DeferredReceiveCompletionFlushes{0};
    uint64_t PendingQuicReceiveBytes{0};
    uint64_t MaxPendingQuicReceiveBytes{0};
    uint64_t PendingQuicReceiveQueueDepth{0};
    uint64_t MaxPendingQuicReceiveQueueDepth{0};
    uint64_t QuicReceivePausedCount{0};
    uint64_t QuicReceiveResumedCount{0};
    uint64_t TcpSendOperationsPosted{0};
    uint64_t TcpSendBytes{0};
    uint64_t TcpSendPartialCompletions{0};
    uint64_t TcpSendWouldBlockOrPendingCount{0};
    uint64_t TcpRecvOperationsCreated{0};
    uint64_t TcpRecvOperationsReused{0};
    uint64_t RelayBufferBytesInUse{0};
    uint64_t RelayBufferAllocateCount{0};
    uint64_t ZstdDecompressInputBytes{0};
    uint64_t ZstdDecompressOutputBytes{0};
    uint64_t ZstdDecompressCalls{0};
    uint64_t ZstdDecompressNeedInput{0};
    uint64_t ZstdDecompressNeedOutput{0};
    uint64_t ZstdDecompressFailures{0};
    uint64_t FatalRelayResets{0};
    uint64_t GracefulRelayDrains{0};
    uint64_t TcpHardErrors{0};
    uint64_t QuicSendBackpressureEvents{0};
    uint64_t QuicSendFatalErrors{0};
    uint64_t Errors{0};
    uint64_t ActiveRelays{0};
};

class TqWindowsRelayWorker {
public:
    TqWindowsRelayWorker();
    ~TqWindowsRelayWorker();

    bool Start();
    void Stop();
    TqWindowsRelayWorkerSnapshot Snapshot() const;

    bool RegisterRelay(
        TqSocketHandle tcpFd,
        MsQuicStream* stream,
        ITqCompressor* compressor,
        ITqDecompressor* decompressor,
        TqRelayHandle* handle,
        const TqTuningConfig& tuning,
        TqCompressAlgo compressAlgo);

#if defined(TQ_UNIT_TESTING)
    bool RegisterRelayForTest(
        MsQuicStream* stream,
        TqRelayHandle* handle,
        const TqTuningConfig& tuning,
        TqCompressAlgo compressAlgo);
    void SetQuicReceiveViewDrainEnabledForTest(bool enabled);
    bool TestCompleteReceiveViewForCleanup(uint64_t relayId, uint64_t completedLength);
    bool TestAdvanceReceiveViewForCompletion(uint64_t relayId, uint64_t completedLength);
    bool TestLateReceiveViewCompletionIgnored(uint64_t relayId, DWORD bytes, uint64_t postedLength);
    bool TestBufferedTcpSendZeroCompletion(uint64_t relayId);
    bool TestCloseRelayTcpSocketForPostRecvFailure(uint64_t relayId);
#endif

    void StopRelay(uint64_t relayId);
    static QUIC_STATUS QUIC_API StreamCallback(
        MsQuicStream* stream,
        void* context,
        QUIC_STREAM_EVENT* event) noexcept;

private:
    struct RelayContext;
    struct IoOperation;
    struct CallbackBinding;

    void Run();
    void PostStop();

    bool PostTcpRecv(const std::shared_ptr<RelayContext>& relay);
    void HandleTcpRecv(std::unique_ptr<IoOperation> op, DWORD bytes);
    void HandleTcpSend(std::unique_ptr<IoOperation> op, DWORD bytes);
    void HandleQuicReceiveQueued(std::unique_ptr<IoOperation> op);
    void HandleQuicReceiveViewQueued(std::unique_ptr<IoOperation> op);
    bool QueueDeferredQuicReceive(
        const std::shared_ptr<RelayContext>& relay,
        MsQuicStream* stream,
        const QUIC_BUFFER* buffers,
        uint32_t bufferCount,
        bool fin);
    bool PostTcpSendFromReceiveView(
        const std::shared_ptr<RelayContext>& relay,
        const std::shared_ptr<TqWindowsPendingQuicReceive>& view);
    bool PostTcpSendFromCompressedReceiveView(
        const std::shared_ptr<RelayContext>& relay,
        const std::shared_ptr<TqWindowsPendingQuicReceive>& view);
    void AdvanceReceiveView(
        const std::shared_ptr<RelayContext>& relay,
        TqWindowsPendingQuicReceive& view,
        uint64_t bytes);
    void FlushDeferredReceiveCompletion(TqWindowsPendingQuicReceive& view, bool force);
    void CompleteRemainingReceiveOwnership(TqWindowsPendingQuicReceive& view);
    bool FinishReceiveView(
        const std::shared_ptr<RelayContext>& relay,
        const std::shared_ptr<TqWindowsPendingQuicReceive>& view);
    bool CompletePendingQuicReceive(
        const std::shared_ptr<RelayContext>& relay,
        const std::shared_ptr<TqWindowsPendingQuicReceive>& view);
    void CompleteAllPendingQuicReceives(const std::shared_ptr<RelayContext>& relay);
    void SetQuicReceiveEnabled(const std::shared_ptr<RelayContext>& relay, bool enabled);
    void MaybeResumeQuicReceive(const std::shared_ptr<RelayContext>& relay);
    void PruneRetiredCallbacks(bool keepNewest);
    TqTraceLinuxRelayStreamState BuildRelayTraceState(const std::shared_ptr<RelayContext>& relay) const;
    void TraceRelayReceiveEvent(
        const std::shared_ptr<RelayContext>& relay,
        uint32_t bufferCount,
        uint64_t totalBufferLength,
        uint32_t receiveFlags,
        bool fin) const;
    void TraceRelayBackpressure(
        const std::shared_ptr<RelayContext>& relay,
        const char* action,
        const char* reason) const;
    bool PostTcpSend(std::unique_ptr<IoOperation> op);
    bool PostQuicSend(std::unique_ptr<IoOperation> op, QUIC_SEND_FLAGS flags, bool repostOnBackpressure);
    void RetryPendingQuicSends(const std::shared_ptr<RelayContext>& relay);
    void CloseRelay(const std::shared_ptr<RelayContext>& relay, TqRelayCloseMode mode);
    bool CloseRelayIfDrained(const std::shared_ptr<RelayContext>& relay);
    void FailRelayFatal(const std::shared_ptr<RelayContext>& relay, const char* reason);
    void RecordTcpHardErrorAndFail(const std::shared_ptr<RelayContext>& relay, const char* reason);
    bool IsQuicSendBackpressureStatus(QUIC_STATUS status) const;

    void* Iocp_{nullptr};
    std::thread Thread_;
    std::atomic<bool> Stopping_{false};
    std::atomic<uint64_t> NextRelayId_{1};
    std::atomic<uint64_t> DeferredReceiveQueued_{0};
    std::atomic<uint64_t> DeferredReceiveBytesQueued_{0};
    std::atomic<uint64_t> DeferredReceiveCompleteBytes_{0};
    std::atomic<uint64_t> DeferredReceiveCompletes_{0};
    std::atomic<uint64_t> DeferredReceiveCompletionFlushes_{0};
    std::atomic<uint64_t> MaxPendingQuicReceiveBytesObserved_{0};
    std::atomic<uint64_t> MaxPendingQuicReceiveQueueObserved_{0};
    std::atomic<uint64_t> QuicReceivePausedCount_{0};
    std::atomic<uint64_t> QuicReceiveResumedCount_{0};
    std::atomic<uint64_t> TcpSendOperationsPosted_{0};
    std::atomic<uint64_t> TcpSendBytes_{0};
    std::atomic<uint64_t> TcpSendPartialCompletions_{0};
    std::atomic<uint64_t> TcpSendWouldBlockOrPendingCount_{0};
    std::atomic<uint64_t> TcpRecvOperationsCreated_{0};
    std::atomic<uint64_t> TcpRecvOperationsReused_{0};
    std::atomic<uint64_t> ZstdDecompressInputBytes_{0};
    std::atomic<uint64_t> ZstdDecompressOutputBytes_{0};
    std::atomic<uint64_t> ZstdDecompressCalls_{0};
    std::atomic<uint64_t> ZstdDecompressNeedInput_{0};
    std::atomic<uint64_t> ZstdDecompressNeedOutput_{0};
    std::atomic<uint64_t> ZstdDecompressFailures_{0};
    std::atomic<uint64_t> FatalRelayResets_{0};
    std::atomic<uint64_t> GracefulRelayDrains_{0};
    std::atomic<uint64_t> TcpHardErrors_{0};
    std::atomic<uint64_t> QuicSendBackpressureEvents_{0};
    std::atomic<uint64_t> QuicSendFatalErrors_{0};
    std::atomic<uint64_t> Errors_{0};
#if defined(TQ_UNIT_TESTING)
    std::atomic<bool> QuicReceiveViewDrainEnabledForTest_{true};
#endif
    mutable std::mutex Lock_;
    std::unordered_map<uint64_t, std::shared_ptr<RelayContext>> Relays_;
    // Retired bindings keep late callbacks on old stream contexts safe until Stop or ref-counted pruning.
    std::vector<std::shared_ptr<CallbackBinding>> RetiredCallbacks_;
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
    TqWindowsRelayWorkerSnapshot Snapshot() const;
    void StopRelay(TqRelayHandle* handle);

private:
    mutable std::mutex Lock_;
    std::vector<std::unique_ptr<TqWindowsRelayWorker>> Workers_;
    std::atomic<uint64_t> NextWorker_{0};
};
