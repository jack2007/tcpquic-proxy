#pragma once

#include "libuv_relay_event_queue.h"
#include "libuv_relay_internal.h"
#include "tuning.h"

#include <uv.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <limits>
#include <mutex>
#include <thread>
#include <variant>
#include <vector>

#if defined(TQ_UNIT_TESTING) && TQ_UNIT_TESTING
struct TqUvCallAdapter {
    int (*LoopInit)(uv_loop_t*);
    int (*LoopClose)(uv_loop_t*);
    int (*AsyncInit)(uv_loop_t*, uv_async_t*, uv_async_cb);
    int (*AsyncSend)(uv_async_t*);
    int (*TcpInit)(uv_loop_t*, uv_tcp_t*);
    int (*TcpOpen)(uv_tcp_t*, uv_os_sock_t);
    int (*ReadStart)(uv_stream_t*, uv_alloc_cb, uv_read_cb);
    int (*ReadStop)(uv_stream_t*);
    int (*Write)(uv_write_t*, uv_stream_t*, const uv_buf_t[], unsigned int, uv_write_cb);
    int (*Shutdown)(uv_shutdown_t*, uv_stream_t*, uv_shutdown_cb);
    void (*Close)(uv_handle_t*, uv_close_cb);
};

const TqUvCallAdapter& TqUvProductionCalls() noexcept;
#endif

struct TqUvRelayWorkerConfig {
    std::uint32_t WorkerIndex{0};
    std::size_t QueueCapacity{4096};
    std::uint32_t ControlCommandTimeoutMs{5000};
#if defined(TQ_UNIT_TESTING) && TQ_UNIT_TESTING
    const TqUvCallAdapter* Calls{nullptr};
    std::atomic<std::uint64_t>* LoopInitCallsForTest{nullptr};
    int (*QueueMutexInitForTest)(uv_mutex_t*){nullptr};
    void (*BeforeQueuePopForTest)(){nullptr};
    void (*BeforeCloseRelayForTest)(std::size_t){nullptr};
    TqUvRelayRegistrationResult (*RegisterLocalForTest)(
        TqUvRelayWorker&,
        TqUvRelayRegistration){nullptr};
    void (*AfterAdmissionCheckForTest)(){nullptr};
    bool FailCommittedAllocationForTest{false};
    bool FailRelayMapInsertForTest{false};
    bool FailStopScanOnceForTest{false};
    void (*BeforePendingOverflowReturnForTest)(){nullptr};
    void (*AfterSafetyTimerForTest)(TqUvRelayWorker&){nullptr};
    bool (*FailDeferredPostForTest)(){nullptr};
#endif
};

struct TqUvRelayWorkerSnapshot {
    std::uint32_t WorkerIndex{0};
    bool Running{false};
    std::uint64_t PendingCommands{0};
    std::uint64_t AsyncWakeAttempts{0};
    std::uint64_t AsyncWakeSuccesses{0};
    std::uint64_t AsyncWakeFailures{0};
    std::uint64_t AsyncWakeCoalesced{0};
    std::uint64_t AsyncCallbacks{0};
    std::uint64_t SafetyTimerCallbacks{0};
    std::uint64_t LoopIterations{0};
    std::uint64_t CommandsExecuted{0};
    std::uint64_t ActiveRelays{0};
    std::uint64_t PendingBytes{0};
    std::uint64_t QuicToTcpPendingBytes{0};
    std::uint64_t TcpToQuicPendingBytes{0};
    std::uint64_t LoopLagMicros{0};
    std::uint64_t StopScanPasses{0};
    std::uint64_t StopScanFailures{0};
    std::uint64_t SendCompletionCommands{0};
    std::uint64_t SendCompletionFallbacks{0};
    std::uint64_t EventQueueCapacity{0};
    std::uint64_t TcpReadBytes{0};
    std::uint64_t TcpWriteBytes{0};
    std::uint64_t CompressedTcpBytes{0};
    std::uint64_t DecompressedTcpBytes{0};
    std::uint64_t ZstdDecompressInputBytes{0};
    std::uint64_t ZstdDecompressOutputBytes{0};
    std::uint64_t ZstdDecompressCalls{0};
    std::uint64_t ZstdDecompressNeedInput{0};
    std::uint64_t ZstdDecompressNeedOutput{0};
    std::uint64_t ZstdDecompressFailures{0};
    std::uint64_t TcpToQuicCompressFailures{0};
    std::uint64_t QuicReceiveDecompressFailures{0};
};

