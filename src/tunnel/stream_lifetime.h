#pragma once

#include "terminal_convergence.h"
#include <msquic.hpp>

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <vector>

// relay-capable QUIC stream 的唯一 wrapper 生命周期与 callback 路由器。
//
// wrapper 必须从创建时使用 CleanUpManual；Callback/Context 只由 factory
// 安装一次。业务 handoff 通过 PublishTarget 完成，不再修改 wrapper 字段。
class TqStreamLifetime final : public std::enable_shared_from_this<TqStreamLifetime> {
public:
    enum class Phase : uint8_t {
        CreatedNotStarted,
        Starting,
        Started,
        StartFailed,
        TerminalPublished,
        Closed,
    };
    enum class ShutdownIntent : uint8_t {
        GracefulSend,
        AbortSend,
        AbortReceive,
        AbortBoth,
        AbortBothImmediate,
    };
#if defined(TQ_UNIT_TESTING)
    using ShutdownHookForTest = std::function<QUIC_STATUS(
        uint64_t, QUIC_STREAM_SHUTDOWN_FLAGS)>;
    using BeforeTerminalLedgerRecordHookForTest = std::function<void()>;
#endif

    class Target {
    public:
        virtual ~Target() = default;
        virtual QUIC_STATUS OnStreamEvent(
            MsQuicStream* stream,
            QUIC_STREAM_EVENT* event,
            uint64_t routeGeneration) noexcept = 0;
#if defined(TQ_UNIT_TESTING)
        virtual void* ContextForTest() const noexcept { return nullptr; }
#endif
    };

    class ApiLease final {
    public:
        ApiLease() = default;
        MsQuicStream* Stream() const noexcept;
        explicit operator bool() const noexcept { return Owner_ != nullptr; }

    private:
        friend class TqStreamLifetime;
        explicit ApiLease(std::shared_ptr<TqStreamLifetime> owner) noexcept
            : Owner_(std::move(owner)) {}
        std::shared_ptr<TqStreamLifetime> Owner_;
    };

    struct RouteSnapshot {
        std::shared_ptr<Target> TargetOwner;
        uint64_t Generation{0};
    };
    struct RetentionSnapshot {
        uint64_t OwnerCount{0};
        uint64_t OldestAgeMs{0};
    };
    struct SendCompletionSnapshot {
        uint64_t ActiveCount{0};
        uint64_t OldestAgeMs{0};
        uint64_t PreSubmitRollbacks{0};
        uint64_t UnknownClaims{0};
        uint64_t DuplicateClaims{0};
    };

    // Move-only RAII guard for a registered send completion key. Armed
    // destructor calls CancelSendCompletion(); Dismiss() only after Send is
    // submitted or the completion callback has claimed the operation.
    class SendCompletionReservation {
    public:
        SendCompletionReservation() noexcept = default;
        SendCompletionReservation(SendCompletionReservation&& other) noexcept;
        SendCompletionReservation& operator=(SendCompletionReservation&& other) noexcept;
        ~SendCompletionReservation() noexcept;
        SendCompletionReservation(const SendCompletionReservation&) = delete;
        SendCompletionReservation& operator=(const SendCompletionReservation&) = delete;

        explicit operator bool() const noexcept {
            return Armed_ && Key_ != nullptr && Owner_ != nullptr;
        }
        void* Key() const noexcept { return Key_; }
        void Dismiss() noexcept;
        bool Cancel() noexcept;

    private:
        friend class TqStreamLifetime;
        SendCompletionReservation(
            std::shared_ptr<TqStreamLifetime> owner,
            void* key) noexcept;
        std::shared_ptr<TqStreamLifetime> Owner_;
        void* Key_{nullptr};
        bool Armed_{false};
    };
    struct RegistrySnapshot {
        uint64_t SendCompletionCount{0};
        uint64_t TerminalApiSuppressedCount{0};
    };

    static std::shared_ptr<TqStreamLifetime> OpenOutgoing(
        const MsQuicConnection& connection,
        QUIC_STREAM_OPEN_FLAGS flags,
        std::shared_ptr<TqStreamLifetime::Target> initialTarget,
        TqTerminalIdentity identity,
        uint32_t watchdogSeconds) noexcept;

