#pragma once

#include "compress.h"
#include "platform_socket.h"
#include "relay.h"
#include "relay_buffer.h"
#include "stream_lifetime.h"

#include <msquic.hpp>
#include <uv.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <unordered_map>
#include <vector>

class TqUvRelayWorker;

enum class TqUvActivation : std::uint8_t {
    Prepared,
    Active,
    Terminal,
    Failed,
};

enum class TqUvTerminalTrigger : std::uint8_t {
    TcpEof,
    TcpError,
    QuicFin,
    QuicAbort,
    QueueFailure,
    AllocationFailure,
    RuntimeStop,
    DecompressionFailure,
    PressureFailure,
    ReceiveCompletionFailure,
    RegistrationFailure,
};

enum class TqUvSocketOwnership : std::uint8_t {
    CallerOwned,
    UvHandleOwned,
    ActiveRelayOwned,
    Closed,
};

enum class TqUvOperationOwnership : std::uint8_t {
    Created,
    Submitted,
    CompletionClaimed,
    Completed,
    Cancelled,
};

enum class TqUvPendingDirection : std::uint8_t {
    QuicToTcp,
    TcpToQuic,
};

struct TqUvRelayRegistration {
    TqSocketHandle TcpSocket{TqInvalidSocket};
    MsQuicStream* Stream{nullptr};
    std::shared_ptr<TqStreamLifetime> StreamOwner;
    std::shared_ptr<TqRelayStopControl> StopControl;
    std::uint64_t ControlGeneration{0};
    ITqCompressor* Compressor{nullptr};
    ITqDecompressor* Decompressor{nullptr};
    TqCompressAlgo CompressAlgo{TqCompressAlgo::None};
    std::uint64_t PrecommitMaxPendingBytes{4ull * 1024 * 1024};
    std::size_t TcpReadChunkSize{0};
    std::uint64_t MaxPendingBufferBytes{0};
    std::uint64_t MaxBufferedQuicSendBytes{0};
    std::uint64_t ResumeBufferedQuicSendBytes{0};
    std::uint32_t WorkerEventBudget{4096};
    std::uint64_t WorkerByteBudgetPerTick{64ull * 1024 * 1024};
    bool ReleaseAccountingOnFailedRegistration{false};
};

struct TqUvRelayRegistrationResult {
    bool Ok{false};
    bool TcpFdConsumed{false};
    bool TerminalCleanupPending{false};
    std::uint64_t RelayId{0};
    std::shared_ptr<const TqUvRelayCommittedState> Committed;
};

struct TqUvQuicReceiveSlice {
    const std::uint8_t* Data{nullptr};
    std::uint32_t Length{0};
};

struct TqUvPendingQuicReceive {
    MsQuicStream* Stream{nullptr};
    std::shared_ptr<TqStreamLifetime> StreamOwner;
    std::uint64_t RelayId{0};
    std::uint64_t RouteGeneration{0};
    std::uint64_t ControlGeneration{0};
    std::vector<TqUvQuicReceiveSlice> Slices;
    std::size_t SliceIndex{0};
    std::size_t SliceOffset{0};
    std::uint64_t TotalBytes{0};
    std::uint64_t CompletedBytes{0};
    std::uint64_t CompletionPendingBytes{0};
    std::uint32_t CompletionRetryCount{0};
    std::vector<TqBufferRef> DeferredOutputOwners;
    std::uint64_t DeferredOutputBytes{0};
    std::shared_ptr<TqUvPendingQuicReceive> FallbackNext;
    bool TcpWriteSubmitted{false};
    bool DecompressionDrained{false};
    bool Fin{false};
    std::atomic<bool> Settled{false};
};

struct TqUvRelayState;

class TqUvActivationMutex final {
public:
    // TqUvRelayState is the sole owner. Initialize exactly once after allocator
    // bootstrap; destroy only after all activation users have quiesced.
    // Destroy() is idempotent and the destructor performs the final fallback.
    TqUvActivationMutex() noexcept = default;
    ~TqUvActivationMutex() noexcept;
    TqUvActivationMutex(const TqUvActivationMutex&) = delete;
    TqUvActivationMutex& operator=(const TqUvActivationMutex&) = delete;
    TqUvActivationMutex(TqUvActivationMutex&&) = delete;
    TqUvActivationMutex& operator=(TqUvActivationMutex&&) = delete;