std::uint64_t TqUvLoopLagMicros(
    std::uint64_t previousNanos,
    std::uint64_t nowNanos,
    std::uint64_t intervalNanos) noexcept;

#if defined(TQ_UNIT_TESTING) && TQ_UNIT_TESTING
struct TqUvRegistrationTestSnapshot {
    TqUvActivation Activation{TqUvActivation::Failed};
    TqUvSocketOwnership Ownership{TqUvSocketOwnership::CallerOwned};
    bool PrecommitSettled{false};
    std::uint64_t PrecommitDrainCount{0};
    std::uint64_t PrecommitDiscardCount{0};
};
#endif

enum class TqUvRegisterCommandState : std::uint8_t {
    Queued,
    Executing,
    Completed,
    Cancelled,
};

class TqUvRelayWorker final {
public:
    using LocalCommand = std::function<void(TqUvRelayWorker&)>;

#if defined(TQ_UNIT_TESTING) && TQ_UNIT_TESTING
    explicit TqUvRelayWorker(TqUvRelayWorkerConfig config);
#endif
    ~TqUvRelayWorker();
    TqUvRelayWorker(const TqUvRelayWorker&) = delete;
    TqUvRelayWorker& operator=(const TqUvRelayWorker&) = delete;

    bool StartAndWaitReady();
    bool Post(LocalCommand command);
    // Always queues, including from the loop thread. HandleWake snapshots the
    // command count, so this cannot execute until a later loop turn.
    bool PostDeferred(LocalCommand command);
    TqUvRelayRegistrationResult RegisterRelayWithId(
        TqUvRelayRegistration registration);
    bool AcceptStop(
        const std::shared_ptr<const TqUvRelayCommittedState>& committed);
    bool QueueSendComplete(
        TqUvQuicSendOperation* operation,
        bool cancelled) noexcept;
    void WakeForDurableFacts() noexcept;
    TqUvRelayWorkerSnapshot Snapshot() const;
    // Caller serializes relay state. Account a direction once on admission and
    // complete it once on write/send completion or terminal discard.
    bool AddPendingBytes(
        TqUvRelayState& relay,
        TqUvPendingDirection direction,
        std::uint64_t bytes) noexcept;
    bool CompletePendingBytes(
        TqUvRelayState& relay,
        TqUvPendingDirection direction,
        std::uint64_t bytes) noexcept;
    std::uint32_t WorkerIndex() const noexcept { return Config_.WorkerIndex; }

#if defined(TQ_UNIT_TESTING) && TQ_UNIT_TESTING
    bool StopForTest();
    bool StopForTest(std::chrono::steady_clock::time_point deadline);
    bool StopRequestedForTest() const noexcept;
    bool IsLoopThreadForTest() const noexcept;
    TqUvRegisterCommandState RegistrationStateForTest() const;
    TqUvRegistrationTestSnapshot RegistrationSnapshotForTest(
        std::uint64_t relayId) const;
    void SignalRegistrationForTest();
    int CallTcpInitForTest(uv_loop_t* loop, uv_tcp_t* tcp);
    int CallTcpOpenForTest(uv_tcp_t* tcp, uv_os_sock_t socket);
    int CallReadStartForTest(
        uv_stream_t* stream,
        uv_alloc_cb allocate,
        uv_read_cb read);
    int CallWriteForTest(
        uv_write_t* request,
        uv_stream_t* stream,
        const uv_buf_t buffers[],
        unsigned int bufferCount,
        uv_write_cb complete);
    int CallShutdownForTest(
        uv_shutdown_t* request,
        uv_stream_t* stream,
        uv_shutdown_cb complete);
    std::shared_ptr<TqUvRelayState> RelayForTest(std::uint64_t relayId) const;
#endif

private:
    friend class TqUvRelayRuntime;
    friend void TqUvProcessQuicToTcp(TqUvRelayWorker&, TqUvRelayState&);
    friend void TqUvHandleTcpRead(
        TqUvRelayWorker&,
        TqUvRelayState&,
        ssize_t,
        const uv_buf_t&);
    friend void TqUvHandleSendComplete(
        TqUvRelayWorker&,
        TqUvQuicSendOperation&,
        bool) noexcept;
    friend void TqUvOnTcpWriteComplete(uv_write_t*, int) noexcept;
    friend void TqUvRetryPendingQuicSends(
        TqUvRelayWorker&,
        TqUvRelayState&) noexcept;
    friend void TqUvProcessTerminalFactsLocal(
        TqUvRelayWorker&,
        TqUvRelayState&) noexcept;
    friend void TqUvBeginTerminalLocal(
        TqUvRelayWorker&,
        TqUvRelayState&,
        TqUvTerminalTrigger) noexcept;
    friend void TqUvCheckTerminalConvergence(
        TqUvRelayWorker&, TqUvRelayState&) noexcept;
    friend void TqUvOnTcpClosed(uv_handle_t*);

#if !defined(TQ_UNIT_TESTING) || !TQ_UNIT_TESTING
    // Production construction is runtime-only so allocator bootstrap always
    // precedes the queue's first libuv synchronization object.
    explicit TqUvRelayWorker(TqUvRelayWorkerConfig config);
#endif