    static std::shared_ptr<TqStreamLifetime> AdoptAccepted(
        HQUIC rawStream,
        std::shared_ptr<TqStreamLifetime::Target> initialTarget,
        TqTerminalIdentity identity,
        uint32_t watchdogSeconds) noexcept;

#if defined(TQ_UNIT_TESTING)
    // 状态机测试不依赖 MsQuic runtime；生产 factory 始终提供真实 wrapper。
    static std::shared_ptr<TqStreamLifetime> CreateForTest(
        Phase phase,
        std::shared_ptr<Target> initialTarget = nullptr);
    bool InstallStreamForTest(MsQuicStream* stream) noexcept;
    void ReleaseStreamForTest() noexcept;
    // 单元测试可将 placement-new 的 detached MsQuicStream 挂到 owner 上。
    bool InstallDetachedStreamForTest(MsQuicStream* stream) noexcept;
    void* TargetContextForTest() const noexcept;
    static void SetFailNextRegisterSendCompletionForTest(bool fail) noexcept;
    static void ResetTestDetachedOwnerDestroyCountForTest() noexcept;
    static uint64_t TestDetachedOwnerDestroyCountForTest() noexcept;
    bool SendDirectionCompleteForTest() const noexcept;
    uint64_t CancelOnLossErrorCodeForTest() const noexcept;
    void SetShutdownHookForTest(ShutdownHookForTest hook) noexcept;
    void SetBeforeTerminalLedgerRecordHookForTest(
        BeforeTerminalLedgerRecordHookForTest hook) noexcept;
    bool TerminalRetryOwnedForTest() const noexcept;
#endif
#if defined(TQ_UNIT_TESTING) || defined(TCPQUIC_TUNNEL_TESTING)
    QUIC_STATUS DispatchForTest(QUIC_STREAM_EVENT* event) noexcept;
#endif

    ~TqStreamLifetime() noexcept;
    TqStreamLifetime(const TqStreamLifetime&) = delete;
    TqStreamLifetime& operator=(const TqStreamLifetime&) = delete;

    Phase GetPhase() const noexcept;
    uint64_t RouteGeneration() const noexcept;
    MsQuicStream* StreamForInitialization() const noexcept { return Stream_; }

    bool BeginStart() noexcept;
    bool CompleteStart(QUIC_STATUS status) noexcept;
    ApiLease TryAcquireApi() noexcept;
    // relay 数据面 API：允许 detached unit-test wrapper，仍拒绝 terminal/closed。
    ApiLease TryAcquireReceiveApi() noexcept;
    ApiLease TryAcquireSendApi() noexcept;
    bool ReceiveDirectionAborted() const noexcept;
    // ClientContext 仅作为 opaque key。成功注册的 operation 在全局 completion
    // retention 中强持 owner 和提交时 route snapshot，直到 once-only claim。
    void* RegisterSendCompletion(
        void* deliveredContext,
        std::function<void()> completionCleanup = {}) noexcept;
    // Registers a completion key and returns an armed reservation. Prefer this
    // over bare RegisterSendCompletion for pre-submit paths that may return
    // before MsQuic Send / callback claim.
    SendCompletionReservation ReserveSendCompletion(
        void* deliveredContext,
        std::function<void()> completionCleanup = {}) noexcept;
    bool CancelSendCompletion(void* clientContext) noexcept;
#if defined(TQ_UNIT_TESTING)
    bool InjectSendCompletionForTest(
        void* clientContext,
        void* deliveredContext,
        std::function<void()> completionCleanup = {}) noexcept;
    void DenyReceiveApiLeasesForTest(uint32_t count) noexcept;
#endif
    QUIC_STATUS RequestShutdown(ShutdownIntent intent, uint64_t errorCode = 0) noexcept;
    void BindTerminalIdentity(
        TqTerminalIdentity identity,
        uint32_t watchdogSeconds = 5) noexcept;
    std::shared_ptr<TqTerminalLedger> TerminalLedger() const noexcept;
    TerminalPhase GetTerminalPhase() const noexcept;
    TqTerminalShutdownResult BeginTerminalShutdown(
        uint64_t errorCode,
        std::shared_ptr<TqStreamLifetime::Target> terminalSink,
        std::shared_ptr<TqTerminalEscalation> escalation) noexcept;

    bool PublishTarget(
        uint64_t expectedGeneration,
        std::shared_ptr<Target> target,
        uint64_t* publishedGeneration = nullptr) noexcept;
    RouteSnapshot PublishStartFailureAndTakeTarget(QUIC_STATUS status) noexcept;
    RouteSnapshot PublishTerminalAndTakeTarget() noexcept;

