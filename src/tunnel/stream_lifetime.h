#pragma once

#include <msquic.hpp>

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <condition_variable>
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
    };

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

    static std::shared_ptr<TqStreamLifetime> OpenOutgoing(
        const MsQuicConnection& connection,
        QUIC_STREAM_OPEN_FLAGS flags,
        std::shared_ptr<Target> initialTarget) noexcept;

    static std::shared_ptr<TqStreamLifetime> AdoptAccepted(
        HQUIC rawStream,
        std::shared_ptr<Target> initialTarget) noexcept;

#if defined(TQ_UNIT_TESTING)
    // 状态机测试不依赖 MsQuic runtime；生产 factory 始终提供真实 wrapper。
    static std::shared_ptr<TqStreamLifetime> CreateForTest(
        Phase phase,
        std::shared_ptr<Target> initialTarget = nullptr);
    bool InstallStreamForTest(MsQuicStream* stream) noexcept;
    void ReleaseStreamForTest() noexcept;
    void* TargetContextForTest() const noexcept;
    static void SetFailNextRegisterSendCompletionForTest(bool fail) noexcept;
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
    QUIC_STATUS RequestShutdown(ShutdownIntent intent, uint64_t errorCode = 0) noexcept;

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
    QUIC_STATUS DispatchBufferedEvent(QUIC_STREAM_EVENT* event) noexcept;

private:
    TqStreamLifetime(Phase phase, std::shared_ptr<Target> initialTarget) noexcept;
    bool InstallStream(MsQuicStream* stream) noexcept;
    void RetainUntilTerminal();
    void ReleaseTerminalRetention();
    QUIC_STATUS Dispatch(MsQuicStream* stream, QUIC_STREAM_EVENT* event) noexcept;

    mutable std::mutex ControlMutex_;
    Phase Phase_;
    MsQuicStream* Stream_{nullptr};
    std::shared_ptr<Target> Target_;
    uint64_t RouteGeneration_{1};
    QUIC_STATUS StartFailureStatus_{QUIC_STATUS_SUCCESS};
    bool TerminalRetained_{false};
    uint8_t DesiredSendShutdown_{0}; // 0=open, 1=graceful, 2=abort
    uint8_t SubmittedSendShutdown_{0};
    uint8_t ReservedSendShutdown_{0};
    bool DesiredReceiveAbort_{false};
    bool SubmittedReceiveAbort_{false};
    bool ReservedReceiveAbort_{false};
    std::vector<std::unique_ptr<uint64_t>> SendKeyEnvelopes_;
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