    struct RegisterCommand;
    struct SendCompleteCommand {
        TqUvQuicSendOperation* Operation{nullptr};
    };
    using Command = std::variant<
        LocalCommand,
        std::shared_ptr<RegisterCommand>,
        SendCompleteCommand>;

    static void ThreadEntry(void* context);
    static void AsyncEntry(uv_async_t* async);
    static void SafetyTimerEntry(uv_timer_t* timer);
    static void PrepareEntry(uv_prepare_t* prepare);
    static void AsyncClosed(uv_handle_t* handle);
    void StopForStartupRollback();
    void PostLocal(LocalCommand command);
    TqUvRelayRegistrationResult RegisterRelayWithIdLocal(
        TqUvRelayRegistration registration);
    void ThreadMain();
    void HandleWake() noexcept;
    void DrainSendCompletionFallbacks() noexcept;
    void RetryPendingQuicSends() noexcept;
    void ProcessSignaledStops() noexcept;
    void BeginCloseHandles(
        TqUvTerminalTrigger trigger = TqUvTerminalTrigger::RuntimeStop)
        noexcept;
    void DispatchCommand(Command& command);
    bool Enqueue(Command command);
    bool WakeLoopUntilAccepted();
    bool WakeLoopUntilAcceptedLocked();
    void WakeLoopAndDisable() noexcept;
    void TrackRegistration(const std::shared_ptr<RegisterCommand>& command);
    void UntrackRegistration(const std::shared_ptr<RegisterCommand>& command);
    void CancelQueuedRegistrations();
    int CallLoopInit(uv_loop_t* loop);
    int CallLoopClose(uv_loop_t* loop);
    int CallAsyncInit(uv_loop_t* loop, uv_async_t* async, uv_async_cb callback);
    int CallAsyncSend(uv_async_t* async);
    int CallTcpInit(uv_loop_t* loop, uv_tcp_t* tcp);
    int CallTcpOpen(uv_tcp_t* tcp, uv_os_sock_t socket);
    int CallReadStart(
        uv_stream_t* stream,
        uv_alloc_cb allocate,
        uv_read_cb read);
    int CallReadStop(uv_stream_t* stream);
    int CallWrite(
        uv_write_t* request,
        uv_stream_t* stream,
        const uv_buf_t buffers[],
        unsigned int bufferCount,
        uv_write_cb complete);
    int CallShutdown(
        uv_shutdown_t* request,
        uv_stream_t* stream,
        uv_shutdown_cb complete);
    void CallClose(uv_handle_t* handle, uv_close_cb callback);
    void CloseRelay(const std::shared_ptr<TqUvRelayState>& relay);
    void PublishActiveRelay(TqUvRelayState& relay) noexcept;
    void EraseRelay(std::uint64_t relayId) noexcept;
    void SettlePrecommit(
        const std::shared_ptr<TqUvRelayState>& relay,
        bool drain);
    void RecordRegistrationSnapshotForTest(const TqUvRelayState& relay);
    void StopAndJoin();
    void StopAndJoin(std::chrono::steady_clock::time_point deadline);
    void ReportReady(int status);
    bool IsLoopThread() const noexcept;