    bool Initialize() noexcept;
    void Destroy() noexcept;
    bool Ready() const noexcept;
    bool Lock() noexcept;
    bool Unlock() noexcept;
#if defined(TQ_UNIT_TESTING) && TQ_UNIT_TESTING
    std::uint64_t DestroyCountForTest() const noexcept;
#endif

private:
    uv_mutex_t Mutex_{};
    bool Initialized_{false};
#if defined(TQ_UNIT_TESTING) && TQ_UNIT_TESTING
    std::uint64_t DestroyCountForTest_{0};
#endif
};

struct TqUvStreamBinding final : TqStreamLifetime::Target {
    TqUvRelayWorker* Worker{nullptr};
    std::weak_ptr<TqUvRelayState> Relay;
    std::shared_ptr<TqRelayStopControl> StopControl;
    std::uint64_t RelayId{0};
    std::uint64_t RouteGeneration{0};
    std::uint64_t ControlGeneration{0};
    std::atomic<std::uint64_t> TerminalRouteGeneration{0};
    std::atomic<TqUvActivation> Activation{TqUvActivation::Prepared};
    std::atomic<bool> Closing{false};
    std::atomic<bool> PrecommitSettled{false};

    QUIC_STATUS OnStreamEvent(
        MsQuicStream* stream,
        QUIC_STREAM_EVENT* event,
        std::uint64_t routeGeneration) noexcept override;
#if defined(TQ_UNIT_TESTING) && TQ_UNIT_TESTING
    void* ContextForTest() const noexcept override {
        return const_cast<TqUvStreamBinding*>(this);
    }
#endif
};

struct TqUvTcpWriteOperation {
    uv_write_t Request{};
    std::shared_ptr<TqUvRelayState> RelayOwner;
    std::shared_ptr<TqUvPendingQuicReceive> ReceiveOwner;
    std::vector<TqBufferRef> OutputOwners;
    std::vector<uv_buf_t> Buffers;
    std::uint64_t RelayId{0};
    std::uint64_t RouteGeneration{0};
    std::uint64_t ControlGeneration{0};
    std::uint64_t TotalBytes{0};
    std::uint64_t ReceiveCompleteBytes{0};
    std::uint64_t PressureBytes{0};
    std::atomic<TqUvOperationOwnership> Ownership{
        TqUvOperationOwnership::Created};
};

struct TqUvQuicSendOperation {
    std::shared_ptr<TqUvRelayState> RelayOwner;
    TqStreamLifetime::SendCompletionReservation Reservation;
    std::vector<TqBufferView> Views;
    std::vector<QUIC_BUFFER> QuicBuffers;
    std::uint64_t RelayId{0};
    std::uint64_t RouteGeneration{0};
    std::uint64_t ControlGeneration{0};
    std::uint64_t TotalBytes{0};
    bool Fin{false};
    TqUvQuicSendOperation* FallbackNext{nullptr};
    std::atomic<bool> CompletionQueued{false};
    std::atomic<bool> CompletionCancelled{false};
    std::atomic<TqUvOperationOwnership> Ownership{
        TqUvOperationOwnership::Created};
};

struct TqUvRelayState : std::enable_shared_from_this<TqUvRelayState> {
    TqUvRelayState() noexcept = default;
    ~TqUvRelayState() noexcept = default;
    TqUvRelayState(const TqUvRelayState&) = delete;
    TqUvRelayState& operator=(const TqUvRelayState&) = delete;
    TqUvRelayState(TqUvRelayState&&) = delete;
    TqUvRelayState& operator=(TqUvRelayState&&) = delete;

    TqUvRelayWorker* Worker{nullptr};
    std::uint32_t WorkerIndex{0};
    std::uint64_t RelayId{0};
    std::uint64_t RouteGeneration{0};
    std::uint64_t ControlGeneration{0};
    MsQuicStream* Stream{nullptr};
    std::shared_ptr<TqStreamLifetime> StreamOwner;
    std::shared_ptr<TqRelayStopControl> StopControl;
    std::shared_ptr<TqUvStreamBinding> Binding;
    ITqCompressor* Compressor{nullptr};
    ITqDecompressor* Decompressor{nullptr};
    TqCompressAlgo CompressAlgo{TqCompressAlgo::None};

