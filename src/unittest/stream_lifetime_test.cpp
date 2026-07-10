#define TQ_UNIT_TESTING 1
#include "stream_lifetime.h"

#include <atomic>
#include <memory>
#include <condition_variable>
#include <mutex>
#include <thread>

const MsQuicApi* MsQuic = nullptr;

namespace {

void* g_handler = nullptr;
void* g_handlerContext = nullptr;
unsigned g_closeCount = 0;
unsigned g_shutdownCount = 0;
QUIC_STREAM_SHUTDOWN_FLAGS g_lastShutdownFlags = QUIC_STREAM_SHUTDOWN_FLAG_NONE;
bool g_shutdownFail = false;
std::mutex g_gateLock;
std::condition_variable g_gateCv;
bool g_gateEntered = false;
bool g_gateRelease = false;

QUIC_STATUS QUIC_API BlockingCallback(
    MsQuicStream*, void*, QUIC_STREAM_EVENT*) noexcept {
    std::unique_lock<std::mutex> guard(g_gateLock);
    g_gateEntered = true;
    g_gateCv.notify_all();
    g_gateCv.wait(guard, [] { return g_gateRelease; });
    return QUIC_STATUS_SUCCESS;
}

void QUIC_API FakeSetCallbackHandler(HQUIC, void* handler, void* context) {
    g_handler = handler;
    g_handlerContext = context;
}

void QUIC_API FakeStreamClose(HQUIC) {
    ++g_closeCount;
}

QUIC_STATUS QUIC_API FakeStreamShutdown(
    HQUIC, QUIC_STREAM_SHUTDOWN_FLAGS flags, QUIC_UINT62) {
    ++g_shutdownCount;
    g_lastShutdownFlags = flags;
    return g_shutdownFail ? QUIC_STATUS_INTERNAL_ERROR : QUIC_STATUS_SUCCESS;
}

bool DispatchAdapter(HQUIC handle, QUIC_STREAM_EVENT& event) {
    if (g_handler == nullptr) {
        return false;
    }
    const auto callback = reinterpret_cast<QUIC_STREAM_CALLBACK_HANDLER>(g_handler);
    return QUIC_SUCCEEDED(callback(handle, g_handlerContext, &event));
}


class CountingTarget final : public TqStreamLifetime::Target {
public:
    QUIC_STATUS OnStreamEvent(
        MsQuicStream*,
        QUIC_STREAM_EVENT* event,
        uint64_t generation) noexcept override {
        ++Calls;
        LastType = event->Type;
        LastGeneration = generation;
        return QUIC_STATUS_SUCCESS;
    }

    std::atomic<unsigned> Calls{0};
    QUIC_STREAM_EVENT_TYPE LastType{QUIC_STREAM_EVENT_START_COMPLETE};
    uint64_t LastGeneration{0};
};

} // namespace

