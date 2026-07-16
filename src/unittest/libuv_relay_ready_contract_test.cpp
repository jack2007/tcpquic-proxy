#include "libuv_relay_worker.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <thread>

namespace {

void CheckAt(bool value, int line) {
    if (!value) {
        std::fprintf(stderr, "ready contract check failed at line %d\n", line);
        std::abort();
    }
}
#define Check(value) CheckAt((value), __LINE__)

std::shared_ptr<TqStreamLifetime> gOwner;
std::atomic<uv_tcp_t*> gTcp{nullptr};
std::atomic<uv_write_t*> gWrite{nullptr};
std::atomic<uv_write_cb> gWriteComplete{nullptr};
std::atomic<std::uint64_t> gReceiveCompleted{0};

void QUIC_API FakeSetCallbackHandler(HQUIC, void*, void*) {}
void QUIC_API FakeStreamClose(HQUIC) {}
void QUIC_API FakeReceiveComplete(HQUIC, std::uint64_t length) {
    gReceiveCompleted.fetch_add(length, std::memory_order_relaxed);
}

std::shared_ptr<TqStreamLifetime> StartedOwner() {
    static QUIC_API_TABLE api{};
    api.SetCallbackHandler = &FakeSetCallbackHandler;
    api.StreamClose = &FakeStreamClose;
    api.StreamReceiveComplete = &FakeReceiveComplete;
    MsQuic = reinterpret_cast<const MsQuicApi*>(&api);
    auto owner = TqStreamLifetime::AdoptAccepted(
        reinterpret_cast<HQUIC>(0x81),
        std::make_shared<TqStreamCallbackTarget>(nullptr, nullptr),
        TqTerminalIdentity{
            81, 82, 83, 1, TqTunnelRole::ClientOpen,
            TqRelayBackendType::LibuvWorker},
        5);
    owner->SetShutdownHookForTest(
        [](std::uint64_t, QUIC_STREAM_SHUTDOWN_FLAGS) {
            return QUIC_STATUS_PENDING;
        });
    return owner;
}

int TcpInit(uv_loop_t* loop, uv_tcp_t* tcp) {
    tcp->loop = loop;
    gTcp.store(tcp, std::memory_order_release);
    return 0;
}
int TcpOpen(uv_tcp_t*, uv_os_sock_t) { return 0; }
int ReadStop(uv_stream_t*) { return 0; }
int ReadStart(uv_stream_t*, uv_alloc_cb, uv_read_cb) {
    QUIC_BUFFER buffer{
        3, reinterpret_cast<std::uint8_t*>(const_cast<char*>("abc"))};
    QUIC_STREAM_EVENT event{};
    event.Type = QUIC_STREAM_EVENT_RECEIVE;
    event.RECEIVE.TotalBufferLength = 3;
    event.RECEIVE.BufferCount = 1;
    event.RECEIVE.Buffers = &buffer;
    auto* binding = static_cast<TqUvStreamBinding*>(
        gOwner->TargetContextForTest());
    Check(binding != nullptr);
    Check(binding->OnStreamEvent(
              reinterpret_cast<MsQuicStream*>(0x81),
              &event,
              binding->RouteGeneration) == QUIC_STATUS_PENDING);
    return 0;
}
int Write(
    uv_write_t* request,
    uv_stream_t*,
    const uv_buf_t[],
    unsigned int,
    uv_write_cb complete) {
    gWrite.store(request, std::memory_order_release);
    gWriteComplete.store(complete, std::memory_order_release);
    return 0;
}
void Close(uv_handle_t* handle, uv_close_cb complete) {
    if (handle == reinterpret_cast<uv_handle_t*>(
            gTcp.load(std::memory_order_acquire))) {
        complete(handle);
    } else {
        uv_close(handle, complete);
    }
}

void PreparedReceiveBecomesActiveWriteWhenReadinessIsEnabled() {
    auto calls = TqUvProductionCalls();
    calls.TcpInit = &TcpInit;
    calls.TcpOpen = &TcpOpen;
    calls.ReadStart = &ReadStart;
    calls.ReadStop = &ReadStop;
    calls.Write = &Write;
    calls.Close = &Close;
    TqUvRelayWorker worker(TqUvRelayWorkerConfig{.Calls = &calls});
    Check(worker.StartAndWaitReady());
    gOwner = StartedOwner();

    auto stop = std::make_shared<TqRelayStopControl>();
    TqUvRelayRegistration registration{};
    registration.TcpSocket = static_cast<TqSocketHandle>(81);
    registration.Stream = reinterpret_cast<MsQuicStream*>(0x81);
    registration.StreamOwner = gOwner;
    registration.StopControl = stop;
    registration.ControlGeneration = stop->Generation;
    registration.PrecommitMaxPendingBytes = 4096;
    registration.TcpReadChunkSize = 4096;
    registration.MaxPendingBufferBytes = 32768;
    registration.MaxBufferedQuicSendBytes = 16384;
    registration.ResumeBufferedQuicSendBytes = 8192;
    const auto registered = worker.RegisterRelayWithId(std::move(registration));
    Check(registered.Ok);
    auto relay = worker.RelayForTest(registered.RelayId);
    Check(relay != nullptr);
    Check(relay->Activation == TqUvActivation::Active);
    Check(relay->AdmittedQuicReceiveBytes.load(std::memory_order_acquire) == 3);
    Check(relay->QuicToTcpPressureBytes.load(std::memory_order_acquire) == 3);
    Check(gWrite.load(std::memory_order_acquire) != nullptr);

    std::atomic<bool> writeCompleted{false};
    Check(worker.Post([&](TqUvRelayWorker&) {
        gWriteComplete.load(std::memory_order_acquire)(
            gWrite.load(std::memory_order_acquire), 0);
        writeCompleted.store(true, std::memory_order_release);
    }));
    while (!writeCompleted.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }
    Check(gReceiveCompleted.load(std::memory_order_acquire) == 3);
    Check(relay->AdmittedQuicReceiveBytes.load(std::memory_order_acquire) == 0);
    Check(relay->QuicToTcpPressureBytes.load(std::memory_order_acquire) == 0);

    std::thread stopper([&] { Check(worker.StopForTest()); });
    const auto deadline = std::chrono::steady_clock::now() +
        std::chrono::seconds(5);
    while (relay->TerminalBeginCount.load(std::memory_order_acquire) == 0 &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::yield();
    }
    Check(relay->TerminalBeginCount.load(std::memory_order_acquire) == 1);
    QUIC_STREAM_EVENT terminal{};
    terminal.Type = QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE;
    Check(gOwner->DispatchForTest(&terminal) == QUIC_STATUS_SUCCESS);
    stopper.join();
}

} // namespace

int main() {
    PreparedReceiveBecomesActiveWriteWhenReadinessIsEnabled();
    return 0;
}
