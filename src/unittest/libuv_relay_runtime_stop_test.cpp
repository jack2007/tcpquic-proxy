#include "libuv_relay_worker.h"
#include "libuv_allocator.h"
#include <atomic>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <type_traits>

static_assert(std::is_same_v<
    decltype(TqUvCallAdapter::ReadStart),
    int (*)(uv_stream_t*, uv_alloc_cb, uv_read_cb)>);
static_assert(std::is_same_v<
    decltype(TqUvCallAdapter::Write),
    int (*)(uv_write_t*, uv_stream_t*, const uv_buf_t[], unsigned int, uv_write_cb)>);
static_assert(std::is_same_v<
    decltype(TqUvCallAdapter::Close), void (*)(uv_handle_t*, uv_close_cb)>);

namespace {

std::atomic<uint32_t>* gHookStateForRuntimeStopDiag = nullptr;
bool gHookPrintForRuntimeStopDiag = false;

void RuntimeStopTerminalHook(
    TqUvRelayState& relay,
    TqUvTerminalTrigger trigger) noexcept {
    if (gHookStateForRuntimeStopDiag != nullptr) {
        gHookStateForRuntimeStopDiag->fetch_or(
            1u << static_cast<uint32_t>(trigger),
            std::memory_order_relaxed);
    }
    if (gHookPrintForRuntimeStopDiag) {
        std::fprintf(
            stderr,
            "[runtime-stop-test hook] terminal-start trigger=%u relay=%llu started=%d aborted=%d mask=%08x\n",
            static_cast<unsigned>(static_cast<uint32_t>(trigger)),
            static_cast<unsigned long long>(relay.RelayId),
            relay.TerminalStarted,
            relay.TerminalAborted,
            relay.TerminalTriggerMask.load(std::memory_order_acquire));
    }
    (void)relay;
}

void RuntimeStopConvergenceHook(TqUvRelayState& relay) noexcept {
    if (gHookStateForRuntimeStopDiag != nullptr && relay.TerminalStarted) {
        gHookStateForRuntimeStopDiag->fetch_or(
            1u << 31,
            std::memory_order_relaxed);
    }
}

void CheckAt(bool condition, int line) {
    if (!condition) {
        std::fprintf(stderr, "libuv runtime stop check failed at line %d\n", line);
        std::abort();
    }
}

#define Check(condition) CheckAt((condition), __LINE__)

std::atomic<uv_tcp_t*> gTrackedTcp{nullptr};

class EmptyTarget final : public TqStreamLifetime::Target {
public:
    QUIC_STATUS OnStreamEvent(
        MsQuicStream*,
        QUIC_STREAM_EVENT*,
        std::uint64_t) noexcept override {
        return QUIC_STATUS_SUCCESS;
    }
};

int FakeTcpInit(uv_loop_t* loop, uv_tcp_t* tcp) {
    (void)loop;
    gTrackedTcp.store(tcp, std::memory_order_release);
    return 0;
}

int FakeTcpOpen(uv_tcp_t*, uv_os_sock_t) { return 0; }
int FakeReadStart(uv_stream_t*, uv_alloc_cb, uv_read_cb) { return 0; }
int FakeReadStop(uv_stream_t*) { return 0; }
int FakeWrite(
    uv_write_t*,
    uv_stream_t*,
    const uv_buf_t*,
    unsigned int,
    uv_write_cb) {
    return 0;
}

void FakeClose(uv_handle_t* handle, uv_close_cb callback) {
    if (handle == reinterpret_cast<uv_handle_t*>(
            gTrackedTcp.load(std::memory_order_acquire))) {
        if (callback != nullptr) {
            callback(handle);
        }
        return;
    }
    uv_close(handle, callback);
}

void FakeStreamClose(HQUIC) {
}

QUIC_STATUS QUIC_API FakeStreamShutdown(
    HQUIC,
    QUIC_STREAM_SHUTDOWN_FLAGS,
    QUIC_UINT62) {
    return QUIC_STATUS_SUCCESS;
}

struct RuntimeStopHarness {
    explicit RuntimeStopHarness(std::uint64_t streamHandle) {
        static QUIC_API_TABLE api{};
        api.StreamClose = &FakeStreamClose;
        api.StreamShutdown = &FakeStreamShutdown;
        MsQuic = reinterpret_cast<const MsQuicApi*>(&api);

        Target_ = std::make_shared<EmptyTarget>();
        Owner_ = TqStreamLifetime::CreateForTest(
            TqStreamLifetime::Phase::Started, Target_);
        Check(Owner_ != nullptr);

        Stream_ = reinterpret_cast<MsQuicStream*>(StreamBytes_.data());
        Stream_->Handle = reinterpret_cast<HQUIC>(streamHandle);
        Stream_->CleanUpMode = CleanUpManual;
        Stream_->Callback = MsQuicStream::NoOpCallback;
        Stream_->Context = nullptr;
        Check(Owner_->InstallDetachedStreamForTest(Stream_));

        Control_ = std::make_shared<TqRelayStopControl>();
        Check(Control_ != nullptr);
    }