    TqUvActivationMutex ActivationMutex;
    TqUvActivation Activation{TqUvActivation::Prepared};
    TqUvSocketOwnership SocketOwnership{TqUvSocketOwnership::CallerOwned};
    uv_tcp_t TcpHandle{};
    uv_shutdown_t TcpShutdown{};
    bool TcpHandleInitialized{false};
    bool TcpReadStarted{false};
    bool TcpReadClosed{false};
    bool TcpWriteClosed{false};
    bool TcpShutdownPending{false};
    bool TcpClosePending{false};
    bool TcpCloseCompleted{false};

    std::deque<std::shared_ptr<TqUvPendingQuicReceive>> PrecommitReceives;
    std::shared_ptr<TqUvPendingQuicReceive> FallbackReceiveHead;
    std::shared_ptr<TqUvPendingQuicReceive> FallbackReceiveTail;
    bool ActiveReceiveFallbackMode{false};
    bool PrecommitSettled{false};
    std::uint64_t PrecommitDrainCount{0};
    std::uint64_t PrecommitDiscardCount{0};
    std::unordered_map<uv_write_t*, std::unique_ptr<TqUvTcpWriteOperation>>
        TcpWrites;
    std::unordered_map<void*, std::unique_ptr<TqUvQuicSendOperation>> QuicSends;
    TqRelayBufferBudget TcpReadBufferBudget;
    std::unordered_map<char*, TqBufferRef> TcpReadBuffers;
    std::vector<std::uint8_t> CompressionOutput;
    std::uint64_t PendingQuicReceiveBytes{0};
    std::atomic<std::uint64_t> AdmittedQuicReceiveBytes{0};
    std::atomic<std::uint64_t> QuicToTcpPressureBytes{0};
    std::uint64_t PrecommitMaxPendingBytes{0};
    std::uint64_t PendingTcpWriteBytes{0};
    std::uint64_t PendingQuicSendBytes{0};
    std::uint64_t PendingTcpReadBytes{0};
    TqRelayBufferBudget BufferBudget;
    TqBufferAcquireFailure TcpReadAcquireFailure{
        TqBufferAcquireFailure::None};
    std::uint64_t MaxBufferedQuicSendBytes{0};
    std::uint64_t ResumeBufferedQuicSendBytes{0};
    std::size_t TcpReadChunkSize{0};
    std::uint32_t QuicToTcpCallBudget{4096};
    std::uint64_t QuicToTcpByteBudgetPerTick{64ull * 1024 * 1024};
    bool QuicToTcpContinuationPending{false};
    std::deque<std::unique_ptr<TqUvQuicSendOperation>> PendingQuicSendRetries;
    // QUIC RECEIVE admission runs on MsQuic callback threads while
    // completion runs on the owning libuv loop.  Keep the directional
    // ownership ledger atomic so concurrent admission/completion cannot lose
    // an update and strand worker pending-byte accounting at terminal.
    std::atomic<std::uint64_t> AccountedQuicToTcpBytes{0};
    std::atomic<std::uint64_t> AccountedTcpToQuicBytes{0};
    bool MetricsActive{false};
    bool TcpReadPausedByQuicBacklog{false};
    bool QuicReceivePausedByTcpBacklog{false};
    std::atomic<bool> QuicFinObserved{false};
    bool QuicFinSubmitted{false};
    bool QuicFinCompleted{false};
    bool TerminalStarted{false};
    bool TerminalAborted{false};
    bool TerminalHandoffStarted{false};
    bool TerminalHandoffSubmitted{false};
    bool TerminalHandoffRetryScheduled{false};
    bool TerminalHandoffRetryPending{false};
    bool TerminalHandoffEscalated{false};
    bool TerminalAbortUpgradePending{false};
    bool TerminalAbortUpgradeApplied{false};
    QUIC_STATUS TerminalHandoffStatus{QUIC_STATUS_SUCCESS};
    std::atomic<bool> TerminalHandoffComplete{false};
    bool LocalOwnershipDrained{false};
    bool TerminalReleased{false};
    std::atomic<std::uint32_t> TerminalTriggerMask{0};
    std::atomic<bool> QuicShutdownObserved{false};
    std::atomic<std::uint64_t> TerminalBeginCount{0};
    std::atomic<std::uint64_t> TerminalReleaseCount{0};
#if defined(TQ_UNIT_TESTING) && TQ_UNIT_TESTING
    bool DirectQuicReceiveForTest{false};
    std::uint32_t FailActiveReceiveQueueAdmissionsForTest{0};
#endif
};