int main() {
    {
        int context = 1;
        auto target = std::make_shared<TqStreamCallbackTarget>(BlockingCallback, &context);
        QUIC_STREAM_EVENT event{};
        event.Type = QUIC_STREAM_EVENT_RECEIVE;
        g_gateEntered = false;
        g_gateRelease = false;
        std::thread callback([&] { (void)target->OnStreamEvent(nullptr, &event, 1); });
        {
            std::unique_lock<std::mutex> guard(g_gateLock);
            g_gateCv.wait(guard, [] { return g_gateEntered; });
        }
        std::atomic<bool> detached{false};
        std::thread detach([&] {
            target->Detach();
            detached.store(true, std::memory_order_release);
        });
        std::this_thread::yield();
        if (detached.load(std::memory_order_acquire)) return 32;
        {
            std::lock_guard<std::mutex> guard(g_gateLock);
            g_gateRelease = true;
        }
        g_gateCv.notify_all();
        callback.join();
        detach.join();
        if (!detached.load(std::memory_order_acquire)) return 33;
        if (target->OnStreamEvent(nullptr, &event, 1) != QUIC_STATUS_SUCCESS) return 34;
    }

    {
        auto target = std::make_shared<CountingTarget>();
        auto owner = TqStreamLifetime::CreateForTest(
            TqStreamLifetime::Phase::Started, target);
        const auto diagnosticsBefore = TqStreamLifetime::SnapshotSendCompletions();
        unsigned cleanups = 0;
        void* first = owner->RegisterSendCompletion(nullptr, [&] { ++cleanups; });
        if (first == nullptr) return 35;
        QUIC_STREAM_EVENT complete{};
        complete.Type = QUIC_STREAM_EVENT_SEND_COMPLETE;
        complete.SEND_COMPLETE.ClientContext = first;
        (void)owner->DispatchForTest(&complete);
        (void)owner->DispatchForTest(&complete);
        if (cleanups != 1 || target->Calls != 1) return 36;
        if (TqStreamLifetime::SnapshotSendCompletions().DuplicateClaims !=
            diagnosticsBefore.DuplicateClaims + 1) return 50;
        void* second = owner->RegisterSendCompletion(nullptr, [&] { ++cleanups; });
        if (second == nullptr || second == first) return 37;
        int unknown = 0;
        complete.SEND_COMPLETE.ClientContext = &unknown;
        (void)owner->DispatchForTest(&complete);
        if (cleanups != 1) return 38;
        if (TqStreamLifetime::SnapshotSendCompletions().UnknownClaims !=
            diagnosticsBefore.UnknownClaims + 1) return 51;
        QUIC_STREAM_EVENT terminal{};
        terminal.Type = QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE;
        (void)owner->DispatchForTest(&terminal);
        complete.SEND_COMPLETE.ClientContext = second;
        (void)owner->DispatchForTest(&complete);
        if (cleanups != 2) return 39;
    }

    {
        // Reservation RAII: armed destructor cancels; Dismiss keeps registry entry.
        auto target = std::make_shared<CountingTarget>();
        auto owner = TqStreamLifetime::CreateForTest(
            TqStreamLifetime::Phase::Started, target);
        const auto baseline = TqStreamLifetime::SnapshotSendCompletions();
        unsigned cleanups = 0;
        {
            auto reservation = owner->ReserveSendCompletion(nullptr, [&] { ++cleanups; });
            if (!reservation) return 40;
            if (TqStreamLifetime::SnapshotSendCompletions().ActiveCount !=
                baseline.ActiveCount + 1) {
                return 41;
            }
        }
        if (cleanups != 1) return 42;
        if (TqStreamLifetime::SnapshotSendCompletions().ActiveCount != baseline.ActiveCount) {
            return 43;
        }

        auto kept = owner->ReserveSendCompletion(nullptr, [&] { ++cleanups; });
        if (!kept) return 44;
        void* key = kept.Key();
        kept.Dismiss();
        if (TqStreamLifetime::SnapshotSendCompletions().ActiveCount !=
            baseline.ActiveCount + 1) {
            return 45;
        }
        if (!owner->CancelSendCompletion(key)) return 46;
        if (cleanups != 2) return 47;
        if (TqStreamLifetime::SnapshotSendCompletions().ActiveCount != baseline.ActiveCount) {
            return 48;
        }
        if (TqStreamLifetime::SnapshotSendCompletions().OldestAgeMs != 0 &&
            TqStreamLifetime::SnapshotSendCompletions().ActiveCount != 0) {
            return 49;
        }
    }

    {
        QUIC_API_TABLE fakeApi{};
        fakeApi.SetCallbackHandler = FakeSetCallbackHandler;
        fakeApi.StreamClose = FakeStreamClose;
        fakeApi.StreamShutdown = FakeStreamShutdown;
        MsQuic = reinterpret_cast<const MsQuicApi*>(&fakeApi);
        g_handler = nullptr;
        g_handlerContext = nullptr;
        g_closeCount = 0;
        g_shutdownCount = 0;

        const HQUIC raw = reinterpret_cast<HQUIC>(static_cast<uintptr_t>(1));
        auto target = std::make_shared<CountingTarget>();
        auto owner = TqStreamLifetime::AdoptAccepted(raw, target);
        if (!owner) return 25;
        if (owner->StreamForInitialization()->CleanUpMode != CleanUpManual) return 26;
        if (QUIC_FAILED(owner->RequestShutdown(
                TqStreamLifetime::ShutdownIntent::GracefulSend))) return 43;
        if (QUIC_FAILED(owner->RequestShutdown(
                TqStreamLifetime::ShutdownIntent::GracefulSend))) return 44;
        if (g_shutdownCount != 1) return 45;
        if (QUIC_FAILED(owner->RequestShutdown(
                TqStreamLifetime::ShutdownIntent::AbortSend))) return 46;
        if (g_shutdownCount != 2 ||
            (g_lastShutdownFlags & QUIC_STREAM_SHUTDOWN_FLAG_ABORT_SEND) == 0) return 47;
        if (QUIC_FAILED(owner->RequestShutdown(
                TqStreamLifetime::ShutdownIntent::AbortReceive))) return 48;
        if (g_shutdownCount != 3) return 49;
        std::weak_ptr<TqStreamLifetime> weak = owner;
        owner.reset();
        if (weak.expired()) return 27;

        QUIC_STREAM_EVENT terminal{};
        terminal.Type = QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE;
        if (!DispatchAdapter(raw, terminal)) return 28;
        if (!weak.expired()) return 29;
        if (g_closeCount != 1) return 30;
        if (target->Calls != 1) return 31;

        const HQUIC rejected = reinterpret_cast<HQUIC>(static_cast<uintptr_t>(2));
        g_shutdownCount = 0;
        TqStreamLifetime::RejectAccepted(rejected);
        if (g_shutdownCount != 1) return 40;
        QUIC_STREAM_EVENT rejectedTerminal{};
        rejectedTerminal.Type = QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE;
        if (!DispatchAdapter(rejected, rejectedTerminal)) return 41;
        if (g_closeCount != 2) return 42;
        MsQuic = nullptr;
    }

    {
        auto first = std::make_shared<CountingTarget>();
        auto second = std::make_shared<CountingTarget>();
        auto owner = TqStreamLifetime::CreateForTest(
            TqStreamLifetime::Phase::CreatedNotStarted, first);
        if (!owner->BeginStart()) return 1;
        if (owner->GetPhase() != TqStreamLifetime::Phase::Starting) return 2;
        const uint64_t generation = owner->RouteGeneration();
        uint64_t published = 0;
        if (!owner->PublishTarget(generation, second, &published)) return 3;
        if (published == generation) return 4;
        if (owner->PublishTarget(generation, first)) return 5;

        QUIC_STREAM_EVENT start{};
        start.Type = QUIC_STREAM_EVENT_START_COMPLETE;
        start.START_COMPLETE.Status = QUIC_STATUS_SUCCESS;
        if (QUIC_FAILED(owner->DispatchForTest(&start))) return 6;
        if (owner->GetPhase() != TqStreamLifetime::Phase::Started) return 7;
        if (first->Calls != 0 || second->Calls != 1) return 8;
        if (second->LastGeneration != published) return 9;
    }

    {
        auto target = std::make_shared<CountingTarget>();
        auto owner = TqStreamLifetime::CreateForTest(
            TqStreamLifetime::Phase::Starting, target);
        std::weak_ptr<TqStreamLifetime> weak = owner;
        auto* callbackContext = owner.get();
        owner.reset();
        if (weak.expired()) return 10; // start/terminal retention

        QUIC_STREAM_EVENT terminal{};
        terminal.Type = QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE;
        if (QUIC_FAILED(TqStreamLifetime::Callback(nullptr, callbackContext, &terminal))) return 11;
        if (target->Calls != 1) return 12;
        if (!weak.expired()) return 13; // callback guard在返回后才释放最后owner
    }

    {
        auto target = std::make_shared<CountingTarget>();
        auto owner = TqStreamLifetime::CreateForTest(
            TqStreamLifetime::Phase::Started, target);
        // test owner没有wrapper，因此不能取得API lease；phase/route仍按terminal seal。
        QUIC_STREAM_EVENT terminal{};
        terminal.Type = QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE;
        if (QUIC_FAILED(owner->DispatchForTest(&terminal))) return 14;
        if (owner->GetPhase() != TqStreamLifetime::Phase::TerminalPublished) return 15;
        if (owner->PublishTarget(owner->RouteGeneration(), target)) return 16;
        if (owner->TryAcquireApi()) return 17;
        if (target->Calls != 1) return 18;
        if (QUIC_FAILED(owner->DispatchForTest(&terminal))) return 19;
        if (target->Calls != 1) return 20; // duplicate terminal once-only
    }

    {
        auto target = std::make_shared<CountingTarget>();
        auto owner = TqStreamLifetime::CreateForTest(
            TqStreamLifetime::Phase::Starting, target);
        QUIC_STREAM_EVENT failed{};
        failed.Type = QUIC_STREAM_EVENT_START_COMPLETE;
        failed.START_COMPLETE.Status = QUIC_STATUS_INTERNAL_ERROR;
        if (QUIC_FAILED(owner->DispatchForTest(&failed))) return 21;
        if (owner->GetPhase() != TqStreamLifetime::Phase::StartFailed) return 22;
        if (target->Calls != 1) return 23;
        if (owner->PublishTarget(owner->RouteGeneration(), target)) return 24;
        if (owner->TryAcquireApi()) return 50;
    }

    {
        auto target = std::make_shared<CountingTarget>();
        auto owner = TqStreamLifetime::CreateForTest(
            TqStreamLifetime::Phase::CreatedNotStarted, target);
        if (!owner->BeginStart()) return 51;
        QUIC_STREAM_EVENT earlyStart{};
        earlyStart.Type = QUIC_STREAM_EVENT_START_COMPLETE;
        earlyStart.START_COMPLETE.Status = QUIC_STATUS_SUCCESS;
        if (QUIC_FAILED(owner->DispatchForTest(&earlyStart))) return 52;
        if (owner->GetPhase() != TqStreamLifetime::Phase::Started) return 53;
        if (target->Calls != 1) return 54;
    }

    {
        auto target = std::make_shared<CountingTarget>();
        auto owner = TqStreamLifetime::CreateForTest(
            TqStreamLifetime::Phase::Starting, target);
        unsigned sendCleanups = 0;
        void* sendKey = owner->RegisterSendCompletion(
            reinterpret_cast<void*>(static_cast<uintptr_t>(99)),
            [&] { ++sendCleanups; });
        if (sendKey == nullptr) return 55;
        QUIC_STREAM_EVENT failedStart{};
        failedStart.Type = QUIC_STREAM_EVENT_START_COMPLETE;
        failedStart.START_COMPLETE.Status = QUIC_STATUS_INTERNAL_ERROR;
        if (QUIC_FAILED(owner->DispatchForTest(&failedStart))) return 56;
        if (owner->GetPhase() != TqStreamLifetime::Phase::StartFailed) return 57;
        if (sendCleanups != 0) return 58;
        if (owner->PublishTarget(owner->RouteGeneration(), target)) return 59;
        if (owner->TryAcquireApi()) return 60;
        QUIC_STREAM_EVENT canceledSend{};
        canceledSend.Type = QUIC_STREAM_EVENT_SEND_COMPLETE;
        canceledSend.SEND_COMPLETE.ClientContext = sendKey;
        canceledSend.SEND_COMPLETE.Canceled = TRUE;
        if (QUIC_FAILED(owner->DispatchForTest(&canceledSend))) return 61;
        if (sendCleanups != 1) return 62;
    }

    return 0;
}