    TqUvRelayWorkerConfig Config_;
#if defined(TQ_UNIT_TESTING) && TQ_UNIT_TESTING
    const TqUvCallAdapter* CallsForTest_{nullptr};
#endif
    TqUvRelayEventQueue<Command> Queue_;
    uv_loop_t Loop_{};
    uv_async_t Async_{};
    uv_timer_t SafetyTimer_{};
    uv_prepare_t Prepare_{};
    uv_thread_t Thread_{};
    uv_mutex_t LifecycleMutex_{};
    uv_cond_t LifecycleCondition_{};
    bool LifecycleSyncInitialized_{false};
    bool ThreadCreated_{false};
    bool ReadyReported_{false};
    int ReadyStatus_{UV_EINVAL};
    std::thread::id LoopThreadId_{};
    std::atomic<bool> Running_{false};
    std::atomic<bool> Accepting_{false};
    std::atomic<bool> StopRequested_{false};
    std::atomic<bool> RuntimeStopRequested_{false};
    std::atomic<bool> AsyncSendAllowed_{true};
    std::atomic<std::uint64_t> RuntimeStopDeadlineNs_{
        std::numeric_limits<std::uint64_t>::max()};
    std::atomic<bool> CloseRequested_{false};
    bool AsyncCloseStarted_{false};
    bool PrepareCloseStarted_{false};
    bool SafetyTimerCloseStarted_{false};
    std::atomic<std::uint64_t> AsyncWakeSuccesses_{0};
    std::atomic<std::uint64_t> AsyncWakeFailures_{0};
    std::atomic<std::uint64_t> AsyncWakeCoalesced_{0};
    std::atomic<std::uint64_t> AsyncCallbacks_{0};
    std::atomic<std::uint64_t> SafetyTimerCallbacks_{0};
    std::atomic<std::uint64_t> LoopIterations_{0};
    std::atomic<std::uint64_t> LoopLagMicros_{0};
    std::uint64_t LastSafetyTimerNanos_{0};
    std::atomic<std::uint64_t> CommandsExecuted_{0};
    std::atomic<std::uint64_t> ActiveRelays_{0};
    std::atomic<std::uint64_t> QuicToTcpPendingBytes_{0};
    std::atomic<std::uint64_t> TcpToQuicPendingBytes_{0};
    std::atomic<std::uint64_t> StopRequestSequence_{0};
    std::uint64_t ObservedStopSequence_{0};
    std::atomic<std::uint64_t> StopScanPasses_{0};
    std::atomic<std::uint64_t> StopScanFailures_{0};
    std::atomic<std::uint64_t> SendCompletionCommands_{0};
    std::atomic<std::uint64_t> SendCompletionFallbacks_{0};
    std::atomic<std::uint64_t> TcpReadBytes_{0};
    std::atomic<std::uint64_t> TcpWriteBytes_{0};
    std::atomic<std::uint64_t> CompressedTcpBytes_{0};
    std::atomic<std::uint64_t> DecompressedTcpBytes_{0};
    std::atomic<std::uint64_t> ZstdDecompressInputBytes_{0};
    std::atomic<std::uint64_t> ZstdDecompressOutputBytes_{0};
    std::atomic<std::uint64_t> ZstdDecompressCalls_{0};
    std::atomic<std::uint64_t> ZstdDecompressNeedInput_{0};
    std::atomic<std::uint64_t> ZstdDecompressNeedOutput_{0};
    std::atomic<std::uint64_t> ZstdDecompressFailures_{0};
    std::atomic<std::uint64_t> TcpToQuicCompressFailures_{0};
    std::atomic<std::uint64_t> QuicReceiveDecompressFailures_{0};
    std::atomic<TqUvQuicSendOperation*> SendCompletionFallbackHead_{nullptr};
#if defined(TQ_UNIT_TESTING) && TQ_UNIT_TESTING
    bool FailStopScanOnceForTest_{false};
#endif
    std::mutex AdmissionMutex_;
    std::mutex AsyncSendMutex_;
    std::mutex PendingRegistrationMutex_;
    std::vector<std::weak_ptr<RegisterCommand>> PendingRegistrations_;
    std::unordered_map<std::uint64_t, std::shared_ptr<TqUvRelayState>> Relays_;
    std::uint64_t NextRelayId_{1};
#if defined(TQ_UNIT_TESTING) && TQ_UNIT_TESTING
    mutable std::mutex TestRegistrationMutex_;
    std::weak_ptr<RegisterCommand> TestRegistration_;
    mutable std::mutex RegistrationSnapshotMutex_;
    std::unordered_map<std::uint64_t, TqUvRegistrationTestSnapshot>
        RegistrationSnapshots_;
#endif
};