    TqUvRelayRegistration Registration(std::uint64_t socket) const {
        TqUvRelayRegistration registration{};
        registration.TcpSocket = static_cast<TqSocketHandle>(socket);
        registration.Stream = Stream_;
        registration.StreamOwner = Owner_;
        registration.StopControl = Control_;
        registration.ControlGeneration = Control_->Generation;
        registration.PrecommitMaxPendingBytes = 4ull * 1024;
        registration.TcpReadChunkSize = 4096;
        registration.MaxPendingBufferBytes = 32768;
        registration.MaxBufferedQuicSendBytes = 16384;
        registration.ResumeBufferedQuicSendBytes = 8192;
        return registration;
    }

    std::shared_ptr<TqStreamLifetime> Owner_;
    std::shared_ptr<TqRelayStopControl> Control_;

private:
    std::shared_ptr<EmptyTarget> Target_;
    alignas(MsQuicStream) std::array<std::uint8_t, sizeof(MsQuicStream)> StreamBytes_{};
    MsQuicStream* Stream_{nullptr};
};

const TqUvCallAdapter& RuntimeCalls() {
    static const TqUvCallAdapter calls = [] {
        TqUvCallAdapter result = TqUvProductionCalls();
        result.TcpInit = &FakeTcpInit;
        result.TcpOpen = &FakeTcpOpen;
        result.ReadStart = &FakeReadStart;
        result.ReadStop = &FakeReadStop;
        result.Write = &FakeWrite;
        result.Close = &FakeClose;
        return result;
    }();
    return calls;
}

void ConfigureRuntimeForTest(const TqUvCallAdapter& calls) {
    static bool configured = false;
    if (configured) {
        return;
    }
    auto& runtime = TqUvRelayRuntime::Instance();
    runtime.SetAllocatorInstallerForTest(&TqUvInstallAllocator);
    runtime.SetCallAdapterForTest(&calls);
    configured = true;
}

int ParseIterations(int argc, char** argv) {
    int iterations = 1;
    for (int index = 1; index + 1 < argc; ++index) {
        if (std::strcmp(argv[index], "--iterations") == 0) {
            iterations = std::atoi(argv[index + 1]);
            if (iterations <= 0) {
                iterations = 1;
            }
            ++index;
        }
    }
    return iterations;
}

void TestRuntimeStopCanStartAgain() {
    const auto& calls = RuntimeCalls();
    auto& runtime = TqUvRelayRuntime::Instance();
    ConfigureRuntimeForTest(calls);

    TqTuningConfig tuning{};
    tuning.RelayWorkerCount = 1;
    Check(runtime.Start(tuning));
    Check(runtime.WorkerCountForTest() == 1);

    auto stopDeadline = std::chrono::steady_clock::now() +
        std::chrono::milliseconds(500);
    Check(runtime.Stop(stopDeadline));
    Check(runtime.WorkerCountForTest() == 0);
    Check(runtime.PickWorker() == nullptr);

    Check(runtime.Start(tuning));
    Check(runtime.WorkerCountForTest() == 1);
    Check(runtime.Stop(std::chrono::steady_clock::time_point::max()));
    Check(runtime.WorkerCountForTest() == 0);
}

void SetQuicShutdownObservedForTest(const std::shared_ptr<TqUvRelayState>& relay) {
    if (relay) {
        relay->QuicShutdownObserved.store(true, std::memory_order_release);
    }
}

void TestRuntimeStopEscalatesToAbortWhenDeadlineExpired() {
    const auto& calls = RuntimeCalls();
    auto& runtime = TqUvRelayRuntime::Instance();
    ConfigureRuntimeForTest(calls);

    TqTuningConfig tuning{};
    tuning.RelayWorkerCount = 1;
    Check(runtime.Start(tuning));
    auto* worker = runtime.PickWorker();
    Check(worker != nullptr);

    RuntimeStopHarness harness(0x7bde);
    const auto registered = worker->RegisterRelayWithId(
        harness.Registration(0x7bde));
    Check(registered.Ok);
    auto relay = worker->RelayForTest(registered.RelayId);
    Check(relay != nullptr);
    SetQuicShutdownObservedForTest(relay);

    const auto deadline = std::chrono::steady_clock::now() -
        std::chrono::milliseconds(1);
    Check(runtime.Stop(deadline));
    Check(relay != nullptr);
    Check(relay->TerminalStarted);
    Check(relay->TerminalAborted);
}

void TestRuntimeStopDrainsWithGraceDeadline() {
    const auto& calls = RuntimeCalls();
    auto& runtime = TqUvRelayRuntime::Instance();
    ConfigureRuntimeForTest(calls);

    TqTuningConfig tuning{};
    tuning.RelayWorkerCount = 1;
    Check(runtime.Start(tuning));
    auto* worker = runtime.PickWorker();
    Check(worker != nullptr);
    std::atomic<uint32_t> hookState{0};
    gHookPrintForRuntimeStopDiag = true;
    gHookStateForRuntimeStopDiag = &hookState;
    TqUvSetTerminalHookForTest(&RuntimeStopTerminalHook);
    TqUvSetConvergenceHookForTest(&RuntimeStopConvergenceHook);

    RuntimeStopHarness harness(0x7bdf);
    const auto registered = worker->RegisterRelayWithId(
        harness.Registration(0x7bdf));
    Check(registered.Ok);
    auto relay = worker->RelayForTest(registered.RelayId);
    Check(relay != nullptr);
    std::fprintf(
        stderr,
        "[runtime-stop-test] pre-stop: started=%d aborted=%d "
        "mask=%08x stop=%d relay_id=%llu\n",
        relay->TerminalStarted,
        relay->TerminalAborted,
        relay->TerminalTriggerMask.load(std::memory_order_acquire),
        relay->StopControl != nullptr
            ? relay->StopControl->Stop.load(std::memory_order_acquire)
            : false,
        static_cast<unsigned long long>(relay->RelayId));
    relay->QuicShutdownObserved.store(true, std::memory_order_release);

    Check(runtime.Stop(std::chrono::steady_clock::time_point::max()));
    std::fprintf(
        stderr,
        "[runtime-stop-test] graceful-stop: started=%d aborted=%d "
        "mask=%08x stop=%d relay_id=%llu relayobj=%llu hooks=%08x\n",
        relay->TerminalStarted,
        relay->TerminalAborted,
        relay->TerminalTriggerMask.load(std::memory_order_acquire),
        relay->StopControl != nullptr
            ? relay->StopControl->Stop.load(std::memory_order_acquire)
            : false,
        static_cast<unsigned long long>(registered.RelayId),
        static_cast<unsigned long long>(relay->RelayId),
        static_cast<unsigned>(hookState.load(std::memory_order_acquire)));
    Check(!relay->TerminalAborted);
    Check(relay->TerminalStarted);
    gHookPrintForRuntimeStopDiag = false;
    gHookStateForRuntimeStopDiag = nullptr;
    TqUvSetTerminalHookForTest(nullptr);
    TqUvSetConvergenceHookForTest(nullptr);
}

} // namespace

int main(int argc, char** argv) {
    const int iterations = ParseIterations(argc, argv);
    for (int iteration = 0; iteration < iterations; ++iteration) {
        (void)iteration;
        TestRuntimeStopCanStartAgain();
        TestRuntimeStopEscalatesToAbortWhenDeadlineExpired();
        TestRuntimeStopDrainsWithGraceDeadline();
    }
    return 0;
}