QUIC_STATUS TqUvAcceptQuicReceive(
    const std::shared_ptr<TqUvRelayState>&,
    MsQuicStream*,
    const QUIC_STREAM_EVENT&) noexcept;
void TqUvProcessQuicToTcp(TqUvRelayWorker&, TqUvRelayState&);
void TqUvSettleQuicReceivesAtTerminal(TqUvRelayState&) noexcept;

#if defined(TQ_UNIT_TESTING) && TQ_UNIT_TESTING
struct TqUvQuicToTcpSnapshotForTest {
    std::uint64_t PendingQuicReceiveBytes{0};
    std::uint64_t PendingTcpWriteBytes{0};
    std::uint64_t PendingReceives{0};
    std::uint64_t PendingWrites{0};
    std::uint64_t PressureBytes{0};
    bool ReceivePaused{false};
};
TqUvQuicToTcpSnapshotForTest TqUvQuicToTcpSnapshot(
    const TqUvRelayState&) noexcept;
#endif
void TqUvHandleTcpRead(
    TqUvRelayWorker&,
    TqUvRelayState&,
    ssize_t,
    const uv_buf_t&);
void TqUvHandleSendComplete(
    TqUvRelayWorker&,
    TqUvQuicSendOperation&,
    bool cancelled) noexcept;
void TqUvRetryPendingQuicSends(
    TqUvRelayWorker&, TqUvRelayState&) noexcept;
bool TqUvAllocateTcpReadBuffer(
    TqUvRelayState&,
    std::size_t,
    uv_buf_t&,
    TqBufferAcquireFailure* failure = nullptr) noexcept;
void TqUvCheckTerminalConvergence(
    TqUvRelayWorker&, TqUvRelayState&) noexcept;
void TqUvRequestTerminal(
    TqUvRelayState&,
    TqUvTerminalTrigger) noexcept;
void TqUvProcessTerminalFactsLocal(
    TqUvRelayWorker&,
    TqUvRelayState&) noexcept;
bool TqUvLocalOperationOwnershipDrained(
    const TqUvRelayState&) noexcept;
void TqUvBeginTerminalLocal(
    TqUvRelayWorker&,
    TqUvRelayState&,
    TqUvTerminalTrigger) noexcept;

void TqUvOnTcpAlloc(uv_handle_t*, std::size_t, uv_buf_t*);
void TqUvOnTcpRead(uv_stream_t*, ssize_t, const uv_buf_t*);
void TqUvOnTcpWriteComplete(uv_write_t*, int) noexcept;
void TqUvOnTcpShutdownComplete(uv_shutdown_t*, int);
void TqUvOnTcpClosed(uv_handle_t*);

#if defined(TQ_UNIT_TESTING) && TQ_UNIT_TESTING
using TqUvStreamSendHookForTest = QUIC_STATUS (*)(
    MsQuicStream*,
    const QUIC_BUFFER*,
    std::uint32_t,
    QUIC_SEND_FLAGS,
    void*);
void TqUvSetStreamSendHookForTest(TqUvStreamSendHookForTest) noexcept;
using TqUvTerminalHookForTest = void (*)(
    TqUvRelayState&,
    TqUvTerminalTrigger);
using TqUvConvergenceHookForTest = void (*)(TqUvRelayState&);
void TqUvSetTerminalHookForTest(TqUvTerminalHookForTest) noexcept;
void TqUvSetConvergenceHookForTest(TqUvConvergenceHookForTest) noexcept;
uv_buf_t TqUvStageTcpReadBufferForTest(
    TqUvRelayState&,
    std::size_t);
#endif