enum class TqUvRelayRuntimeState : std::uint8_t {
    Stopped,
    Starting,
    Running,
    Draining,
    Closed,
};

class TqUvRelayRuntime final {
public:
    static TqUvRelayRuntime& Instance();

    bool Start(const TqTuningConfig& tuning);
#if defined(TQ_UNIT_TESTING) && TQ_UNIT_TESTING
    bool Stop(
        std::chrono::steady_clock::time_point deadline =
            std::chrono::steady_clock::time_point::max());
#endif
    TqUvRelayWorker* PickWorker();
    bool StopRelay(const std::shared_ptr<const TqUvRelayCommittedState>& committed);
    std::vector<TqUvRelayWorkerSnapshot> SnapshotWorkers() const;

#if defined(TQ_UNIT_TESTING) && TQ_UNIT_TESTING
    using AllocatorInstaller = bool (*)() noexcept;
    void StopForTest();
    std::size_t WorkerCountForTest() const;
    bool AllocatorInstalledBeforeLoopForTest() const;
    std::uint64_t LoopInitCallsForTest() const;
    std::uint64_t StartedWorkersForTest() const;
    std::uint64_t RolledBackWorkersForTest() const;
    void SetAllocatorInstallerForTest(AllocatorInstaller installer);
    void SetCallAdapterForTest(const TqUvCallAdapter* calls);
    void ResetTestHooksForTest();
#endif

private:
    TqUvRelayRuntime() = default;
    ~TqUvRelayRuntime();
    TqUvRelayRuntime(const TqUvRelayRuntime&) = delete;
    TqUvRelayRuntime& operator=(const TqUvRelayRuntime&) = delete;

    static TqUvRelayWorkerConfig MakeWorkerConfig(
        const TqTuningConfig& tuning,
        std::uint32_t index
#if defined(TQ_UNIT_TESTING) && TQ_UNIT_TESTING
        , const TqUvCallAdapter* calls,
        std::atomic<std::uint64_t>* loopInitCalls
#endif
    );

    mutable std::mutex Lock_;
    TqUvRelayRuntimeState State_{TqUvRelayRuntimeState::Stopped};
    std::vector<std::unique_ptr<TqUvRelayWorker>> Workers_;
    std::size_t NextWorker_{0};
    bool AllocatorInstalledBeforeLoop_{false};
#if defined(TQ_UNIT_TESTING) && TQ_UNIT_TESTING
    AllocatorInstaller AllocatorInstallerForTest_{nullptr};
    const TqUvCallAdapter* CallsForTest_{nullptr};
    std::atomic<std::uint64_t> LoopInitCallsForTest_{0};
    std::uint64_t StartedWorkersForTest_{0};
    std::uint64_t RolledBackWorkersForTest_{0};
#endif
};