    static QUIC_STATUS QUIC_API Callback(
        MsQuicStream* stream,
        void* context,
        QUIC_STREAM_EVENT* event) noexcept;
    static void RejectAccepted(HQUIC rawStream) noexcept;
    static RetentionSnapshot SnapshotTerminalRetentions() noexcept;
    static SendCompletionSnapshot SnapshotSendCompletions() noexcept;
    static void RecordUnknownSendClaim() noexcept;
    static void RecordDuplicateSendClaim() noexcept;
    static RegistrySnapshot SnapshotRegistries() noexcept;
#if defined(TQ_UNIT_TESTING)
    static void ResetLifecycleRegistriesForTest() noexcept;
    static void SetBeforeTerminalRetentionSnapshotForTest(
        std::function<void()> hook) noexcept;
#endif
    QUIC_STATUS DispatchBufferedEvent(QUIC_STREAM_EVENT* event) noexcept;

private:
    friend class TqTerminalScheduler;
    friend struct TqTerminalSchedulerInternals;
    TqStreamLifetime(Phase phase, std::shared_ptr<Target> initialTarget) noexcept;
    bool InstallStream(MsQuicStream* stream) noexcept;
    void RetainUntilTerminal();
    void RetainUntilTerminalLocked();
    void ReleaseTerminalRetention();
    QUIC_STATUS CallTerminalShutdown(
        MsQuicStream* stream,
        uint64_t errorCode,
        QUIC_STREAM_SHUTDOWN_FLAGS flags) noexcept;
    TqTerminalShutdownResult RetryTerminalShutdown() noexcept;
    QUIC_STATUS Dispatch(MsQuicStream* stream, QUIC_STREAM_EVENT* event) noexcept;

    mutable std::mutex ControlMutex_;
    Phase Phase_;
    MsQuicStream* Stream_{nullptr};
    std::shared_ptr<Target> Target_;
    uint64_t RouteGeneration_{1};
    QUIC_STATUS StartFailureStatus_{QUIC_STATUS_SUCCESS};
    bool TerminalRetained_{false};
    bool DetachedStreamForTest_{false};
    uint8_t DesiredSendShutdown_{0}; // 0=open, 1=graceful, 2=abort
    uint8_t SubmittedSendShutdown_{0};
    uint8_t ReservedSendShutdown_{0};
    bool DesiredReceiveAbort_{false};
    bool SubmittedReceiveAbort_{false};
    bool ReservedReceiveAbort_{false};
    bool DesiredImmediate_{false};
    bool SubmittedImmediate_{false};
    bool ReservedImmediate_{false};
    bool SendDirectionComplete_{false};
    uint64_t CancelOnLossErrorCode_{0};
    std::shared_ptr<TqTerminalLedger> TerminalLedger_;
    TerminalPhase TerminalPhase_{TerminalPhase::Active};
    std::shared_ptr<TqTerminalEscalation> TerminalEscalation_;
    uint64_t TerminalErrorCode_{0};
    uint32_t TerminalShutdownAttempt_{0};
    bool TerminalRetryOwned_{false};
    uint32_t TerminalWatchdogSeconds_{5};
    std::shared_ptr<Target> TerminalSink_;
    std::vector<std::unique_ptr<uint64_t>> SendKeyEnvelopes_;
#if defined(TQ_UNIT_TESTING)
    uint32_t DenyReceiveApiLeasesForTest_{0};
    ShutdownHookForTest ShutdownHookForTest_;
    BeforeTerminalLedgerRecordHookForTest BeforeTerminalLedgerRecordHookForTest_;
#endif
};

class TqTerminalSink final : public TqStreamLifetime::Target {
public:
    static std::shared_ptr<TqTerminalSink> Create(
        std::weak_ptr<TqStreamLifetime> owner,
        std::shared_ptr<TqTerminalLedger> ledger) noexcept;
#if defined(TQ_UNIT_TESTING)
    static void SetFailNextControlBlockForTest(bool fail) noexcept;
#endif
    ~TqTerminalSink() noexcept override;
    QUIC_STATUS OnStreamEvent(
        MsQuicStream*, QUIC_STREAM_EVENT*, uint64_t) noexcept override;

private:
    TqTerminalSink(
        std::weak_ptr<TqStreamLifetime> owner,
        std::shared_ptr<TqTerminalLedger> ledger) noexcept;
    void ReleasePendingOnce() noexcept;
    void ArmPending() noexcept;
    std::weak_ptr<TqStreamLifetime> Owner_;
    std::shared_ptr<TqTerminalLedger> Ledger_;
    std::atomic<bool> Pending_{false};
};

// callback-safe target adapter。Detach 从外部线程调用时等待已经进入的 callback；
// callback 内自删除 context 时只关闭 admission，由当前 callback guard 自然退出。
class TqStreamCallbackTarget final : public TqStreamLifetime::Target {
public:
    using CallbackFn = QUIC_STATUS (QUIC_API *)(
        MsQuicStream*, void*, QUIC_STREAM_EVENT*) noexcept;

    TqStreamCallbackTarget(CallbackFn callback, void* context) noexcept;
    QUIC_STATUS OnStreamEvent(
        MsQuicStream* stream,
        QUIC_STREAM_EVENT* event,
        uint64_t routeGeneration) noexcept override;
    void Detach() noexcept;
#if defined(TQ_UNIT_TESTING)
    void* ContextForTest() const noexcept override;
#endif

private:
    mutable std::mutex Lock_;
    std::condition_variable Drained_;
    CallbackFn Callback_{nullptr};
    void* Context_{nullptr};
    uint32_t ActiveCalls_{0};
    bool Accepting_{true};
};
